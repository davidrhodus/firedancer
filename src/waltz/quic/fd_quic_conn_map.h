#ifndef HEADER_fd_src_waltz_quic_fd_quic_conn_map_h
#define HEADER_fd_src_waltz_quic_fd_quic_conn_map_h

#include "../../util/fd_util_base.h"

struct __attribute__((aligned(16))) fd_quic_conn_map {
  ulong conn_id;
  fd_quic_conn_t *  conn;
};
typedef struct fd_quic_conn_map fd_quic_conn_map_t;

#define MAP_NAME        fd_quic_conn_map
#define MAP_T           fd_quic_conn_map_t
#define MAP_KEY         conn_id
#define MAP_MEMOIZE     0
#define MAP_KEY_HASH(k) ((uint)k)
#include "../../util/tmpl/fd_map_dynamic.c"

FD_PROTOTYPES_BEGIN

fd_quic_conn_t *
fd_quic_conn_query( fd_quic_conn_map_t * map,
                    ulong                conn_id );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_waltz_quic_fd_quic_conn_map_h */

