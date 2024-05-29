set -e

EXTRAS=no-solana make -j fddev
make -j


# sudo ./build/native/gcc/bin/fd_shmem_cfg fini || true
# sudo ./build/native/gcc/bin/fd_shmem_cfg init 0700 chali ""

# rm -f /data/chali/snapshot-*
# wget --trust-server-names https://api.mainnet-beta.solana.com/snapshot.tar.bz2 -P /data/chali

# rm -f /data/chali/mainnet-funk
# sudo ./build/native/gcc/bin/fd_ledger --cmd ingest --funk-page-cnt 600 --index-max 600000000 --txns-max 1024 --funk-only 1 --checkpt-funk /data/chali/mainnet-funk --snapshot /data/chali/snapshot-*


rm -f /data/chali/incremental-snapshot-*
wget --trust-server-names https://api.mainnet-beta.solana.com/incremental-snapshot.tar.bz2 -P /data/chali

GOSSIP_PORT=$(shuf -i 8000-10000 -n 1)

echo "[gossip]
    port = $GOSSIP_PORT
[tiles]
    [tiles.gossip]
        entrypoints = [\"147.75.84.157\"]
        peer_ports = [8000]
        gossip_listen_port = $GOSSIP_PORT
    [tiles.repair]
        repair_intake_listen_port = $(shuf -i 8000-10000 -n 1)
        repair_serve_listen_port = $(shuf -i 8000-10000 -n 1)
    [tiles.replay]
        snapshot = \"wksp:/data/chali/mainnet-funk\"
        incremental = \"$(echo /data/chali/incremental-*)\"
        tpool_thread_count = 13
        funk_sz_gb = 600
        funk_txn_max = 1024
        funk_rec_max = 600000000
[consensus]
    expected_shred_version = 50093
[log]
  path = \"fddev.log\"
  level_stderr = \"NOTICE\"
[development]
    topology = \"firedancer\"
" > mainnet.toml

sudo ./build/native/gcc/bin/fddev configure fini all || true
sudo gdb -ex=r --args ./build/native/gcc/bin/fddev --config mainnet.toml --no-sandbox --no-clone --no-solana-labs