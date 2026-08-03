# UB two-node benchmark

This directory contains independent OBMM micro tests and the existing Memlink
benchmark. The OBMM programs are deliberately separate executables so a local
test cannot accidentally join or reuse a two-host run:

- `ub_two_node_bench`: the existing Memlink latency/correctness benchmark;
- `obmm_local_nc_test`: one-host OBMM export, NC mmap, correctness, and local
  load/store latency;
- `obmm_nc_exporter`: owner/export half of the two-host NC visibility test;
- `obmm_nc_importer`: remote/import half of the two-host NC visibility test.

## OBMM micro tests

Every OBMM test opens `/dev/obmm_shmdev<mem_id>` with `O_RDWR | O_SYNC` and
maps it with `PROT_READ | PROT_WRITE | MAP_SHARED`. All mappings in this first
round are non-cacheable. The programs do not call `obmm_set_ownership`, CLWB,
or any cache invalidation instruction.

The local test creates and destroys its own export without using TCP. In the
two-host test, TCP carries only the export descriptor and phase notifications;
the payload travels only through the OBMM mapping:

```text
owner NC store    -> importer NC load and byte-for-byte verification
importer NC store -> owner NC load and byte-for-byte verification
```

### 1. Check each host before building

The kernel module must already be loaded and its control device must exist:

```bash
grep '^obmm ' /proc/modules
ls -l /dev/obmm
```

It is normal for `/dev/obmm_shmdev*` not to exist before the first successful
export or import.

Find the local UB controller values used by the test:

```bash
find /sys/devices -path '*/ubc/eid' -print
find /sys/devices -path '*/ubc/primary_cna' -print

cat /sys/devices/ub_bus_controller0/*/ubc/eid
cat /sys/devices/ub_bus_controller0/*/ubc/primary_cna
```

Use the controller that belongs to the intended UB path. The exporter and
local test need the local EID. The importer needs both its local EID and its
`primary_cna`.

### 2. Build three independent OBMM executables

When `libobmm.so` has not been built, point CMake at the directory containing
`libobmm.c`, `vendor_adaptor.c`, and their headers. The two C files will be
compiled into a private static library for this test. Since `libobmm.h`
includes `ub/obmm.h`, also provide the UAPI include root:

```bash
cmake -S tests/ub-two-node -B tests/ub-two-node/build-obmm \
  -DOBMM_SOURCE_DIR=/path/to/obmm-master/src/libobmm \
  -DOBMM_UAPI_INCLUDE_DIR=/path/to/include-root-containing-ub-directory
cmake --build tests/ub-two-node/build-obmm \
  --target obmm_local_nc_test obmm_nc_exporter obmm_nc_importer -j
```

`OBMM_INCLUDE_DIR` is inferred from `OBMM_SOURCE_DIR`. Set it explicitly only
if `libobmm.h` is stored elsewhere. If a prebuilt library becomes available,
you may instead set `OBMM_LIBRARY=/path/to/libobmm.so`.

For example, if the UAPI header is
`/opt/obmm-master/include/uapi/ub/obmm.h`, pass
`-DOBMM_UAPI_INCLUDE_DIR=/opt/obmm-master/include/uapi`.

Check that all executables were produced:

```bash
ls -l tests/ub-two-node/build-obmm/obmm_local_nc_test \
      tests/ub-two-node/build-obmm/obmm_nc_exporter \
      tests/ub-two-node/build-obmm/obmm_nc_importer
```

### 3. Run the local test independently on each host

Run this first on host 0, with no process running on host 1:

```bash
tests/ub-two-node/build-obmm/obmm_local_nc_test \
  --local-eid 0x10 --numa-id 0 \
  --region-mb 2 --test-bytes 4096 --iterations 100000
```

After it exits and removes its export, run the same executable independently
on host 1 using host 1's EID and valid local NUMA node. The result contains:

```text
PASS local NC load/store correctness (4096 bytes)
local_nc_load_avg_ns=...
local_nc_fenced_store_avg_ns=...
PASS local OBMM NC test and cleanup
```

`local_nc_load_avg_ns` measures repeated volatile 8-byte loads. The fenced
store metric executes a full compiler/CPU barrier after every 8-byte store; it
is an ordered-store cost, not a claim about persistent-media completion.

The program maps the complete `--region-mb` region but touches
`--test-bytes` for correctness. OBMM may require a larger region than 2 MiB on
systems with a larger basic allocation granularity.

### 4. Run the two-host NC export/import test

After both local tests have exited, start the exporter on host 0. Its EID is
placed in the export descriptor's `deid` field:

```bash
tests/ub-two-node/build-obmm/obmm_nc_exporter \
  --bind-ip 192.0.2.10 --port 18515 \
  --local-eid 0x10 --numa-id 0 \
  --region-mb 2 --test-bytes 4096
```

The exporter creates and maps its memory before listening. While it waits,
host 0 should show one `/dev/obmm_shmdev<mem_id>` device. Then start the
importer on host 1. Its EID and primary CNA become `seid` and `scna`:

```bash
tests/ub-two-node/build-obmm/obmm_nc_importer \
  --owner-ip 192.0.2.10 --port 18515 \
  --local-eid 0x20 --local-scna 0x1 \
  --test-bytes 4096
```

Use the actual `eid`, `primary_cna`, IP addresses, valid owner NUMA ID, and
OBMM allocation granularity from the two machines. On kernels whose OBMM basic
granularity is larger than 2 MiB, increase `--region-mb` accordingly.

Successful exporter output ends with:

```text
PASS exporter NC store -> importer NC load (4096 bytes)
PASS importer NC store -> exporter NC load (4096 bytes)
PASS two-node OBMM NC exporter and cleanup
```

The importer first performs `munmap`, `close`, and `obmm_unimport`, and only
then sends the final cleanup notification. The exporter subsequently performs
`munmap`, `close`, and `obmm_unexport`. After both programs exit successfully,
their newly created `/dev/obmm_shmdev*` devices should disappear.

## Memlink benchmark

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
