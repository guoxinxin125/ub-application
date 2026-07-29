# UB two-node benchmark

`ub_two_node_bench` runs the same binary on two UB hosts. TCP is used only for
reachability, phase synchronization, and correctness acknowledgements. The
measured data accesses use the Memlink mappings directly. One invocation runs
one test point so that failures remain isolated and easy to diagnose.

List the test points without requiring Memlink arguments:

```bash
tests/ub-two-node/build/ub_two_node_bench --list-tests
```

Available test points:

- `connectivity`: TCP reachability and bidirectional RTT only;
- `basic`: the original local/remote latency and 8-byte visibility test;
- `size_visibility`: bidirectional 8B, 64B, 256B, 4KiB, and 64KiB tests;
- `owner_stream_remote_read`: continuous owner writes with high-frequency
  remote snapshot reads;
- `remote_atomic`: the explicitly selected experimental remote CPU atomic
  capability test.

The `basic` test reports:

- bidirectional TCP round-trip latency;
- owner-local hot read, pointer-chase read, cached write, and ordered write;
- owner-local atomic load, store, fetch-add, and successful CAS;
- owner write followed by remote invalidate and load, in both directions;
- remote invalidate, store, write-back, and owner read, in both directions.

The `remote_atomic` test measures sequential remote fetch-add/CAS and stresses
one counter with concurrent owner-local and remote fetch-add operations. A
latency number does not prove distributed atomicity. Only a PASS from the
contended correctness test supports using that operation on this mapping, and
the result still applies only to the tested platform/configuration.

## Build

```bash
cmake -S tests/ub-two-node -B tests/ub-two-node/build \
  -DMEMLINK_CLIENT_LIBRARY=/path/to/libmemlink_client.so
cmake --build tests/ub-two-node/build -j
```

If the client library has further runtime dependencies:

```bash
export LD_LIBRARY_PATH=/path/to/vendor/libs:${LD_LIBRARY_PATH}
ldd tests/ub-two-node/build/ub_two_node_bench | grep 'not found'
```

The final command must print nothing.

## Run

Use a new prefix for every run.  Start both commands within the startup
timeout.  Replace the IP addresses and physical Memlink node IDs.

Host 0:

```bash
tests/ub-two-node/build/ub_two_node_bench \
  --id 0 --local-ip 192.0.2.10 --peer-ip 192.0.2.11 \
  --owner-node-id 0 --prefix ub_probe_001 \
  --test basic \
  --region-mb 64 --working-set-mb 4 --iterations 10000
```

Host 1:

```bash
tests/ub-two-node/build/ub_two_node_bench \
  --id 1 --local-ip 192.0.2.11 --peer-ip 192.0.2.10 \
  --owner-node-id 1 --prefix ub_probe_001 \
  --test basic \
  --region-mb 64 --working-set-mb 4 --iterations 10000
```

For another test point, keep all networking and owner arguments the same on
the two hosts, choose a new prefix, and change `--test` on both commands.

### Different-size visibility

```bash
--test size_visibility \
--prefix ub_size_probe_001 \
--size-iterations 1000
```

Every size is run separately in these four directions:

```text
owner 0 write -> host 1 invalidate/read
host 1 invalidate/write/writeback -> owner 0 ordinary read
owner 1 write -> host 0 invalidate/read
host 0 invalidate/write/writeback -> owner 1 ordinary read
```

The complete payload is compared byte by byte. A 64-byte guard exists before
and after the payload, so partial-cache-line corruption is also detected.

### Continuous owner write and remote polling

```bash
--test owner_stream_remote_read \
--prefix ub_stream_probe_001 \
--stream-payload-size 4096 \
--stream-duration-ms 3000
```

The owner continuously publishes a versioned multi-cache-line record using
ordinary cached stores and fences only. It never flushes or invalidates. The
remote host repeatedly:

1. invalidates and reads the leading version;
2. invalidates and copies the payload plus trailing version;
3. invalidates and reads the leading version again;
4. accepts the snapshot only when both versions match, are even, and every
   payload byte matches that version's deterministic pattern.

The output reports accepted snapshots, retries caused by concurrent updates,
corruption, version regressions, and whether the final owner version was seen.
Both hosts take a turn as owner.

### Remote atomics

After the base tests pass, explicitly select the remote-atomic capability
probe on both hosts with a new prefix:

```bash
--test remote_atomic \
--prefix ub_atomic_probe_001 \
--atomic-iterations 10000
```

The test unmaps its virtual mappings but deliberately does not delete named
Memlink regions.  Deleting a region can affect the peer, so cleanup must use
the vendor Memlink management command after both processes have exited.

## Interpreting the main metrics

- `local_owner_*`: owner CPU access; no cache-line flush/invalidate is used.
- `remote_read_*_invalidate_load`: remote correctness path including explicit
  invalidation and the following load.
- `remote_write_*_invalidate_store_writeback`: safe partial-line remote write
  path including invalidation, store, write-back, and completion fence.
- `remote_atomic_*_with_cache_maintenance`: experimental CPU atomic on a
  remote mapping; it is not a device-atomic API.
- `contended_cross_host_fetch_add`: the important cross-host atomic
  correctness result.  FAIL means remote CPU atomics must not be used for
  cross-host locks or counters.

Run with CPU and memory placement pinned when comparing machines, for example
with the appropriate local NUMA node:

```bash
numactl --physcpubind=4 --membind=0 tests/ub-two-node/build/ub_two_node_bench ...
```
