# UB two-copy RMW mode (experimental)

The main `tigon/` tree contains a gated UB-specific alternative to remote NC reads. Each UB
host would own a write-back (WB) copy of every YCSB tuple in its local region.
Readers lock and read only their local copy. A writer takes write locks on both
copies using one-sided atomics, writes the value to both regions, waits for UB
write completion, publishes the same version in both copies, and then unlocks.
It never caches a remote value in process-local DRAM.

The mode is now wired into the two-node `bench_ycsb --query=rmw` path through
`--ub_replicated_rmw=true` / `TIGON_UB_REPLICATED_RMW=1`. It remains disabled by
default; leave the environment variable unset or set it to `0` to select the
previous UB/CXL behavior. The CMake option `TIGON_BUILD_UB_REPLICA_PROTOTYPE`
(default `OFF`) builds a host-only protocol test. Keep replica and non-replica
campaigns separate; this mode has not yet passed two-host hardware validation.
For the complete build, sync, and smoke commands see
[`UB_REPLICATED_RMW_RUNBOOK.md`](UB_REPLICATED_RMW_RUNBOOK.md).

## Protocol implemented so far

`UBReplicatedTuple.h` provides a local shared-locked read and a two-copy
write guard. The guard checks both region generations and tuple layouts,
acquires both write locks without waiting (lowest region ID first), checks
`valid` and matching versions, and commits the same value and version to
both copies. If a lock is unavailable, the operation fails and the caller
must abort/retry its transaction. The non-blocking path avoids waiting while
holding another tuple lock. If write completion fails after either copy may
have changed, both locks remain held: the region is in doubt and must be
reconstructed, not reused.

The `UBReplicaWriteOps::complete()` callback is mandatory. The integrated
trial uses a CPU fence followed by a scalar readback/compare of the peer tuple
and, after publication, a peer version readback. It also omits
`UBSM_FLAG_WR_DELAY_COMP` when allocating replica-mode regions. This is a
conservative *trial*, not proof that peer writes have become visible in the
other host's WB mapping. If this mode cannot map on the target platform or
cross-host readback/visibility tests fail, do not run the formal matrix.

To run the isolated host test on a Linux development machine with a C++14
compiler, from the repository root:

```sh
g++ -std=c++14 -O2 -pthread -I tigon \
  tigon/tests/ub_replicated_tuple_test.cpp -o /tmp/ub_replicated_tuple_test
/tmp/ub_replicated_tuple_test
```

Or use the CMake option on a machine that already satisfies the normal Tigon
UB build dependencies (including the UBSM SDK):

```sh
cmake -S tigon -B /tmp/tigon-replica-build \
  -DTIGON_ENABLE_UB=ON -DTIGON_BUILD_UB_REPLICA_PROTOTYPE=ON
cmake --build /tmp/tigon-replica-build --target ub_replicated_tuple_test
ctest --test-dir /tmp/tigon-replica-build -R '^ub_replicated_tuple_test$' \
  --output-on-failure
```

## Two-node YCSB wiring and its limits

Each host creates an index and tuple copy for *every* YCSB partition in its
own UB region. Initial values are deterministic from the same fixed seed; the
remote copy is found by `(table, partition, key)` in the peer region, not by
assuming equal offsets. A read takes only the local tuple read lock. A write
takes both copies' write locks and retains them across the transaction. Failed
try-locks abort; data is written to all locked copies before versions are
published and locks released. After both hosts stop their workers, each scans
its local WB copy and logs `UB_REPLICA_CHECKSUM count=... hash=...`. The parser
separates `replicated_rmw=0` and `1` and excludes replica runs whose two host
checksums differ or are missing. The checksum excludes the internal `INT32_MAX`
next-key-locking sentinel in each partition: with two partitions and 300000
configured keys per partition, the expected count is 600000.

For fixed-key YCSB RMW, the first write to each local table builds a
process-local dense key-to-peer-tuple directory by scanning the peer's B+ tree
once. Later writes use the saved generation-tagged tuple offset instead of
traversing the remote tree. Every hit still resolves the current region
generation and checks the tuple key and valid bit; neither values nor locks
are cached. A miss or invalid entry falls back to the peer tree. This changes
neither two-copy locking nor version publication. The directory is enabled by
default only in replica RMW mode; set `TIGON_UB_REPLICA_LOOKUP_DIR=0` on both
hosts to use the old per-write peer lookup for an A/B run. The first scan can
be expensive, so retain the normal 30-second warmup for throughput runs.

The following remain hardware-validation gates or unsupported cases:

1. Prove remote NC write/readback ordering against the peer's WB owner mapping
   with both hosts writing. The readback is a check from the writer's importer
   mapping and may not suffice on all UB platforms. Do not interpret
   throughput before the cross-host correctness test passes.
2. Specify recovery for a process/host failure mid-commit, including an
   in-doubt marker and deterministic region rebuild. Also handle insert,
   delete, region generation change, and replica resynchronization. Until
   those are defined, scope the experiment to a fixed-key YCSB RMW dataset.
3. Add two-host correctness tests: concurrent write/write and read/write on
   the same key, multi-key abort, fault injection between data and version
   publication, reboot/rebuild, and checksum equality after each run. Only
   then enable comparative throughput measurements.

For an initial *smoke* on both UB hosts after syncing the same integrated Tigon revision and compiling
`bench_ycsb`, set identical campaign/point variables on both machines:

```sh
export TIGON_UB_REPLICATED_RMW=1
export TIGON_UB_RMW_TUPLE_CACHE=0
export UB_COMPARE_REPETITIONS=1 UB_COMPARE_RW_RATIOS='95' \
       UB_COMPARE_CROSS_RATIOS='0' UB_COMPARE_WARMUP_SECONDS=5 \
       UB_COMPARE_RUN_SECONDS=10
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/replica-smoke"
bash tigon/scripts/run_ub_two_node_compare.sh ycsb "$UB_ID" "$SERVERS" replica-smoke
```

Use a *new campaign* for the normal matrix only after both-host checks pass:

```sh
export UB_COMPARE_REPETITIONS=3 UB_COMPARE_RW_RATIOS='95 50' \
       UB_COMPARE_CROSS_RATIOS='0 25 50 75 100' \
       UB_COMPARE_WARMUP_SECONDS=30 UB_COMPARE_RUN_SECONDS=30
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/replica-formal"
bash tigon/scripts/run_ub_two_node_compare.sh ycsb "$UB_ID" "$SERVERS" replica-formal
```

Run both commands concurrently on the two hosts, using their respective
`UB_ID=0`/`1`. `TIGON_UB_REPLICATED_RMW=1` must be set on both. Do not reuse a
failed run's UB regions; remove only regions belonging to that campaign using
the normal cleanup procedure. The script refuses replica mode for non-RMW
YCSB and TPCC.

The expected benefit is replacing frequent remote NC index and tuple reads
with local WB reads. The cost is duplicate memory plus cross-host writes and
write-lock traffic. Read-heavy and low-conflict workloads are the most likely
to benefit; no performance claim is made yet.
