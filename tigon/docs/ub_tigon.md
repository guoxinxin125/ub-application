# UB-Tigon implementation notes

## Target data path

Each coordinator owns one named UBS Memory region and maps every peer region.
Partition ownership and physical region ownership are identical. Shared memory
never contains a process virtual address; `UBGlobalPtr` identifies an object by
`region_id`, `generation`, and `offset`.

For point reads and updates, the requester resolves the owner-region pointer,
performs the tuple CAS itself, copies the value into or out of the transaction's
local buffer, and releases the same shared lock at commit or abort. The owner is
not on this data path. Migration and the CXL SCC bitmap/flush path are disabled
when `--shared_memory_backend=ub` is selected.

Messages remain copy based. A sender copies a small serialized `Message` into
the destination's UB queue and the receiver copies it into a local `Message`.
UB queues use acquire/release publication and do not execute `clflush`, `clwb`,
or `_mm_sfence`.

## Implemented substrate

- `common/UBGlobalPtr.h`: fixed 16-byte cross-process pointer ABI.
- `common/UBMemory.*`: UBSM initialization; one owner region per coordinator;
  all-region mapping; generation/range checks; root directory; CAS bump allocator;
  importer-first unmap and owner deallocation.
- `common/UBTuple.h`: requester-side shared read/write lock and tuple header.
- `common/UBCpu.h`: AArch64/x86 portable spin-loop relaxation.
- `common/UBMPSCRingBuffer.*`: bounded copy-based UB MPSC queue.
- `core/UBBTreeCatalog.*`: version-2 catalog/descriptor ABI in root slot 8;
  every table descriptor also publishes a stable `+infinity` gap tuple.
- `core/UBBPlusTree.h`: position-independent UB B+ Tree, stable tuples,
  ordered leaves, requester-side insert, leaf/inner split, and root replacement.
- `core/UBBPlusTreeAdapter.h`: the UB B+ Tree implementation of `ITable`.
- `tests/ub_primitives_test.cpp`: host-only ABI and contended-lock test.
- `tests/ub_btree_host_test.cpp`: concurrent insert/split, bounded range scan,
  successor/end-gap locking, tombstone, and same-key reuse over an in-process
  region arena; 10,000 delete/reinsert cycles verify that same-key churn does
  not advance tuple allocation.
- `tests/ub_btree_compile_test.cpp` and `tests/ub_adapter_compile_test.cpp`:
  complete template/`ITable` build checks.
- `tests/ub_tpcc_adapter_compile_test.cpp`: instantiates all eleven TPCC UB
  table/index adapters.

The integrated transaction path supports YCSB `--query=rmw`,
`--query=insert`, `--query=delete`, `--query=scan`, and `--query=mixed`. Database
initialization creates `TableUBBPlusTree` objects and writes owner-partition tuples
directly into the owner's region. Both local and remote point reads/updates use
the same requester-side `UBTupleHeader` lock; the remote path does not send a
migration request. Inserts reserve a state-2 placeholder in the owner index and
publish it as state 1 at commit; abort changes it to a state-0 tombstone. Deletes
lock the visible tuple directly, publish state 0 at commit, and merely unlock on
abort. A range transaction locks every selected tuple and the first visible
successor, or a stable end-gap tuple when the range reaches the end. A
requester-side insert must acquire that same successor lock while holding the
tree structural write lock, preventing a phantom from entering a protected
range.

TPCC now constructs all eleven primary/secondary tables as UB B+ Trees,
including `customer_name_idx` and `order_customer`. The shared item table is
initialized only by its owner. Payment/new-order/first-two/mixed are available
for staged validation, but this TPCC path has only passed template/static
validation in this workspace and must not yet be treated as UB-machine verified.

With `--use_ub_transport=true`, each coordinator publishes one bounded MPSC
inbox in root slot 9 of its owner region. The outgoing dispatcher copies each
serialized `Message` into the destination inbox, and its single incoming
dispatcher copies it into a process-local `Message`. Message grouping is
disabled for this fixed-entry transport; an individual message must not exceed
`--cxl_trans_entry_struct_size`.

The B+ Tree leaf stores `{key, tuple_offset}` rather than embedding the tuple.
Leaf entry movement and split therefore never move `UBTupleHeader` or its
transaction lock. The B0-B3 implementation uses one requester-side tree RW lock
to make lookup, ordered traversal, insert, split, and root replacement correct
before node-level OLC is optimized. Tuple transaction locks remain independent
and are still held until commit/abort.

The region allocator remains monotonic. A state-0 tombstone may be reused in
place for the same logical key while keeping its address and version monotonic.
An aborted placeholder is currently retained as a tombstone; neither tuples nor
B+ Tree nodes are recycled for a different key. Cross-key reuse, physical leaf
deletion, merge, and root shrink require a shared epoch/QSBR protocol to avoid
lookup-to-tuple-lock ABA.

The B+ Tree descriptor ABI changed to version 2 for end-gap protection. Do not
reuse a prefix created by the version-1 tree; every post-upgrade run must use a
fresh region prefix.

## Prerequisites on both UB hosts

The instructions below use the following fixed deployment. Replace the example
IPs and provider hostnames with the values registered on the real machines:

| Machine | Tigon ID | Example IP | Memory NUMA node | Usable CPUs on that node |
| --- | ---: | --- | ---: | ---: |
| 81 | 0 | `192.0.2.10` | 1 | 37 |
| 82 | 1 | `192.0.2.11` | 0 | 20 |

Both AArch64 hosts must run the same Tigon revision and a mutually compatible
UBS Memory software stack. Machine 82 is the worker-thread capacity bottleneck.

Verify the architecture, services, SDK architecture, and dynamic dependencies:

```bash
uname -m                         # expected: aarch64
systemctl is-active ubse.service
systemctl is-active ubsmd
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so
```

Both services must be active, `libubsm_sdk.so` must be an AArch64 library, and
`ldd` must not report `not found`. Configure the SDK runtime environment in
every shell that builds or runs Tigon:

```bash
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:${LD_LIBRARY_PATH:-}
export HCOM_CONNECTION_RETRY_TIMES=2
```

The application user must be able to access the local `ubsmd` Unix socket:

```bash
id
getent group ubsmd
```

If the user is not a member of the group, an administrator can add it with
`sudo usermod -aG ubsmd "$(id -un)"`; log in again before testing. Prefer the
same UID/GID on both hosts because shared regions use mode `0660`.

Each process should explicitly name its local physical provider. The value must
exactly match the hostname registered in the UBS Engine topology; it is not
necessarily the FQDN returned by `hostname -f`:
> 这里需要把命令里面的ub_provider_host换成实际的Host name

```bash
# Machine 81 / coordinator 0 only
export UB_PROVIDER_HOST=host-81
export UB_NUMA_NODE=1
export UB_PROVIDER_NUMA=1

# Machine 82 / coordinator 1 only
export UB_PROVIDER_HOST=host-82
export UB_NUMA_NODE=0
export UB_PROVIDER_NUMA=0
```

Confirm that the hostnames resolve and the management network is reachable.
TCP port `18951` on host 0 is used by the standalone correctness barrier, and
TCP port `10010` on both hosts is used by the Tigon coordinators. UBS
Engine/OBMM discovery and data paths must already be operational.

Verify the configured placement against the real topology:

```bash
numactl --hardware
```

`UB_NUMA_NODE` and `UB_PROVIDER_NUMA` control different layers. The supplied
scripts use `UB_NUMA_NODE` only for
`numactl --cpunodebind=N --membind=N`, which places process threads and ordinary
process-local allocations. They translate `UB_PROVIDER_NUMA` to the UBSM
provider NUMA argument, which places the coordinator-owned UB region. For this
deployment both values are 1 on machine 81 and 0 on machine 82. If
`UB_PROVIDER_NUMA` is unset, UBS Engine selects the provider NUMA; if
`UB_NUMA_NODE` is unset, the script does not invoke `numactl`.

`one-sided` mode uses
`UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP` and therefore
requires the platform's NC-CC/snoop configuration to support that mode.
`nocache` uses `UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`.

## Build

The current CMake configuration requires CMake, `/usr/bin/clang-15`,
`/usr/bin/clang++-15`, `lld-15`, pthread, Boost, jemalloc, glog, and gflags.
`numactl` is recommended for runtime placement. Debian/Ubuntu package names are:

```bash
sudo apt-get install -y \
  cmake clang-15 lld-15 \
  libboost-all-dev libjemalloc-dev \
  libgoogle-glog-dev libgflags-dev numactl
```

On an RPM-based UB host, install the corresponding development packages and
confirm the exact compiler/linker paths expected by `CMakeLists.txt`:

```bash
# openEuler 22.03 LTS SP2/SP3/SP4; clang15/lld15 are normally in EPOL.
sudo dnf makecache
sudo dnf install -y \
  cmake make gcc-c++ glibc-devel \
  clang15 lld15 \
  boost-devel jemalloc-devel \
  glog-devel gflags-devel \
  numactl
```

If `dnf` cannot find `clang15`, `lld15`, `glog-devel`, or `gflags-devel`, first
check that the EPOL repository matching the installed openEuler release and
AArch64 architecture is configured and enabled:

```bash
cat /etc/openEuler-release
uname -m                         # expected: aarch64
sudo dnf repolist --enabled
sudo dnf provides '*/clang-15'
sudo dnf provides '*/clang++-15'
sudo dnf provides '*/ld.lld-15'
```

Do not enable an EPOL repository from a different openEuler service pack. Some
newer openEuler releases name the packages `clang` and `lld` instead of
`clang15` and `lld15`; use the packages reported by `dnf provides`, but verify
that they install the exact paths currently required by Tigon:

```bash
command -v cmake
test -x /usr/bin/clang-15
test -x /usr/bin/clang++-15
command -v ld.lld-15
rpm -q cmake clang15 lld15 boost-devel jemalloc-devel \
  glog-devel gflags-devel numactl
```

The final `rpm -q` example uses the openEuler 22.03 LTS package names. If the
installed release uses unversioned `clang`/`lld` packages, substitute those two
names in the query. The UB SDK itself is not installed by this command; its
headers and AArch64 `libubsm_sdk.so` must already be installed under
`/usr/local/ubs_mem`, or supplied through `UBSM_INCLUDE_DIR` and `UBSM_LIBRARY`.

Host/build-layer tests:

```bash
cmake -S tigon -B tigon/build-host \
  -DTIGON_BUILD_UB_UNIT_TESTS=ON
cmake --build tigon/build-host -j --target \
  ub_primitives_test ub_btree_host_test ub_btree_compile_test \
  ub_adapter_compile_test ub_tpcc_adapter_compile_test ub_twopl_compile_test
ctest --test-dir tigon/build-host --output-on-failure \
  -R 'ub_(primitives|btree|adapter|tpcc|twopl)'
```

Build independently on both AArch64 hosts because the project uses
`-march=native`. Use a new directory rather than a cache produced for x86-64 or
the legacy CXL backend:

Run the following from the `ub-application` repository root on each host:

```bash
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64"

cmake -S "$TIGON_REPO_ROOT/tigon" -B "$TIGON_BUILD_DIR" \
  -DTIGON_ENABLE_UB=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build "$TIGON_BUILD_DIR" -j \
  --target bench_ycsb bench_tpcc ub_tigon_two_node_test
```

`$PWD` is not a requirement that the build output live in an arbitrary current
directory. It expands to the absolute path of the shell's current directory;
the command above is correct only after changing to the repository root. If the
repository is at `/work/ub-application`, an equivalent location-independent
setting is:

```bash
export TIGON_REPO_ROOT=/work/ub-application
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64"
```

The scripts default to `tigon/build-ub`. `TIGON_BUILD_DIR` is needed here only
because this guide deliberately uses the distinct `build-ub-aarch64` directory.

The UB build does not link
`dependencies/cxlalloc/libcxlalloc_static.a`. That archive is an x86-64
artifact retained only for the legacy CXL backend and is incompatible with an
AArch64 UB host. Legacy CXL declarations are satisfied by fail-fast stubs in a
UB build; reaching one of those stubs means that an unadapted CXL execution
path was selected and the process terminates with the offending function name.
On AArch64, use an AArch64 UBS Memory SDK and verify its complete dependency
chain before building:

```bash
uname -m                         # expected: aarch64
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so
```

The configure output should contain:

```text
Tigon UB backend enabled for aarch64; the legacy cxlalloc archive will not be linked
```

Check both resulting executables before running:

```bash
file "$TIGON_BUILD_DIR/bench_ycsb"
file "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
ldd "$TIGON_BUILD_DIR/bench_ycsb"
ldd "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
```

They must be AArch64 executables and have no unresolved shared libraries.

Use a unique `--ub_region_prefix` for every run and do not reuse a prefix
between `one-sided` and `nocache`. Both hosts must use the same prefix, region
size, coordinator count, and mode; their `--id` and provider host differ.

## Two-host validation flow

The socket addresses remain necessary for startup, shutdown barriers, and as a
fallback control/data transport when `--use_ub_transport=false`. Start the two
commands close together because each process allocates its owner region and
then waits for the peer region to appear.

Use a unique region prefix for every run. Never reuse a prefix between
`one-sided` and `nocache`, or after an abnormal termination, until the old UBS
objects are known to have been released. Both hosts must use the same prefix,
mode, region size, server ordering, key count, and timing parameters. Their
`--id` and `UB_PROVIDER_HOST` differ.

### Step 1: standalone two-node correctness

For a first smoke test, set a region size compatible with the local OBMM block
configuration on both hosts:

On machine 81:

```bash
export UB_REGION_MB=128
export UB_PROVIDER_HOST=host-81
export UB_NUMA_NODE=1
export UB_PROVIDER_NUMA=1
```

On machine 82:

```bash
export UB_REGION_MB=128
export UB_PROVIDER_HOST=host-82
export UB_NUMA_NODE=0
export UB_PROVIDER_NUMA=0
```

Start machine 81 / coordinator 0 first; `192.0.2.10` is the example address on
which its TCP barrier listens:

```bash
bash tigon/scripts/run_ub_correctness.sh \
  0 192.0.2.10 one-sided correctness_001_os 10000 18951
```

Then start machine 82 / coordinator 1:

```bash
bash tigon/scripts/run_ub_correctness.sh \
  1 192.0.2.10 one-sided correctness_001_os 10000 18951
```

Both processes must finish with `PASS`. The test covers cross-node writer-lock
increments, placeholder conflict and publication, delete visibility, same-key
tombstone reuse, aborted-placeholder tombstones, concurrent requester inserts,
leaf/inner/root splits, ordered scan, queue-full backpressure, bidirectional
queue payload integrity, range successor/end-gap exclusion, and importer-first
cleanup.

Repeat on both hosts with a new prefix and `nocache`:

```bash
# Host 0
bash tigon/scripts/run_ub_correctness.sh \
  0 192.0.2.10 nocache correctness_001_nc 10000 18951

# Host 1
bash tigon/scripts/run_ub_correctness.sh \
  1 192.0.2.10 nocache correctness_001_nc 10000 18951
```

### Step 2: one YCSB point-operation run

For a manual `one-sided` RMW run using the UB message queue, start these
commands close together. Machine 81 / coordinator 0:

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --id=0 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_run_001 --ub_region_mb=4096 \
  --ub_provider_host=host-81 --ub_provider_numa=1 \
  --use_ub_transport=true --query=rmw --keys=200000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0
```

Machine 82 / coordinator 1:

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --id=1 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_run_001 --ub_region_mb=4096 \
  --ub_provider_host=host-82 --ub_provider_numa=0 \
  --use_ub_transport=true --query=rmw --keys=200000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0
```

For failure isolation, first run with `--use_ub_transport=false`. Tuples, locks,
and indexes still use UB, but Tigon messages use TCP. Then use a new prefix and
set `--use_ub_transport=true` to validate the UB queue.

### Step 3: staged YCSB matrix

The matrix script defaults to 12 combinations: two memory modes, three point
operations, and two message transports. Start with one combination on both
hosts:

```bash
export UB_REGION_MB=4096
export UB_TIGON_THREADS=1
export UB_TIGON_KEYS=200000
export UB_TIGON_RUN_SECONDS=20
export UB_TIGON_WARMUP_SECONDS=5
export UB_TIGON_MODES="one-sided"
export UB_TIGON_QUERIES="rmw"
export UB_TIGON_TRANSPORTS="false"
```

Keep the machine-specific `UB_PROVIDER_HOST`, `UB_PROVIDER_NUMA`, and
`UB_NUMA_NODE` values exported from Step 1.

Run the following concurrently, with the host-specific provider already set.
Host 0:

```bash
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_stage_tcp_001
```

Host 1:

```bash
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_stage_tcp_001
```

After that passes, set `UB_TIGON_TRANSPORTS=true` and use a new prefix base to
test the UB queue. Then run the full matrix:

```bash
export UB_TIGON_MODES="one-sided nocache"
export UB_TIGON_QUERIES="rmw insert delete"
export UB_TIGON_TRANSPORTS="false true"

# Host 0
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_matrix_001

# Host 1
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_matrix_001
```

The two host commands must run concurrently. The script-generated suffixes
make every mode/query/transport combination use a distinct region prefix.
Supported overrides are `TIGON_BUILD_DIR`, `UB_PROVIDER_HOST`,
`UB_PROVIDER_NUMA`, `UB_NUMA_NODE`, `UB_REGION_MB`, `UB_TIGON_THREADS`,
`UB_TIGON_KEYS`,
`UB_TIGON_RUN_SECONDS`, `UB_TIGON_WARMUP_SECONDS`, `UB_TIGON_MODES`,
`UB_TIGON_QUERIES`, and `UB_TIGON_TRANSPORTS`.

### Step 4: YCSB range and phantom protection

After all point cases pass, run range transactions separately so a failure is
not hidden in the larger matrix. The wrapper selects `scan mixed`; it otherwise
uses exactly the same arguments and environment as Step 3:

```bash
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"
export UB_TIGON_RANGE_QUERIES="scan"

# Run concurrently on host 0 and host 1.
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_scan_tcp_001
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_scan_tcp_001
```

Then test `mixed`, UB transport, and finally `one-sided nocache`, always with a
new prefix base. A correct run must not report an unlocked next row, duplicate
or out-of-order scan keys, or a committed insert inside a concurrently protected
gap.

### Step 5: TPCC primary and secondary indexes

TPCC consumes substantially more region space; the launcher defaults to 8 GiB
per owner region. First isolate one transaction type and TCP messaging:

```bash
export UB_REGION_MB=8192
export UB_TIGON_THREADS=1
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"
export UB_TPCC_QUERIES="payment"
export UB_TPCC_PAYMENT_DIST=100
export UB_TPCC_NEWORDER_DIST=100

# Run concurrently on host 0 and host 1.
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' tpcc_payment_tcp_001
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' tpcc_payment_tcp_001
```

Proceed in this order, using a fresh prefix at every stage: `payment`,
`neworder`, `first_two`, then `mixed`; TCP before UB queue; one-sided before
nocache. Startup and shutdown consistency checks must pass on both nodes. The
script accepts `UB_TPCC_QUERIES`, `UB_TPCC_PAYMENT_DIST`, and
`UB_TPCC_NEWORDER_DIST` in addition to the common matrix variables.

For `--threads=N`, each process also has a manager, an incoming dispatcher, an
outgoing dispatcher, and the coordinator main thread, so the primary Tigon
thread footprint is approximately `N + 4` CPUs. Machine 82 therefore gives a
theoretical ceiling near `N=16`, but `N=14` or `N=15` is a safer upper bound
that leaves some CPU capacity for UBS services and the OS. Scale in stages such
as 1, 4, 8, 12, then 14; use the same `UB_TIGON_THREADS` on both coordinators.

## Supported scope and troubleshooting

The currently machine-verified baseline remains the earlier point-operation
scope until Steps 4 and 5 are executed on the two UB hosts. The source now
contains transactional YCSB range/mixed handling, requester-side next-key/end-
gap protection, and UB B+ Tree adapters for all TPCC primary and secondary
tables. These additions passed host-only B+ Tree tests and template checks, not
a real UBS Memory run in this workspace. Cross-key tuple/node reclamation,
general N-node shutdown, and crash recovery/WAL replay remain unimplemented.

Common failures:

- `ubs_mem.h was not found`: correct `UBSM_INCLUDE_DIR`.
- `libubsm_sdk.so: cannot open shared object file`: inspect
  `LD_LIBRARY_PATH` and `ldd` output.
- `UBSM_ERR_IN_USING` (`6024`): an old process or mapping still references the
  object. Do not force-delete a live region; stop both old processes or use a
  fresh prefix while investigating cleanup.
- `UBSM_ERR_NOT_SUPPORTED` (`6025`) in `one-sided`: verify BIOS snoop/NC-CC and
  `/sys/bus/ub/ub_feature` compatibility.
- `UBSM_ERR_NET` (`6040`): inspect UBS node discovery and control-plane
  connectivity.
- `UBSM_ERR_UBSE` (`6050`): inspect `ubse.service`, OBMM resources, and UBS
  Engine logs.
- Peer mapping/header timeout: compare both hosts' prefix, mode, region size,
  coordinator count, provider hostname, and launch time. The same
  `--ub_map_timeout` deadline covers both SDK mapping and waiting for the peer
  to publish its region header.
- A fatal `cxlalloc_* was called while Tigon is using the UB backend` message:
  an unadapted legacy CXL path was selected, such as an unsupported workload.

## Lifecycle contract

Normal shutdown stops workers, message producers, and dispatchers, unmaps all
imported regions, executes a final TCP peer barrier, then unmaps/deallocates the
local owner region and calls `ubsmem_finalize()`. The coordinator performs this
ordering for the supported one-node/two-node YCSB path. General N-node shutdown,
crash cleanup, and replay are not provided and remain outside this milestone.
One-node runs skip the TCP peer barrier because no peer socket or imported
region exists.
