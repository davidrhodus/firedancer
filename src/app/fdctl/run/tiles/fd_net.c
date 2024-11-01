#include "../../../../disco/tiles.h"

/* The net tile translates between AF_XDP and fd_tango
   traffic.  It is responsible for setting up the XDP and
   XSK socket configuration.

   ### Why does this tile bind to loopback?
   
   The Linux kernel does some short circuiting optimizations
   when sending packets to an IP address that's owned by the
   same host. The optimization is basically to route them over
   to the loopback interface directly, bypassing the network
   hardware.

   This redirection to the loopback interface happens before
   XDP programs are executed, so local traffic destined for
   our listen addresses will not get ingested correctly.

   There are two reasons we send traffic locally,

   * For testing and development.
   * The Agave code sends local traffic to itself to
     as part of routine operation (eg, when it's the leader
     it sends votes to its own TPU socket).

   So for now we need to also bind to loopback. This is a
   small performance hit for other traffic, but we only
   redirect packets destined for our target IP and port so
   it will not otherwise interfere. Loopback only supports
   XDP in SKB mode. */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h> /* MSG_DONTWAIT needed before importing the net seccomp filter */
#include <linux/if_xdp.h>

#include "generated/net_seccomp.h"

#include "../../../../disco/metrics/fd_metrics.h"

#include "../../../../waltz/quic/fd_quic.h"
#include "../../../../waltz/xdp/fd_xdp.h"
#include "../../../../waltz/xdp/fd_xsk_aio_private.h"
#include "../../../../waltz/xdp/fd_xsk_private.h"
#include "../../../../util/net/fd_ip4.h"
#include "../../../../waltz/ip/fd_ip.h"

#include <unistd.h>
#include <linux/unistd.h>

#define MAX_NET_INS (32UL)

typedef struct {
  fd_wksp_t * mem;
  ulong       chunk0;
  ulong       wmark;
} fd_net_in_ctx_t;

typedef struct {
  fd_frag_meta_t * mcache;
  ulong *          sync;
  ulong            depth;
  ulong            seq;

  fd_wksp_t * mem;
  ulong       chunk0;
  ulong       wmark;
  ulong       chunk;
} fd_net_out_ctx_t;

typedef struct {
  fd_xsk_t * xsk;
  void *     xsk_aio;
  int        xsk_map_fd;
  int        xdp_prog_link_fd;

  fd_xsk_t * lo_xsk;
  void *     lo_xsk_aio;
  int        lo_xdp_prog_link_fd;

  fd_ip_t *  ip;
} fd_net_init_ctx_t;

typedef struct {
  fd_net_init_ctx_t init;

  ulong xsk_aio_cnt;
  fd_xsk_aio_t * xsk_aio[ 2 ];

  ulong round_robin_cnt;
  ulong round_robin_id;

  const fd_aio_t * tx;
  const fd_aio_t * lo_tx;

  uchar frame[ FD_NET_MTU ];

  uint   src_ip_addr;
  uchar  src_mac_addr[6];

  ushort shred_listen_port;
  ushort quic_transaction_listen_port;
  ushort legacy_transaction_listen_port;
  ushort gossip_listen_port;
  ushort repair_intake_listen_port;
  ushort repair_serve_listen_port;

  ulong in_cnt;
  fd_net_in_ctx_t in[ MAX_NET_INS ];

  fd_net_out_ctx_t quic_out[1];
  fd_net_out_ctx_t shred_out[1];
  fd_net_out_ctx_t gossip_out[1];
  fd_net_out_ctx_t repair_out[1];

  fd_ip_t *   ip;
  long        ip_next_upd;

  struct {
    ulong tx_dropped_cnt;
  } metrics;
} fd_net_ctx_t;

fd_net_init_ctx_t *
fd_net_init_ctx_init( fd_net_init_ctx_t * ctx ) {
  *ctx = (fd_net_init_ctx_t){
    .xdp_prog_link_fd    = -1,
    .lo_xdp_prog_link_fd = -1,
    .xsk_map_fd          = -1
  };
  return ctx;
}

/* Known port types
   These are IDs set by the XDP redirect program */

#define FDCTL_NET_BIND_TPU_USER_UDP  (0)
#define FDCTL_NET_BIND_TPU_USER_QUIC (1)
#define FDCTL_NET_BIND_SHRED         (2)
#define FDCTL_NET_BIND_GOSSIP        (3)
#define FDCTL_NET_BIND_REPAIR_IN     (4)
#define FDCTL_NET_BIND_REPAIR_SERVE  (5)
#define FDCTL_NET_BIND_MAX           (6)

FD_FN_CONST static inline ulong
scratch_align( void ) {
  return 4096UL;
}

FD_FN_PURE static inline ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  /* TODO reproducing this conditional memory layout twice is susceptible to bugs. Use more robust object discovery */
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(fd_net_ctx_t),      sizeof(fd_net_ctx_t) );
  l = FD_LAYOUT_APPEND( l, fd_aio_align(),             fd_aio_footprint() );
  if( tile->kind_id == 0 ) {
    l = FD_LAYOUT_APPEND( l, alignof(fd_xdp_session_t),      sizeof(fd_xdp_session_t)      );
    l = FD_LAYOUT_APPEND( l, alignof(fd_xdp_link_session_t), sizeof(fd_xdp_link_session_t) );
    l = FD_LAYOUT_APPEND( l, alignof(fd_xdp_link_session_t), sizeof(fd_xdp_link_session_t) );
  }
  l = FD_LAYOUT_APPEND( l, fd_xsk_align(),     fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) );
  l = FD_LAYOUT_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) );
  if( FD_UNLIKELY( strcmp( tile->net.interface, "lo" ) && tile->kind_id == 0 ) ) {
    l = FD_LAYOUT_APPEND( l, fd_xsk_align(),     fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) );
    l = FD_LAYOUT_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) );
  }
  l = FD_LAYOUT_APPEND( l, fd_ip_align(), fd_ip_footprint( 0U, 0U ) );
  return FD_LAYOUT_FINI( l, scratch_align() );
}

/* net_rx_aio_send is a callback invoked by aio when new data is
   received on an incoming xsk.  The xsk might be bound to any interface
   or ports, so the purpose of this callback is to determine if the
   packet might be a valid transaction, and whether it is QUIC or
   non-QUIC (raw UDP) before forwarding to the appropriate handler.

   This callback is supposed to return the number of packets in the
   batch which were successfully processed, but we always return
   batch_cnt since there is no logic in place to backpressure this far
   up the stack, and there is no sane way to "not handle" an incoming
   packet. */
static int
net_rx_aio_send( void *                    _ctx,
                 fd_aio_pkt_info_t const * batch,
                 ulong                     batch_cnt,
                 ulong *                   opt_batch_idx,
                 int                       flush ) {
  (void)flush;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  for( ulong i=0; i<batch_cnt; i++ ) {
    uchar const * packet = batch[i].buf;
    uchar const * packet_end = packet + batch[i].buf_sz;

    if( FD_UNLIKELY( batch[i].buf_sz > FD_NET_MTU ) )
      FD_LOG_ERR(( "received a UDP packet with a too large payload (%u)", batch[i].buf_sz ));

    uchar const * iphdr = packet + 14U;

    /* Filter for UDP/IPv4 packets. Test for ethtype and ipproto in 1
       branch */
    uint test_ethip = ( (uint)packet[12] << 16u ) | ( (uint)packet[13] << 8u ) | (uint)packet[23];
    if( FD_UNLIKELY( test_ethip!=0x080011 ) )
      FD_LOG_ERR(( "Firedancer received a packet from the XDP program that was either "
                   "not an IPv4 packet, or not a UDP packet. It is likely your XDP program "
                   "is not configured correctly." ));

    /* IPv4 is variable-length, so lookup IHL to find start of UDP */
    uint iplen = ( ( (uint)iphdr[0] ) & 0x0FU ) * 4U;
    uchar const * udp = iphdr + iplen;

    /* Ignore if UDP header is too short */
    if( FD_UNLIKELY( udp+8U > packet_end ) ) continue;

    /* Extract IP dest addr and UDP src/dest port */
    uint ip_srcaddr    =                  *(uint   *)( iphdr+12UL );
    ushort udp_srcport = fd_ushort_bswap( *(ushort *)( udp+0UL    ) );
    ushort udp_dstport = fd_ushort_bswap( *(ushort *)( udp+2UL    ) );

    ushort proto;
    fd_net_out_ctx_t * out;
    if(      FD_UNLIKELY( udp_dstport==ctx->shred_listen_port ) ) {
      proto = DST_PROTO_SHRED;
      out = ctx->shred_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->quic_transaction_listen_port ) ) {
      proto = DST_PROTO_TPU_QUIC;
      out = ctx->quic_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->legacy_transaction_listen_port ) ) {
      proto = DST_PROTO_TPU_UDP;
      out = ctx->quic_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->gossip_listen_port ) ) {
      proto = DST_PROTO_GOSSIP;
      out = ctx->gossip_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->repair_intake_listen_port ) ) {
      proto = DST_PROTO_REPAIR;
      out = ctx->repair_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->repair_serve_listen_port ) ) {
      proto = DST_PROTO_REPAIR;
      out = ctx->repair_out;
    } else {
      
      FD_LOG_ERR(( "Firedancer received a UDP packet on port %hu which was not expected. "
                   "Only the following ports should be configured to forward packets: "
                   "%hu, %hu, %hu, %hu, %hu, %hu (excluding any 0 ports, which can be ignored)."
                   "It is likely you changed the port configuration in your TOML file and "
                   "did not reload the XDP program. You can reload the program by running "
                   "`fdctl configure fini xdp && fdctl configure init xdp`.",
                   udp_dstport,
                   ctx->shred_listen_port,
                   ctx->quic_transaction_listen_port,
                   ctx->legacy_transaction_listen_port,
                   ctx->gossip_listen_port,
                   ctx->repair_intake_listen_port,
                   ctx->repair_serve_listen_port ));
    }

    fd_memcpy( fd_chunk_to_laddr( out->mem, out->chunk ), packet, batch[ i ].buf_sz );

    /* tile can decide how to partition based on src ip addr and src port */
    ulong sig = fd_disco_netmux_sig( ip_srcaddr, udp_srcport, 0U, proto, 14UL+8UL+iplen );

    ulong tspub  = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );
    fd_mcache_publish( out->mcache, out->depth, out->seq, sig, out->chunk, batch[ i ].buf_sz, 0, 0, tspub );

    out->seq = fd_seq_inc( out->seq, 1UL );
    out->chunk = fd_dcache_compact_next( out->chunk, FD_NET_MTU, out->chunk0, out->wmark );
  }

  if( FD_LIKELY( opt_batch_idx ) ) {
    *opt_batch_idx = batch_cnt;
  }

  return FD_AIO_SUCCESS;
}

static void
metrics_write( fd_net_ctx_t * ctx ) {
  ulong rx_cnt = ctx->xsk_aio[ 0 ]->metrics.rx_cnt;
  ulong rx_sz  = ctx->xsk_aio[ 0 ]->metrics.rx_sz;
  ulong tx_cnt = ctx->xsk_aio[ 0 ]->metrics.tx_cnt;
  ulong tx_sz  = ctx->xsk_aio[ 0 ]->metrics.tx_sz;
  if( FD_LIKELY( ctx->xsk_aio[ 1 ] ) ) {
    rx_cnt += ctx->xsk_aio[ 1 ]->metrics.rx_cnt;
    rx_sz  += ctx->xsk_aio[ 1 ]->metrics.rx_sz;
    tx_cnt += ctx->xsk_aio[ 1 ]->metrics.tx_cnt;
    tx_sz  += ctx->xsk_aio[ 1 ]->metrics.tx_sz;
  }

  FD_MCNT_SET( NET_TILE, RECEIVED_PACKETS, rx_cnt );
  FD_MCNT_SET( NET_TILE, RECEIVED_BYTES,   rx_sz  );
  FD_MCNT_SET( NET_TILE, SENT_PACKETS,     tx_cnt );
  FD_MCNT_SET( NET_TILE, SENT_BYTES,       tx_sz  );

  FD_MCNT_SET( NET_TILE, TX_DROPPED, ctx->metrics.tx_dropped_cnt );
}

static void
before_credit( fd_net_ctx_t *      ctx,
               fd_stem_context_t * stem,
               int *               charge_busy ) {
  (void)stem;

  for( ulong i=0; i<ctx->xsk_aio_cnt; i++ ) {
    if( FD_LIKELY( fd_xsk_aio_service( ctx->xsk_aio[i] ) ) ) {
      *charge_busy = 1;
    }
  }
}

struct xdp_statistics_v0 {
  __u64 rx_dropped; /* Dropped for other reasons */
  __u64 rx_invalid_descs; /* Dropped due to invalid descriptor */
  __u64 tx_invalid_descs; /* Dropped due to invalid descriptor */
};

struct xdp_statistics_v1 {
  __u64 rx_dropped; /* Dropped for other reasons */
  __u64 rx_invalid_descs; /* Dropped due to invalid descriptor */
  __u64 tx_invalid_descs; /* Dropped due to invalid descriptor */
  __u64 rx_ring_full; /* Dropped due to rx ring being full */
  __u64 rx_fill_ring_empty_descs; /* Failed to retrieve item from fill ring */
  __u64 tx_ring_empty_descs; /* Failed to retrieve item from tx ring */
};

static inline void
poll_xdp_statistics( fd_net_ctx_t * ctx ) {
  struct xdp_statistics_v1 stats;
  uint optlen = (uint)sizeof(stats);
  if( FD_UNLIKELY( -1==getsockopt( ctx->init.xsk->xsk_fd, SOL_XDP, XDP_STATISTICS, &stats, &optlen ) ) )
    FD_LOG_ERR(( "getsockopt(SOL_XDP, XDP_STATISTICS) failed: %s", strerror( errno ) ));

  if( FD_LIKELY( optlen==sizeof(struct xdp_statistics_v1) ) ) {
    FD_MCNT_SET( NET_TILE, XDP_RX_DROPPED_OTHER, stats.rx_dropped );
    FD_MCNT_SET( NET_TILE, XDP_RX_DROPPED_RING_FULL, stats.rx_ring_full );

    FD_TEST( !stats.rx_invalid_descs );
    FD_TEST( !stats.tx_invalid_descs );
    /* TODO: We shouldn't ever try to tx or rx with empty descs but we
             seem to sometimes. */
    // FD_TEST( !stats.rx_fill_ring_empty_descs );
    // FD_TEST( !stats.tx_ring_empty_descs );
  } else if( FD_LIKELY( optlen==sizeof(struct xdp_statistics_v0) ) ) {
    FD_MCNT_SET( NET_TILE, XDP_RX_DROPPED_OTHER, stats.rx_dropped );

    FD_TEST( !stats.rx_invalid_descs );
    FD_TEST( !stats.tx_invalid_descs );
  } else {
    FD_LOG_ERR(( "getsockopt(SOL_XDP, XDP_STATISTICS) returned unexpected size %u", optlen ));
  }
}

static void
during_housekeeping( fd_net_ctx_t * ctx ) {
  long now = fd_log_wallclock();
  if( FD_UNLIKELY( now > ctx->ip_next_upd ) ) {
    ctx->ip_next_upd = now + (long)60e9;
    fd_ip_arp_fetch( ctx->ip );
    fd_ip_route_fetch( ctx->ip );
  }

  /* Only net tile 0 polls the statistics, as they are retrieved for the
     XDP socket which is shared across all net tiles. */

  if( FD_LIKELY( !ctx->round_robin_id ) ) poll_xdp_statistics( ctx );
}

FD_FN_PURE static int
route_loopback( uint  tile_ip_addr,
                ulong sig ) {
  return fd_disco_netmux_sig_dst_ip( sig )==FD_IP4_ADDR(127,0,0,1) ||
    fd_disco_netmux_sig_dst_ip( sig )==tile_ip_addr;
}

static inline int
before_frag( fd_net_ctx_t * ctx,
             ulong          in_idx,
             ulong          seq,
             ulong          sig ) {
  (void)in_idx;

  ulong proto = fd_disco_netmux_sig_proto( sig );
  if( FD_UNLIKELY( proto!=DST_PROTO_OUTGOING ) ) return 1;

  /* Round robin by sequence number for now, QUIC should be modified to
     echo the net tile index back so we can transmit on the same queue.

     127.0.0.1 packets for localhost must go out on net tile 0 which
     owns the loopback interface XSK, which only has 1 queue. */

  if( FD_UNLIKELY( route_loopback( ctx->src_ip_addr, sig ) ) ) return ctx->round_robin_id != 0UL;
  else                                                         return (seq % ctx->round_robin_cnt) != ctx->round_robin_id;
}

static inline void
during_frag( fd_net_ctx_t * ctx,
             ulong          in_idx,
             ulong          seq,
             ulong          sig,
             ulong          chunk,
             ulong          sz ) {
  (void)in_idx;
  (void)seq;
  (void)sig;

  if( FD_UNLIKELY( chunk<ctx->in[ in_idx ].chunk0 || chunk>ctx->in[ in_idx ].wmark || sz>FD_NET_MTU ) )
    FD_LOG_ERR(( "chunk %lu %lu corrupt, not in range [%lu,%lu]", chunk, sz, ctx->in[ in_idx ].chunk0, ctx->in[ in_idx ].wmark ));

  uchar * src = (uchar *)fd_chunk_to_laddr( ctx->in[ in_idx ].mem, chunk );
  fd_memcpy( ctx->frame, src, sz ); // TODO: Change xsk_aio interface to eliminate this copy
}

static void
send_arp_probe( fd_net_ctx_t * ctx,
                uint           dst_ip_addr,
                uint           ifindex ) {
  uchar          arp_buf[FD_IP_ARP_SZ];
  ulong          arp_len = 0UL;

  uint           src_ip_addr  = ctx->src_ip_addr;
  uchar *        src_mac_addr = ctx->src_mac_addr;

  /* prepare arp table */
  int arp_table_rtn = fd_ip_update_arp_table( ctx->ip, dst_ip_addr, ifindex );

  if( FD_UNLIKELY( arp_table_rtn == FD_IP_SUCCESS ) ) {
    /* generate a probe */
    fd_ip_arp_gen_arp_probe( arp_buf, FD_IP_ARP_SZ, &arp_len, dst_ip_addr, fd_uint_bswap( src_ip_addr ), src_mac_addr );

    /* send the probe */
    fd_aio_pkt_info_t aio_buf = { .buf = arp_buf, .buf_sz = (ushort)arp_len };
    ulong sent_cnt;
    ctx->tx->send_func( ctx->xsk_aio[ 0 ], &aio_buf, 1, &sent_cnt, 1 );
    ctx->metrics.tx_dropped_cnt += 1UL-sent_cnt;
  }
}

static void
after_frag( fd_net_ctx_t *      ctx,
            ulong               in_idx,
            ulong               seq,
            ulong               sig,
            ulong               chunk,
            ulong               sz,
            ulong               tsorig,
            fd_stem_context_t * stem ) {
  (void)in_idx;
  (void)seq;
  (void)sig;
  (void)chunk;
  (void)tsorig;
  (void)stem;

  fd_aio_pkt_info_t aio_buf = { .buf = ctx->frame, .buf_sz = (ushort)sz };
  if( FD_UNLIKELY( route_loopback( ctx->src_ip_addr, sig ) ) ) {
    ulong sent_cnt;
    ctx->lo_tx->send_func( ctx->xsk_aio[ 1 ], &aio_buf, 1, &sent_cnt, 1 );
    ctx->metrics.tx_dropped_cnt += 1UL-sent_cnt;
  } else {
    /* extract dst ip */
    uint dst_ip = fd_uint_bswap( fd_disco_netmux_sig_dst_ip( sig ) );

    uint  next_hop    = 0U;
    uchar dst_mac[6]  = {0};
    uint  if_idx      = 0;

    /* route the packet */
    /*
     * determine the destination:
     *   same host
     *   same subnet
     *   other
     * determine the next hop
     *   localhost
     *   gateway
     *   subnet local host
     * determine the mac address of the next hop address
     *   and the local ipv4 and eth addresses */
    int rtn = fd_ip_route_ip_addr( dst_mac, &next_hop, &if_idx, ctx->ip, dst_ip );
    if( FD_UNLIKELY( rtn == FD_IP_PROBE_RQD ) ) {
      /* another fd_net instance might have already resolved this address
         so simply try another fetch */
      fd_ip_arp_fetch( ctx->ip );
      rtn = fd_ip_route_ip_addr( dst_mac, &next_hop, &if_idx, ctx->ip, dst_ip );
    }

    long now;
    switch( rtn ) {
      case FD_IP_PROBE_RQD:
        /* TODO possibly buffer some data while waiting for ARPs to complete */
        /* TODO rate limit ARPs */
        /* TODO add caching of ip_dst -> routing info */
        send_arp_probe( ctx, next_hop, if_idx );

        /* refresh tables */
        now = fd_log_wallclock();
        ctx->ip_next_upd = now + (long)200e3;
        break;
      case FD_IP_NO_ROUTE:
        /* cannot make progress here */
        break;
      case FD_IP_SUCCESS:
        /* set destination mac address */
        memcpy( ctx->frame, dst_mac, 6UL );

        /* set source mac address */
        memcpy( ctx->frame + 6UL, ctx->src_mac_addr, 6UL );

        ulong sent_cnt;
        ctx->tx->send_func( ctx->xsk_aio[ 0 ], &aio_buf, 1, &sent_cnt, 1 );
        ctx->metrics.tx_dropped_cnt += 1UL-sent_cnt;
        break;
      case FD_IP_RETRY:
        /* refresh tables */
        now = fd_log_wallclock();
        ctx->ip_next_upd = now + (long)200e3;
        /* TODO consider buffering */
        break;
      case FD_IP_MULTICAST:
      case FD_IP_BROADCAST:
      default:
        /* should not occur in current use cases */
        break;
    }
  }
}

/* init_link_session is part of privileged_init.  It only runs on net
   tile 0.  This function does shared pre-configuration used by all 
   other net tiles.  This includes installing the XDP program and 
   setting up the XSKMAP into which the other net tiles can register
   themselves into.
   
   session, link_session, lo_session get initialized with session
   objects.  tile points to the net tile's config.  if_idx, lo_idx
   locate the device IDs of the main and loopback interface. 
   *xsk_map_fd, *lo_xsk_map_fd are set to the newly created XSKMAP file
   descriptors.
   
   Note that if the main interface is loopback, then the loopback-
   related structures are uninitialized.
   
   Kernel object references:
     
     BPF_LINK file descriptor
      |
      +-> XDP program installation on NIC
      |    |
      |    +-> XDP program <-- BPF_PROG file descriptor (prog_fd)
      |
      +-> XSKMAP object <-- BPF_MAP file descriptor (xsk_map)
      |
      +-> BPF_MAP object <-- BPF_MAP file descriptor (udp_dsts) */

static void
init_link_session( fd_xdp_session_t *      session,
                   fd_xdp_link_session_t * link_session,
                   fd_xdp_link_session_t * lo_session,
                   fd_topo_tile_t const *  tile,
                   uint                    if_idx,
                   uint                    lo_idx,
                   fd_net_init_ctx_t *     init_ctx,
                   int *                   lo_xsk_map_fd ) { 
    
  /* Set up port redirection map */

  if( FD_UNLIKELY( !fd_xdp_session_init( session ) ) ) {
    FD_LOG_ERR(( "fd_xdp_session_init failed" ));
  }
  
  ushort udp_port_candidates[ FDCTL_NET_BIND_MAX ] = { 
    [ FDCTL_NET_BIND_TPU_USER_UDP  ] = (ushort)tile->net.legacy_transaction_listen_port,
    [ FDCTL_NET_BIND_TPU_USER_QUIC ] = (ushort)tile->net.quic_transaction_listen_port,
    [ FDCTL_NET_BIND_SHRED         ] = (ushort)tile->net.shred_listen_port,
    [ FDCTL_NET_BIND_GOSSIP        ] = (ushort)tile->net.gossip_listen_port,
    [ FDCTL_NET_BIND_REPAIR_IN     ] = (ushort)tile->net.repair_intake_listen_port,
    [ FDCTL_NET_BIND_REPAIR_SERVE  ] = (ushort)tile->net.repair_serve_listen_port,
  };
  for( ulong bind_id=0UL; bind_id<FDCTL_NET_BIND_MAX; bind_id++ ) {
    ushort port = (ushort)udp_port_candidates[bind_id];
    if( FD_UNLIKELY( !port ) ) continue;  /* port 0 implies drop */
    if( FD_UNLIKELY( fd_xdp_listen_udp_port(
        session,
        tile->net.src_ip_addr,
        port,
        (uint)bind_id ) ) ) {
      FD_LOG_ERR(( "fd_xdp_listen_udp_port failed" ));
    }
  }

  /* Install XDP programs to network devices */

  uint xdp_mode = 0;
  if(      FD_LIKELY( !strcmp( tile->net.xdp_mode, "skb" ) ) ) xdp_mode = XDP_FLAGS_SKB_MODE;
  else if( FD_LIKELY( !strcmp( tile->net.xdp_mode, "drv" ) ) ) xdp_mode = XDP_FLAGS_DRV_MODE;
  else if( FD_LIKELY( !strcmp( tile->net.xdp_mode, "hw"  ) ) ) xdp_mode = XDP_FLAGS_HW_MODE;
  else FD_LOG_ERR(( "unknown XDP mode `%.4s`", tile->net.xdp_mode ));

  if( FD_UNLIKELY( !fd_xdp_link_session_init( link_session, session, if_idx, xdp_mode ) ) ) {
    FD_LOG_ERR(( "fd_xdp_link_session_init failed" ));
  }
  FD_TEST( 0==close( link_session->prog_fd ) );

  init_ctx->xdp_prog_link_fd = link_session->prog_link_fd;
  init_ctx->xsk_map_fd       = link_session->xsk_map_fd;

  if( 0!=strcmp( tile->net.interface, "lo" ) ) {
    if( FD_UNLIKELY( !fd_xdp_link_session_init( lo_session, session, lo_idx, XDP_FLAGS_SKB_MODE ) ) ) {
      FD_LOG_ERR(( "fd_xdp_link_session_init failed" ));
    }
    FD_TEST( 0==close( lo_session->prog_fd ) );
    
    init_ctx->lo_xdp_prog_link_fd = lo_session->prog_link_fd;
    *lo_xsk_map_fd                = lo_session->xsk_map_fd;
  }
  
  FD_TEST( 0==close( session->udp_dsts_map_fd ) );
  
}

typedef union {
  struct {
    int pid;
    int xskmap_fd;
  };
  ulong ul;
} fd_net0_tile_args_t;

FD_STATIC_ASSERT( sizeof(fd_net0_tile_args_t)==sizeof(ulong), align );

static void
privileged_init( fd_topo_t *      topo,
                 fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );

  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_net_ctx_t), sizeof(fd_net_ctx_t) );
                       FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(),        fd_aio_footprint()   );

  fd_net_init_ctx_t * init_ctx = fd_net_init_ctx_init( &ctx->init );

  uint if_idx = if_nametoindex( tile->net.interface );
  if( FD_UNLIKELY( !if_idx ) ) FD_LOG_ERR(( "if_nametoindex(%s) failed", tile->net.interface ));

  uint lo_idx = if_nametoindex( "lo" );
  if( FD_UNLIKELY( !lo_idx ) ) FD_LOG_ERR(( "if_nametoindex(lo) failed" ));

  ulong            p_net0_pid_id = fd_pod_query_ulong( topo->props, "net0_pid", ULONG_MAX ); FD_TEST( p_net0_pid_id!=ULONG_MAX );
  ulong volatile * p_net0_line   = fd_fseq_app_laddr( fd_fseq_join( fd_topo_obj_laddr( topo, p_net0_pid_id ) ) );

  int lo_xsk_map_fd = -1;

  if( tile->kind_id == 0 ) {

    /* We are net tile 0.  Do link-wide initialization */

    fd_xdp_session_t *      session      = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_xdp_session_t),      sizeof(fd_xdp_session_t)      );
    fd_xdp_link_session_t * link_session = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_xdp_link_session_t), sizeof(fd_xdp_link_session_t) );
    fd_xdp_link_session_t * lo_session   = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_xdp_link_session_t), sizeof(fd_xdp_link_session_t) );

    init_link_session( session, link_session, lo_session, tile,
                       if_idx, lo_idx,
                       init_ctx, &lo_xsk_map_fd );

    /* Notify other net tiles how to find it */

    fd_net0_tile_args_t net0_args = { .pid = getpid(), .xskmap_fd = link_session->xsk_map_fd };
    FD_COMPILER_MFENCE();
    *p_net0_line = net0_args.ul;
    FD_COMPILER_MFENCE();

  } else {

    /* Wait for net tile 0 to do link-wide initialization (in other branch). */

    /* Find PID of net tile 0 */
    FD_COMPILER_MFENCE();
    fd_net0_tile_args_t net0_args = {0};
    do {
      net0_args.ul = *p_net0_line;
      FD_SPIN_PAUSE();
    } while( !net0_args.ul );
    FD_COMPILER_MFENCE();

    /* "Steal" XSKMAP file descriptor from net tile 0 into our tile */

    char xskmap_path[ PATH_MAX ];
    FD_TEST( fd_cstr_printf_check( xskmap_path, PATH_MAX, NULL, "/proc/%d/fd/%d", net0_args.pid, net0_args.xskmap_fd ) );
    init_ctx->xsk_map_fd = open( xskmap_path, O_RDONLY );
    if( FD_UNLIKELY( init_ctx->xsk_map_fd<0 ) ) FD_LOG_ERR(( "open(%s,O_RDONLY) failed", xskmap_path ));

  }

  /* Create and install XSKs */

  fd_xsk_t * xsk =
      fd_xsk_join(
      fd_xsk_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_align(), fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) ),
                  FD_NET_MTU,
                  tile->net.xdp_rx_queue_size,
                  tile->net.xdp_rx_queue_size,
                  tile->net.xdp_tx_queue_size,
                  tile->net.xdp_tx_queue_size ) );
  if( FD_UNLIKELY( !xsk ) ) FD_LOG_ERR(( "fd_xsk_new failed" ));
  
  uint flags = tile->net.zero_copy ? XDP_ZEROCOPY : XDP_COPY;
  if( FD_UNLIKELY( !fd_xsk_init( xsk, if_idx, (uint)tile->kind_id, flags ) ) )
    FD_LOG_ERR(( "failed to bind xsk for net tile %lu", tile->kind_id ));

  if( FD_UNLIKELY( !fd_xsk_activate( xsk, init_ctx->xsk_map_fd ) ) )
    FD_LOG_ERR(( "failed to activate xsk for net tile %lu", tile->kind_id ));
  init_ctx->xsk = xsk;
  if( tile->kind_id != 0 ) {
    FD_TEST( 0==close( init_ctx->xsk_map_fd ) );
    init_ctx->xsk_map_fd = -1;
  }

  init_ctx->xsk_aio = fd_xsk_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) ),
                                      tile->net.xdp_tx_queue_size,
                                      tile->net.xdp_aio_depth );
  if( FD_UNLIKELY( !init_ctx->xsk_aio ) ) FD_LOG_ERR(( "fd_xsk_aio_new failed" ));

  /* Networking tile at index 0 also binds to loopback (only queue 0 available on lo) */

  init_ctx->lo_xsk     = NULL;
  init_ctx->lo_xsk_aio = NULL;
  if( FD_UNLIKELY( 0!=strcmp( tile->net.interface, "lo" ) && !tile->kind_id ) ) {

    fd_xsk_t * lo_xsk =
        fd_xsk_join( 
        fd_xsk_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_align(), fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) ),
                    FD_NET_MTU,
                    tile->net.xdp_rx_queue_size,
                    tile->net.xdp_rx_queue_size,
                    tile->net.xdp_tx_queue_size,
                    tile->net.xdp_tx_queue_size ) );
    if( FD_UNLIKELY( !lo_xsk ) ) FD_LOG_ERR(( "fd_xsk_join failed" ));
    if( FD_UNLIKELY( !fd_xsk_init( lo_xsk, lo_idx, (uint)tile->kind_id, 0 /* flags */ ) ) )
      FD_LOG_ERR(( "failed to bind lo_xsk" ));
    if( FD_UNLIKELY( !fd_xsk_activate( lo_xsk, lo_xsk_map_fd ) ) )
      FD_LOG_ERR(( "failed to activate lo_xsk" ));
    init_ctx->lo_xsk = lo_xsk;
    FD_TEST( 0==close( lo_xsk_map_fd ) );

    init_ctx->lo_xsk_aio = fd_xsk_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) ),
                                           tile->net.xdp_tx_queue_size,
                                           tile->net.xdp_aio_depth );
    if( FD_UNLIKELY( !init_ctx->lo_xsk_aio ) ) FD_LOG_ERR(( "fd_xsk_aio_new failed" ));
  }

  /* init fd_ip */
  init_ctx->ip = fd_ip_join( fd_ip_new( FD_SCRATCH_ALLOC_APPEND( l, fd_ip_align(), fd_ip_footprint( 0UL, 0UL ) ),
                                        0UL, 0UL ) );
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );

  fd_net_ctx_t *      ctx        = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_net_ctx_t),      sizeof(fd_net_ctx_t)      );
  fd_aio_t *          net_rx_aio = fd_aio_join( fd_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(), fd_aio_footprint() ), ctx, net_rx_aio_send ) );
  if( FD_UNLIKELY( !net_rx_aio ) ) FD_LOG_ERR(( "fd_aio_join failed" ));

  ctx->round_robin_cnt = fd_topo_tile_name_cnt( topo, tile->name );
  ctx->round_robin_id  = tile->kind_id;

  ctx->xsk_aio_cnt = 1;
  ctx->xsk_aio[ 0 ] = fd_xsk_aio_join( ctx->init.xsk_aio, ctx->init.xsk );
  if( FD_UNLIKELY( !ctx->xsk_aio[ 0 ] ) ) FD_LOG_ERR(( "fd_xsk_aio_join failed" ));
  fd_xsk_aio_set_rx( ctx->xsk_aio[ 0 ], net_rx_aio );
  ctx->tx = fd_xsk_aio_get_tx( ctx->init.xsk_aio );
  if( FD_UNLIKELY( ctx->init.lo_xsk ) ) {
    ctx->xsk_aio[ 1 ] = fd_xsk_aio_join( ctx->init.lo_xsk_aio, ctx->init.lo_xsk );
    if( FD_UNLIKELY( !ctx->xsk_aio[ 1 ] ) ) FD_LOG_ERR(( "fd_xsk_aio_join failed" ));
    fd_xsk_aio_set_rx( ctx->xsk_aio[ 1 ], net_rx_aio );
    ctx->lo_tx = fd_xsk_aio_get_tx( ctx->init.lo_xsk_aio );
    ctx->xsk_aio_cnt = 2;
  }

  ctx->src_ip_addr = tile->net.src_ip_addr;
  memcpy( ctx->src_mac_addr, tile->net.src_mac_addr, 6UL );

  ctx->metrics.tx_dropped_cnt = 0UL;

  ctx->shred_listen_port = tile->net.shred_listen_port;
  ctx->quic_transaction_listen_port = tile->net.quic_transaction_listen_port;
  ctx->legacy_transaction_listen_port = tile->net.legacy_transaction_listen_port;
  ctx->gossip_listen_port = tile->net.gossip_listen_port;
  ctx->repair_intake_listen_port = tile->net.repair_intake_listen_port;
  ctx->repair_serve_listen_port = tile->net.repair_serve_listen_port;

  /* Put a bound on chunks we read from the input, to make sure they
      are within in the data region of the workspace. */
  if( FD_UNLIKELY( !tile->in_cnt ) ) FD_LOG_ERR(( "net tile in link cnt is zero" ));
  if( FD_UNLIKELY( tile->in_cnt>MAX_NET_INS ) ) FD_LOG_ERR(( "net tile in link cnt %lu exceeds MAX_NET_INS %lu", tile->in_cnt, MAX_NET_INS ));
  FD_TEST( tile->in_cnt<=32 );
  for( ulong i=0UL; i<tile->in_cnt; i++ ) {
    fd_topo_link_t * link = &topo->links[ tile->in_link_id[ i ] ];
    if( FD_UNLIKELY( link->mtu!=FD_NET_MTU ) ) FD_LOG_ERR(( "net tile in link does not have a normal MTU" ));

    ctx->in[ i ].mem    = topo->workspaces[ topo->objs[ link->dcache_obj_id ].wksp_id ].wksp;
    ctx->in[ i ].chunk0 = fd_dcache_compact_chunk0( ctx->in[ i ].mem, link->dcache );
    ctx->in[ i ].wmark  = fd_dcache_compact_wmark( ctx->in[ i ].mem, link->dcache, link->mtu );
  }
  
  for( ulong i = 0; i < tile->out_cnt; i++ ) {
    fd_topo_link_t * out_link = &topo->links[ tile->out_link_id[ i  ] ];
    if( strcmp( out_link->name, "net_quic" ) == 0 ) {
      fd_topo_link_t * quic_out = out_link;
      ctx->quic_out->mcache = quic_out->mcache;
      ctx->quic_out->sync   = fd_mcache_seq_laddr( ctx->quic_out->mcache );
      ctx->quic_out->depth  = fd_mcache_depth( ctx->quic_out->mcache );
      ctx->quic_out->seq    = fd_mcache_seq_query( ctx->quic_out->sync );
      ctx->quic_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( quic_out->dcache ), quic_out->dcache );
      ctx->quic_out->mem    = topo->workspaces[ topo->objs[ quic_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->quic_out->wmark  = fd_dcache_compact_wmark ( ctx->quic_out->mem, quic_out->dcache, quic_out->mtu );
      ctx->quic_out->chunk  = ctx->quic_out->chunk0;
    } else if( strcmp( out_link->name, "net_shred" ) == 0 ) {
      fd_topo_link_t * shred_out = out_link;
      ctx->shred_out->mcache = shred_out->mcache;
      ctx->shred_out->sync   = fd_mcache_seq_laddr( ctx->shred_out->mcache );
      ctx->shred_out->depth  = fd_mcache_depth( ctx->shred_out->mcache );
      ctx->shred_out->seq    = fd_mcache_seq_query( ctx->shred_out->sync );
      ctx->shred_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( shred_out->dcache ), shred_out->dcache );
      ctx->shred_out->mem    = topo->workspaces[ topo->objs[ shred_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->shred_out->wmark  = fd_dcache_compact_wmark ( ctx->shred_out->mem, shred_out->dcache, shred_out->mtu );
      ctx->shred_out->chunk  = ctx->shred_out->chunk0;
    } else if( strcmp( out_link->name, "net_gossip" ) == 0 ) {
      fd_topo_link_t * gossip_out = out_link;
      ctx->gossip_out->mcache = gossip_out->mcache;
      ctx->gossip_out->sync   = fd_mcache_seq_laddr( ctx->gossip_out->mcache );
      ctx->gossip_out->depth  = fd_mcache_depth( ctx->gossip_out->mcache );
      ctx->gossip_out->seq    = fd_mcache_seq_query( ctx->gossip_out->sync );
      ctx->gossip_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( gossip_out->dcache ), gossip_out->dcache );
      ctx->gossip_out->mem    = topo->workspaces[ topo->objs[ gossip_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->gossip_out->wmark  = fd_dcache_compact_wmark ( ctx->gossip_out->mem, gossip_out->dcache, gossip_out->mtu );
      ctx->gossip_out->chunk  = ctx->gossip_out->chunk0;
    } else if( strcmp( out_link->name, "net_repair" ) == 0 ) {
      fd_topo_link_t * repair_out = out_link;
      ctx->repair_out->mcache = repair_out->mcache;
      ctx->repair_out->sync   = fd_mcache_seq_laddr( ctx->repair_out->mcache );
      ctx->repair_out->depth  = fd_mcache_depth( ctx->repair_out->mcache );
      ctx->repair_out->seq    = fd_mcache_seq_query( ctx->repair_out->sync );
      ctx->repair_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( repair_out->dcache ), repair_out->dcache );
      ctx->repair_out->mem    = topo->workspaces[ topo->objs[ repair_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->repair_out->wmark  = fd_dcache_compact_wmark ( ctx->repair_out->mem, repair_out->dcache, repair_out->mtu );
      ctx->repair_out->chunk  = ctx->repair_out->chunk0;
    } else {
      FD_LOG_ERR(( "unrecognized out link `%s`", out_link->name ));
    }
  }

  /* Check if any of the tiles we set a listen port for do not have an outlink. */
  if( FD_UNLIKELY( ctx->shred_listen_port!=0 && ctx->shred_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "shred listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->quic_transaction_listen_port!=0 && ctx->quic_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "quic transaction listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->legacy_transaction_listen_port!=0 && ctx->quic_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "legacy transaction listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->gossip_listen_port!=0 && ctx->gossip_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "gossip listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->repair_intake_listen_port!=0 && ctx->repair_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "repair intake port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->repair_serve_listen_port!=0 && ctx->repair_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "repair serve listen port set but no out link was found" ));
  }

  ctx->ip = ctx->init.ip;

  ulong scratch_top = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  if( FD_UNLIKELY( scratch_top > (ulong)scratch + scratch_footprint( tile ) ) )
    FD_LOG_ERR(( "scratch overflow %lu %lu %lu", scratch_top - (ulong)scratch - scratch_footprint( tile ), scratch_top, (ulong)scratch + scratch_footprint( tile ) ));
}

static ulong
populate_allowed_seccomp( fd_topo_t const *      topo,
                          fd_topo_tile_t const * tile,
                          ulong                  out_cnt,
                          struct sock_filter *   out ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_ctx_t ), sizeof( fd_net_ctx_t ) );

  /* A bit of a hack, if there is no loopback XSK for this tile, we still need to pass
     two "allow" FD arguments to the net policy, so we just make them both the same. */
  int allow_fd2 = ctx->init.lo_xsk ? ctx->init.lo_xsk->xsk_fd : ctx->init.xsk->xsk_fd;
  FD_TEST( ctx->init.xsk->xsk_fd >= 0 && allow_fd2 >= 0 );
  int netlink_fd = fd_ip_netlink_get( ctx->init.ip )->fd;
  populate_sock_filter_policy_net( out_cnt, out, (uint)fd_log_private_logfile_fd(), (uint)ctx->init.xsk->xsk_fd, (uint)allow_fd2, (uint)netlink_fd );
  return sock_filter_policy_net_instr_cnt;
}

static ulong
populate_allowed_fds( fd_topo_t const *      topo,
                      fd_topo_tile_t const * tile,
                      ulong                  out_fds_cnt,
                      int *                  out_fds ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_ctx_t ), sizeof( fd_net_ctx_t ) );

  if( FD_UNLIKELY( out_fds_cnt<7UL ) ) FD_LOG_ERR(( "out_fds_cnt %lu", out_fds_cnt ));

  ulong out_cnt = 0UL;

  out_fds[ out_cnt++ ] = 2; /* stderr */
  if( FD_LIKELY( -1!=fd_log_private_logfile_fd() ) )
    out_fds[ out_cnt++ ] = fd_log_private_logfile_fd(); /* logfile */
  out_fds[ out_cnt++ ] = fd_ip_netlink_get( ctx->init.ip )->fd;

  out_fds[ out_cnt++ ] = ctx->init.xsk->xsk_fd;
  if( ctx->init.xdp_prog_link_fd >= 0 )
    out_fds[ out_cnt++ ] = ctx->init.xdp_prog_link_fd;
  if( ctx->init.xsk_map_fd >= 0 )
    out_fds[ out_cnt++ ] = ctx->init.xsk_map_fd;

  if( ctx->init.lo_xdp_prog_link_fd >= 0 )
    out_fds[ out_cnt++ ] = ctx->init.lo_xdp_prog_link_fd;
  if( ctx->init.lo_xsk )
    out_fds[ out_cnt++ ] = ctx->init.lo_xsk->xsk_fd;
  return out_cnt;
}

#define STEM_BURST (1UL)

#define STEM_CALLBACK_CONTEXT_TYPE  fd_net_ctx_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_net_ctx_t)

#define STEM_CALLBACK_METRICS_WRITE       metrics_write
#define STEM_CALLBACK_DURING_HOUSEKEEPING during_housekeeping
#define STEM_CALLBACK_BEFORE_CREDIT       before_credit
#define STEM_CALLBACK_BEFORE_FRAG         before_frag
#define STEM_CALLBACK_DURING_FRAG         during_frag
#define STEM_CALLBACK_AFTER_FRAG          after_frag

#include "../../../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_net = {
  .name                     = "net",
  .populate_allowed_seccomp = populate_allowed_seccomp,
  .populate_allowed_fds     = populate_allowed_fds,
  .scratch_align            = scratch_align,
  .scratch_footprint        = scratch_footprint,
  .privileged_init          = privileged_init,
  .unprivileged_init        = unprivileged_init,
  .run                      = stem_run,
};
