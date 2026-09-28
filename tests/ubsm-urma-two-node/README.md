# UBSM and URMA two-node compatibility tests

This directory deliberately stays independent from eRPC. It answers two
questions before a hybrid transport is attempted:

1. What are URMA READ/WRITE post-to-completion latencies for relevant sizes?
2. Can an existing UBSM owner mapping be registered as an URMA segment, and
   are CPU and UDMA writes mutually visible after completion?

TCP is used only to exchange URMA metadata and synchronize test phases. TCP is
outside every reported latency interval.

## Build

Run on both UB hosts from the repository root:

```bash
cmake -S tests/ubsm-urma-two-node \
  -B tests/ubsm-urma-two-node/build \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so \
  -DURMA_INCLUDE_DIR=/usr/include/ub/umdk/urma \
  -DURMA_COMMON_INCLUDE_DIR=/usr/include/ub/umdk/urma \
  -DURMA_LIBRARY=/usr/lib64/liburma.so \
  -DURMA_COMMON_LIBRARY=/usr/lib64/liburma_common.so

cmake --build tests/ubsm-urma-two-node/build -j
```

Adjust the URMA paths to the actual installation. Before running, verify the
device and EID names:

```bash
urma_admin show --all
```

The examples below assume an RC-capable `udma2` device using RTP (`--tp-type
0`). For another device, use the `tp_type` reported by the working
`urma_sample` configuration.

## 1. Pure URMA latency

Start the server first on machine 98:

```bash
numactl --cpunodebind=0 --membind=0 \
  ./tests/ubsm-urma-two-node/build/urma_rw_latency_test \
  --role server --device udma2 --bind-ip <IP98> --port 18620 \
  --tp-type 0 --iterations 10000 --warmup 100 \
  --sizes 64,128,256,512,1024,1300,2048,4096
```

Then run the client on machine 99:

```bash
numactl --cpunodebind=0 --membind=0 \
  ./tests/ubsm-urma-two-node/build/urma_rw_latency_test \
  --role client --device udma2 --server-ip <IP98> --port 18620 \
  --tp-type 0 --iterations 10000 --warmup 100 \
  --sizes 64,128,256,512,1024,1300,2048,4096
```

The reported interval starts immediately before
`urma_post_jetty_send_wr()` and ends after a successful JFC completion.

## 2. Register an UBSM mapping and test visibility

Use a unique UBSM name for each run. Start the owner on machine 98:

```bash
numactl --cpunodebind=0 --membind=0 \
  ./tests/ubsm-urma-two-node/build/ubsm_urma_compat_test \
  --role owner --device udma2 --bind-ip <IP98> --port 18621 \
  --name ubsm_urma_98_99_01 --provider-numa 0 --region-mb 4 \
  --tp-type 0 --iterations 10000 --warmup 100 \
  --sizes 64,128,256,512,1024,1300,2048,4096
```

Then run the remote side on machine 99:

```bash
numactl --cpunodebind=0 --membind=0 \
  ./tests/ubsm-urma-two-node/build/ubsm_urma_compat_test \
  --role remote --device udma2 --server-ip <IP98> --port 18621 \
  --name ubsm_urma_98_99_01 --region-mb 4 \
  --tp-type 0 --iterations 10000 --warmup 100 \
  --sizes 64,128,256,512,1024,1300,2048,4096
```

This test verifies all of the following:

- `urma_register_seg()` accepts the exact UBSM owner virtual mapping;
- remote CPU loads through the UBSM imported mapping see owner CPU writes;
- URMA READ from that owner mapping reaches a normal local registered buffer;
- owner CPU loads see data after URMA WRITE completion;
- the remote UBSM CPU mapping also sees the completed URMA WRITE.

Registration success alone is not enough. Treat the test as compatible only
when all visibility checks pass in both machine directions. Repeat with
machines 98 and 99 swapped as owner.

## Interpretation

- If pure URMA works but owner registration fails, a direct UBSM+URMA hybrid
  cannot reuse the current machine region. A separate URMA buffer pool or a
  staging copy would be required.
- If registration succeeds but a visibility check fails, do not integrate the
  paths until cacheability and CPU/UDMA ordering are resolved.
- If all checks pass, compare `urma_read_from_ubsm` at 1300 and 2048 bytes with
  the existing UBSM scalar-copy and memcpy results before choosing a crossover
  threshold.
