# Social Network UB reference mode

An opt-in bounded-domain adapter now exists for the Client/UserTimeline read
path. Default behavior remains the original RPC path. The adapter currently
supports a configured multi-Client registry and 1..3 nginx instance domains;
Client/Timeline/PostStorage use an optional versioned readiness handshake. It
requires the separate x86-64 KVM multi-domain device, **not ARM64 UB**; this is
not a completed true-UB end-to-end deployment. See
[scope, flags, diagnostics and sync commands](../../../eRPC-LRPC/docs/social-network-domain-integration.md).
An isolated two-VM UserTimeline packaging/acceptance runner is also available:
[QEMU runbook](../../../eRPC-LRPC/docs/social-network-qemu.md). It runs the actual
Client/Timeline/PostStorage path with a guest-local shared-file backend and
MongoDB fixture; local tests do not yet certify a successful full VM boot.

This application can run directly on two UB machines. Docker and the other
DeathStarBench containers are not required. The application requires the UB
runtime, the eRPC service binaries, and the MongoDB instances used to load the
initial social-network data.

The recommended first deployment is:

- machine 98, NUMA 0 (95 CPUs): MongoDB, timeline, and Post Storage services
- machine 99, NUMA 0 (95 CPUs): `client`, front-end, compose, and helper services
- one `erpc_ub_manager` process on each machine

Replace `IP98` and `IP99` in the commands below with the real reachable
addresses. The repository is assumed to be `/home/gxx/src/ub-application`,
and private dependencies are installed under `/home/gxx/.local`. The two
machines must use the same source revision,
routing addresses, RPC IDs, UB memory mode, region size, arena size, and region
prefix. The supplied machine-local JSON files keep each host's launch intent
explicit. Their `ERPC_UB_MACHINE_ID` values must be different and nonzero.

## Optional AArch64 UB localized-routing experiment

An opt-in UserTimeline-read adapter now lives in
[eRPC-LRPC-UB/SOCIAL_USER_READ.md](../../../eRPC-LRPC-UB/SOCIAL_USER_READ.md).
Use `ERPC_SN_DOMAIN_PROXY=ON` and `ERPC_SN_DOMAIN_BACKEND=ub` at build time;
normal UB-eRPC remains the default. This is not the x86/QEMU backend.
The initial experiment places Client/Timeline on A, keeps PostStorage on B,
and publishes read-only LB/nginx routing domains from B. A/B are experiment
roles, not aliases for machine numbers: keeping the original Storage host
means A=99 and B=98. Its RPC-only comparison keeps those exact service locations
and configurations, replacing B's routing domains with real LB/nginx processes.
The linked runbook includes both builds, two-host commands, DB connection/tunnel
configuration and log checks. It preserves the database startup,
data import, PostData consumption and handle ownership described here; no
Docker or VM image is involved. Application integration has local fixture and
cross-compilation coverage, not a completed real-UB end-to-end validation.
Follow that runbook for the module/owner update, standalone routing probe,
private configurations, two-host startup, log acceptance, and RPC comparison.

Real-data measurements use `ERPC_SN_DOMAIN_FIXTURE=0` in both paths; VM fixture
validation is opt-in, not a prerequisite for measuring UserTimeline reads.
Timeline/Storage database read failures now emit `SN_MONGODB_ERROR` and stop
before readiness; an empty successful query remains valid. Auxiliary CPU
allocation supports independent NUMA node IDs (including 2/3) and asymmetric
CPU counts. Rebuild the affected application binaries after updating; these
review fixes do not change the UB gate module or require a database restart.

## Data path and ownership

Requests are carried directly in eRPC `MsgBuffer` objects. Every intermediate
service reads the incoming UB payload and copies it into a `MsgBuffer` owned by
the outgoing endpoint before forwarding it. The transport does not forward a
borrowed remote `MsgBuffer`.

Home/User Timeline read handlers copy their small incoming request to a local
MsgBuffer before handing it to the worker. This avoids an extra remote
reference increment in the handler and remote decrement in the worker; the
transport still owns and releases the original borrowed request normally.

Post Storage keeps the index in its process-local map and stores every
`PostData` object in its shared UB arena. A read response carries a 32-byte
`{machine_id, block_offset, payload_offset, payload_length}` handle. Post
Storage transfers one reference with every returned handle. Intermediate
services copy the handle only. The final client fetches the contiguous 104-byte
metadata prefix, then consumes every effective field directly from fixed
compile-time offsets in the imported object
and retains a checksum before stopping the Timeline timer. Reference release
is outside the timer. The former fixed 1300-byte copy is no longer used.

`PostData` layout version 4 is exactly **2048 bytes**, enforced by
`static_assert`. Every field has a compile-time fixed position: text capacity
is 1168 bytes, creator/mention usernames 16 bytes each, 10 media types 4 bytes
each, 5 shortened URLs 32 bytes each, and 5 expanded URLs 72 bytes each.
Lengths and counts are stored in the contiguous metadata prefix. Strings keep
a local trailing NUL for service code, but the NUL and unused capacity are not
consumed by the Timeline checksum. There are no string offsets or shared string
pool, so a remote reader does not perform an offset-dependent lookup.

Both UB and DmRPC clients now generate **500 B** of initial Compose text,
then append the existing 5 mentions and 5 URLs and retain 10 media. With
three-digit generated user IDs the final text is 921 B. The complete fixture
fits every field without dropping data.
Oversized input fails explicitly; no truncation or automatic spill allocation.
Both initialization scripts now use the same 500-byte initial text for newly
generated datasets. Existing MongoDB documents are not rewritten. Post Storage
fills the fixed arrays and lengths when loading them. Real dataset capacity still
needs validation at startup; BSON size alone is not an exact capacity test.

Rebuild all UB application services together and recreate their in-memory posts.
The 32-byte Timeline handle is unchanged. The Compose RPC envelope is larger
than the 2048-byte Post itself, so a request may still use a 4 KiB allocator
size class. Stored posts, pregenerated requests and RPC buffers all count
toward `ERPC_UB_ARENA_MB` and `ERPC_UB_REGION_MB`; object size is distinct
from allocator capacity and the effective bytes consumed.

The launcher therefore defaults to a **64 MiB arena per endpoint** and a
**2048 MiB machine region**. A stored 2048-byte `PostData` uses the allocator's
2048-byte size class plus one 64-byte block header, so a 64 MiB arena holds at
most about 31,774 such objects after the arena header. Leave additional
headroom for RPC buffers. Check the source dataset size before launch:

```bash
mongosh --port 20014 --quiet --eval \
  'db.getSiblingDB("post").post.countDocuments({})'
```

If the result is near or above 30,000, increase `ERPC_UB_ARENA_MB` on **both**
machines. The machine region must also cover every endpoint's inbox and arena;
for example, use `ERPC_UB_ARENA_MB=128` with `ERPC_UB_REGION_MB=4096`, subject
to the available UB shared-memory capacity. Both values are fixed when the UB
manager creates its region, so stop all workers and both managers before
changing them, then start both launchers again.

Checksum rules, changed files and validation commands:
[POST_CONSUMPTION.md](POST_CONSUMPTION.md).

## Application breakdown profiling

Set `ERPC_SN_PROFILE=1` on both hosts before running `run_ub.sh` to enable
application-level timing. The launcher passes this environment variable to
its services. Keep `ERPC_UB_PROFILE` unset unless transport profiling is also
needed; enabling both adds more overhead. For final latency numbers, unset
both variables and rerun the same workload.

Each service writes `SN_UB_PROFILE` lines to its own log. Compare lines with
the same `thread` and `interval` fields; a report boundary can split a stage's
call count. A thread reports
after every 100,000 stage samples and once at normal thread exit. Do not use
`kill -9` for a short profiling run, as it prevents the final report. Each line
contains `stage`, `calls`, raw `avg_ns`, `p99_upper_ns`, and `max_ns`.
`p99_upper_ns` is the upper edge of a power-of-two bucket, not an exact
percentile. A separate line gives
the minimum observed `timestamp_overhead_ns` for two clock reads. The averages
include timestamp overhead and should not be treated as uninstrumented latency.

- `proxy_forward_*` and `proxy_reverse_*` in the Load Balance and Nginx logs
  separate request/response pinning, queue insertion, outgoing-buffer
  preparation, RPC enqueue, and completion-time release. Their
  `queue_handoff` stages measure server-thread push to client-RPC-thread pop,
  including scheduling delay.
- `timeline_rx_*` and `timeline_worker_*` in the Home/User Timeline logs
  separate receive-side handoff from post-ID lookup plus result construction.
  UB read handlers clone the small request into a local MsgBuffer before
  enqueueing it; `timeline_worker_release` frees that clone. The experimental
  Timeline inline queue was removed after showing no latency benefit. Unset
  `ERPC_SN_TIMELINE_INLINE` in older run scripts; it is no longer supported.
  `timeline_queue_handoff` measures server-thread push to worker-thread pop;
  `timeline_forward_queue_handoff` measures worker-thread push to client-RPC-
  thread pop. They include scheduling delay, not just queue instructions.
  `timeline_tx_*` measures outgoing Post Storage setup, while
  `timeline_callback_*` measures response handling and upstream forwarding.
- `timeline_storage_rtt` starts before the Timeline enqueues its Post Storage
  RPC and ends at its callback. It includes transport, Post Storage processing,
  and event-loop scheduling; it is not additive with the service-local stages.
- `storage_read_*` separates map-lock acquisition, lookup, reference retain,
  response build, and response enqueue. `storage_read_response` contains its
  build/enqueue stages and must not be added to them. `storage_write_*`
  separates allocation, the 2 KiB
  remote-to-local copy, validation, map lock/lookup/insert, and response
  construction/enqueue. `storage_write_map` and `storage_write_response` are
  enclosing totals and must not be added to their nested stages.
- `client_import`, `client_metadata`, and `client_fields` cover mapping/object
  resolution and effective-field consumption before the Timeline timer stops.
  `client_release` happens after that timer stops. Import/field stages have
  fewer calls than total reads when a post is not found.

To correlate tail samples across services, enable the separate diagnostic
trace on **both UB hosts** before starting `run_ub.sh`:

```bash
export ERPC_SN_SLOW_TRACE=1
export ERPC_SN_SLOW_STAGE_NS=5000
export ERPC_SN_SLOW_E2E_NS=50000
unset ERPC_SN_PROFILE
unset ERPC_SN_STORAGE_WRITE_PROBE
```

After a graceful stop, grep `SN_SLOW_TRACE` in Client, Load Balance and Nginx
(host 99), and Home/User Timeline and Post Storage (host 98) logs; join entries
by application `req_number` and stage name. The proxy queue-handoff traces
currently use eRPC packet sequence numbers instead, so do not join those
entries to Client/Timeline/Post Storage events by numeric ID. The trace now
includes `client_import` and Post Storage read-side `storage_read_lock`,
`storage_read_lookup`, `storage_read_retain`, `storage_read_response_build`,
and `storage_read_response_enqueue`.
`client_*_e2e` uses the application's existing microsecond-resolution timer;
individual stages use nanosecond wall and thread-CPU clocks. For stages with
`cpu_valid=1`, a large `offcpu_ns` indicates the thread was not scheduled for
most of that stage; `wall_ns` near `cpu_ns` instead indicates on-CPU waiting or
work, not necessarily useful computation. Queue handoffs and end-to-end samples
span threads, so they have `cpu_valid=0`. Trace events are buffered per thread
and printed at normal thread exit; a forced `kill -9` loses them. The trace
adds clock reads and is for diagnosis, not final latency reporting. Disable it
and rerun the same workload for publication-quality percentiles.

To diagnose the physical source of `storage_write_copy`, enable this only on
the Post Storage host before starting `run_ub.sh`:

```bash
export ERPC_SN_STORAGE_WRITE_PROBE=1
```

The first write prints one `SN_UB_STORAGE_WRITE_PROBE` line containing the
request backing/payload/PostData addresses, source and destination alignment,
`source_is_local`, request/PostData sizes, and two consecutive 2 KiB copy
times. For the documented machine-99 Client to machine-98 Post Storage
placement, `local_machine_id=98` and `source_is_local=0` are required. The
second copy and full local checksum deliberately perturb the first request, so
use this probe only for diagnosis and unset it for reported latency runs.

These are per-process/per-thread measurements; do not subtract timestamps
from different machines to infer one-way network latency. Profile one request
type at a time if stage averages must be attributed to that type, because the
proxy stages otherwise aggregate all request types.

## 1. Prerequisites

Both machines need:

- a working UB/UBSM installation and `libubsm_sdk.so`
- CMake and a C++17 compiler
- matching Protobuf libraries and `protoc`, version 3.12 or later
- gflags and libnuma
- pkg-config, libmongoc-1.0, and libbson-1.0
- the header-only nlohmann/json library (`nlohmann/json.hpp`)

Check the build dependencies:

```bash
pkg-config --modversion libmongoc-1.0
pkg-config --modversion libbson-1.0
pkg-config --libs libmongoc-1.0
pkg-config --libs libbson-1.0
protoc --version
test -f /usr/include/nlohmann/json.hpp || \
  test -f /home/gxx/.local/include/nlohmann/json.hpp
```

The schema uses proto3 `optional`. `protoc` 3.12-3.14 requires
`--experimental_allow_proto3_optional`; the current application CMake rule
passes it explicitly, and newer protoc releases also accept it. If an existing
build reports that this flag was not set, update `eRPC/CMakeLists.txt`, rerun
the same CMake configure command for that build directory, then rebuild.
Keep `optional` in the schema and use the matching compiler, headers and
libraries from one Protobuf installation. `Protobuf_PROTOC_EXECUTABLE` in
`CMakeCache.txt` identifies the compiler actually selected by CMake.

On Ubuntu or Debian, install the JSON header with:

```bash
sudo apt-get install nlohmann-json3-dev
```

Without root access, install it under the same private prefix used by the other
dependencies:

```bash
cd /home/gxx/src
git clone --depth 1 --branch v3.11.3 \
  https://github.com/nlohmann/json.git nlohmann-json
cmake -S nlohmann-json -B nlohmann-json/build \
  -DJSON_BuildTests=OFF \
  -DCMAKE_INSTALL_PREFIX=/home/gxx/.local
cmake --install nlohmann-json/build
test -f /home/gxx/.local/include/nlohmann/json.hpp
```

If the dependencies are installed under a private prefix, export it before
configuring CMake. For example:

```bash
export SN_DEPS=/home/gxx/.local
export PATH="$SN_DEPS/bin:$PATH"
export LD_LIBRARY_PATH="$SN_DEPS/lib:$SN_DEPS/lib64:/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SN_DEPS/lib/pkgconfig:$SN_DEPS/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$SN_DEPS:${CMAKE_PREFIX_PATH:-}"
```

When only the UBSM library is outside the default loader path, this is enough:

```bash
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
```

Before launching the full application, first verify that `hello_world_ub`
works between the same two machines. Also check that the selected NUMA node
has enough logical CPUs for the offsets in the machine-local UB configuration:

```bash
numactl -H
lscpu -e=CPU,NODE
```

The deployment below assigns local core indices 0-13 on machine 98 and 0-30
on machine 99. Both ranges fit within the 95 CPUs available on NUMA node 0.

## 2. Build on both machines

Run from the `eRPC` directory on both machines:

```bash
cd /home/gxx/src/ub-application/eRPC

printf '%s\n' social_network_cxl > scripts/autorun_app_file

cmake -S . -B cmake-build-social-ub \
  -DTRANSPORT=ub \
  -DPERF=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build cmake-build-social-ub -j \
  --target erpc_ub_manager \
  client load_balance nginx compose_post \
  unique_id url_shorten user_mention user_service \
  user_timeline home_timeline post_storage
```

Do not skip the preceding CMake configure command: CMake registers the
`erpc_ub_manager` target while configuring a `TRANSPORT=ub` build tree.

The repository CMake configuration places the executables in `eRPC/build`,
which is also the default directory used by `run_ub.sh`.

Check that no runtime library is missing:

```bash
ldd build/client | grep 'not found' || true
ldd build/post_storage | grep 'not found' || true
test -x build/erpc_ub_manager
test -x build/client
test -x build/post_storage
```

## 3. Start MongoDB on machine 98

No Docker container is needed. The current code opens these MongoDB instances:

| Port | Configuration entry | Database/collection read by the code |
| --- | --- | --- |
| 20011 | `user_mongodb` | `user.user` |
| 20012 | `user_timeline_mongodb` | `user-timeline.user-timeline` |
| 20014 | `post_storage_mongodb` | `post.post` |

`social_graph_mongodb` on port 20013 is present in the configuration and in
the original exported dataset, but the current UB application does not open a
connection to it. It may be started and restored for dataset compatibility,
but it is not required by the current request paths.

Machine 99 does not run `mongod` or require the MongoDB database tools. All
three database instances run on machine 98. User Mention runs on machine 99
and reads port 20011 remotely during initialization, so port 20011 must bind to
`IP98` and be reachable from machine 99. Ports 20012 and 20014 are used only
by services on machine 98 and may remain localhost-only.

```bash
export SN_MONGO_ROOT=/path/to/mongodb_data
mkdir -p \
  "$SN_MONGO_ROOT/user" \
  "$SN_MONGO_ROOT/user_timeline" \
  "$SN_MONGO_ROOT/post_storage"

mongod --port 20011 \
  --dbpath "$SN_MONGO_ROOT/user" \
  --logpath "$SN_MONGO_ROOT/user/mongod.log" \
  --bind_ip 127.0.0.1,IP98 --fork

mongod --port 20012 \
  --dbpath "$SN_MONGO_ROOT/user_timeline" \
  --logpath "$SN_MONGO_ROOT/user_timeline/mongod.log" \
  --bind_ip 127.0.0.1 --fork

mongod --port 20014 \
  --dbpath "$SN_MONGO_ROOT/post_storage" \
  --logpath "$SN_MONGO_ROOT/post_storage/mongod.log" \
  --bind_ip 127.0.0.1 --fork
```

If the original four-database layout is desired, port 20013 may also be
started on machine 98:

```bash
mkdir -p "$SN_MONGO_ROOT/social_network"
mongod --port 20013 \
  --dbpath "$SN_MONGO_ROOT/social_network" \
  --logpath "$SN_MONGO_ROOT/social_network/mongod.log" \
  --bind_ip 127.0.0.1 --fork
```

Import the provided DeathStarBench archives once, if they have not already
been restored. Set `SN_MONGO_EXPORT` to the directory containing the archives:

Run all restore commands on machine 98:

```bash
export SN_MONGO_EXPORT=/path/to/mongodb_export

mongorestore --host 127.0.0.1 --port 20011 \
  --gzip --archive="$SN_MONGO_EXPORT/user_archive.gz"
mongorestore --host 127.0.0.1 --port 20012 \
  --gzip --archive="$SN_MONGO_EXPORT/user_timeline_archive.gz"
mongorestore --host 127.0.0.1 --port 20014 \
  --gzip --archive="$SN_MONGO_EXPORT/post_archive.gz"
```

The optional social-graph archive is restored with:

```bash
mongorestore --host 127.0.0.1 --port 20013 \
  --gzip --archive="$SN_MONGO_EXPORT/social_graph_archive.gz"
```

Verify all required listeners and data on machine 98:

```bash
ss -ltnp | grep -E ':20011|:20012|:20014'
mongosh --host 127.0.0.1 --port 20011 \
  --quiet --eval 'db.getSiblingDB("user").user.countDocuments({})'
mongosh --host 127.0.0.1 --port 20012 \
  --quiet --eval 'db.getSiblingDB("user-timeline")["user-timeline"].countDocuments({})'
mongosh --host 127.0.0.1 --port 20014 \
  --quiet --eval 'db.getSiblingDB("post").post.countDocuments({})'
```

An empty database can allow the processes to start, but timeline reads will
not produce representative results. Use the restored dataset for performance
measurements.

## 4. Create the two-machine configurations

The repository provides one file for each machine:

```text
apps/social_network_cxl/config/config.ub98.json
apps/social_network_cxl/config/config.ub99.json
```

Their routing and RPC ID fields are identical, and both use NUMA node 0.
Replace the `IP98` and `IP99` placeholders with the machines' real reachable
addresses. The configured placement is:

| Service | `server_addr` | `rpc_id` | `bind_core_offset` |
| --- | --- | ---: | ---: |
| `client` | `IP99:31851` | 1 | 0 |
| `load_balance` | `IP99:31850` | 0 | 4 |
| `nginx` | `IP99:31852` | 2 | 7 |
| `compose_post` | `IP99:31855` | 5 | 10 |
| `unique_id` | `IP99:31853` | 3 | 14 |
| `url_shorten` | `IP99:31854` | 4 | 18 |
| `user_service` | `IP99:31860` | 10 | 22 |
| `user_mention` | `IP99:31858` | 8 | 26 |
| `user_timeline` | `IP98:31856` | 6 | 0 |
| `home_timeline` | `IP98:31857` | 7 | 5 |
| `post_storage` | `IP98:31859` | 9 | 10 |

Do not leave these addresses as `127.0.0.1`. They are eRPC control-plane
addresses and must be bindable on the hosting machine and reachable by the
other machine. Set `user_mongodb.addr` to `IP98` because User Mention runs on
machine 99. Keep the other MongoDB addresses as `127.0.0.1`.

In both `config.ub98.json` and `config.ub99.json`, place all eRPC client,
server, worker, and Nexus threads on NUMA node 0:

```json
"numa_client_node": 0,
"numa_server_node": 0
```

The per-service `bind_core_offset` is an index within the selected NUMA node,
not a global Linux CPU ID. Binding of data-plane threads is deterministic and
does not depend on thread creation order. A service reserves consecutive CPUs:

```text
client RPC threads: base ... base + client_num - 1
server RPC threads: base + client_num ... base + client_num + server_num - 1
worker threads:     base + client_num + server_num ...
```

With the configured single client/server thread, worker services use
`base+0`, `base+1`, and `base+2`; services without a worker use `base+0` and
`base+1`. The configured service blocks do not overlap: data-plane threads use
local core indices 0-28 on machine 99 and 0-11 on machine 98. Leader threads
block while joining workers, MongoDB initialization threads exit before the
measurement, and Nexus session-management threads block on UDP control-plane
input. CPUs remain available for the manager, MongoDB, and the OS.

The RPC IDs must be globally unique in the range 0-63. With
`server_num=client_num=1`, the service server endpoints use the configured IDs
0-10 and their internal client endpoints use IDs 20-30.

Check the edited configuration:

```bash
grep -n 'numa_.*_node\|server_addr\|rpc_id\|_mongodb\|"addr"\|"port"' \
  apps/social_network_cxl/config/config.ub98.json
grep -n 'numa_.*_node\|server_addr\|rpc_id\|_mongodb\|"addr"\|"port"' \
  apps/social_network_cxl/config/config.ub99.json
```

## 5. Start services on machine 98

Start machine 98 first. Run this command from a dedicated terminal or `tmux`
pane; `run_ub.sh` remains in the foreground and writes per-process logs.

```bash
cd /home/gxx/src/ub-application/eRPC

export LD_LIBRARY_PATH="/home/gxx/.local/lib:/home/gxx/.local/lib64:/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export ERPC_UB_MACHINE_ID=98
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_REGION_MB=2048
export ERPC_UB_ARENA_MB=64

export SN_BUILD_DIR="$(pwd)/build"
export SN_CONFIG="$(pwd)/apps/social_network_cxl/config/config.ub98.json"
export SN_LOG_DIR="$(pwd)/apps/social_network_cxl/logs/ub-98"
export SN_SERVICES="post_storage user_timeline home_timeline"

numactl --cpunodebind=0 --membind=0 \
  ./apps/social_network_cxl/run_ub.sh
```

The expected default region name is:

```text
erpc_ub_rx_0000000000000062
```

## 6. Start client and front-end services on machine 99

After the services on machine 98 have started, run on machine 99:

```bash
cd /home/gxx/src/ub-application/eRPC

export LD_LIBRARY_PATH="/home/gxx/.local/lib:/home/gxx/.local/lib64:/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export ERPC_UB_MACHINE_ID=99
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_REGION_MB=2048
export ERPC_UB_ARENA_MB=64

export SN_BUILD_DIR="$(pwd)/build"
export SN_CONFIG="$(pwd)/apps/social_network_cxl/config/config.ub99.json"
export SN_LOG_DIR="$(pwd)/apps/social_network_cxl/logs/ub-99"
export SN_SERVICES="client load_balance nginx compose_post unique_id url_shorten user_service user_mention"

numactl --cpunodebind=0 --membind=0 \
  ./apps/social_network_cxl/run_ub.sh
```

The expected default region name is:

```text
erpc_ub_rx_0000000000000063
```

A hostname-derived hash in the region name means that
`ERPC_UB_MACHINE_ID` did not reach the launched manager or worker process.

If the UBSM provider requires nondefault connection settings, export the same
working values used by `hello_world_ub`, such as `ERPC_UB_PROVIDER_HOST`,
`ERPC_UB_PROVIDER_SOCKET`, or `ERPC_UB_PROVIDER_PORT`, before invoking the
launcher.

## 7. Logs and startup checks

On machine 99:

```bash
tail -f apps/social_network_cxl/logs/ub-99/client.log
tail -f apps/social_network_cxl/logs/ub-99/erpc_ub_manager.log
```

On machine 98:

```bash
tail -f apps/social_network_cxl/logs/ub-98/user_timeline.log
tail -f apps/social_network_cxl/logs/ub-98/post_storage.log
tail -f apps/social_network_cxl/logs/ub-98/erpc_ub_manager.log
```

Search all local logs for common failures:

```bash
grep -RniE 'error|failed|assert|6050|UBSM_ERR|not connected' \
  apps/social_network_cxl/logs/ub-*
```

The control-plane ports must also be reachable between the two machines. The
application uses ports 31850-31860 from the JSON configuration.

## 8. Select a benchmark workload

`generate_num` controls the number of generated requests.
`user_num`, `home_num`, and `write_num` determine the request-type ratio.
Use the following values to isolate one request path:

| Workload | `user_num` | `home_num` | `write_num` |
| --- | ---: | ---: | ---: |
| User Timeline Read | 1 | 0 | 0 |
| Home Timeline Read | 0 | 1 | 0 |
| Compose Post | 0 | 0 | 1 |

For example, a User Timeline Read run can use:

```json
"client": {
  "server_addr": "IP99:31851",
  "rpc_id": 1,
  "generate_num": 5000,
  "user_num": 1,
  "home_num": 0,
  "write_num": 0,
  "bind_core_offset": 0
}
```

Restart the application processes after changing the JSON file. Recompilation
is not required.

## 9. Non-cache mode

To test the all-non-cache mapping mode, set this on both machines before
launching:

```bash
export ERPC_UB_MEMORY_MODE=nocache
```

Do not use spellings such as `non-cache`. Both machines must use the same mode.

## 9.1 Timeline post read layout

The UB client supports two remote `PostData` consumption layouts. The default
reads the 104-byte metadata header followed only by fields whose recorded
length/count says they are present:

```bash
export ERPC_SN_POST_READ_MODE=fields
```

To compare sparse field access with a contiguous remote access stream, select:

```bash
export ERPC_SN_POST_READ_MODE=contiguous
```

`contiguous` still materializes and validates the 104-byte header, then
consumes the remainder of the fixed 2048-byte `PostData` layout sequentially
with eight independent scalar accumulation lanes. On UB AArch64 and x86-64,
each 8-byte payload read uses an explicit scalar load instruction; this avoids
compiler conversion of the loop into wide/vector loads. Other architectures
fall back to the previous 8-byte `memcpy` expression. It does not copy the
object into a local 2 KiB buffer. The
two modes therefore compare fewer but potentially scattered remote bytes with
more but contiguous remote bytes. Set the variable before starting `run_ub.sh`;
the client process reads it once. Restart the processes to change modes.

With `ERPC_SN_PROFILE=1`, compare `client_fields` between modes. Contiguous
runs additionally report `client_fields_contiguous`; the individual
username/text/media/mentions/URL stages are present only in `fields` mode.

## 9.2 UB proxy small-message A/B

On machine 99, `ERPC_SN_PROXY_INLINE=0` (or unset) keeps the existing
LB/Nginx borrowed-MsgBuffer forwarding path. Set `ERPC_SN_PROXY_INLINE=1`
before `run_ub.sh` to copy only User/Home Timeline requests and their small
PostStorage-reference responses into local queue values. The proxy sender
thread then allocates its own outgoing MsgBuffer. This avoids retaining and
releasing a remote borrowed buffer across the proxy thread boundary. Ping and
Compose Post messages keep the existing path; service hops and response
semantics are unchanged. Restart LB and Nginx after changing the variable.
Confirm `SN_UB_PROXY_INLINE=0|1` in both `logs/ub-99/load_balance.log` and
`logs/ub-99/nginx.log` before comparing results.

The default queue-drain limit is `15360` for `ERPC_SN_PROXY_INLINE=0`, matching
the pre-inline code, and `8` for `ERPC_SN_PROXY_INLINE=1`. Set
`ERPC_SN_PROXY_POLL_BATCH=8` in both modes when testing whether a bounded
drain helps tail latency, or set it to `15360` in both modes to isolate the
inline-copy change. The allowed range is `1..15360`; confirm the selected
`SN_UB_PROXY_POLL_BATCH` value in both proxy logs. Unset
`ERPC_SN_SLOW_TRACE`, `ERPC_SN_PROFILE`, and `ERPC_UB_PROFILE` for final
end-to-end latency measurements.

For A/B, use the same `ERPC_SN_POST_READ_MODE`, pure-read workload, CPU/NUMA
binding, request count, and binaries. Run once with `ERPC_SN_PROFILE=1` to
compare proxy breakdown stages, then unset profiling on both hosts and compare
end-to-end P50/P99. In inline mode `proxy_*_rx_pin` measures the small local
copy, while `proxy_*_tx_prepare` includes materializing the sender-owned
MsgBuffer; the stage names are retained for A/B log compatibility.

## 9.3 UB Post Storage write-copy A/B

On machine 98, `ERPC_SN_STORAGE_WRITE_COPY_MODE=memcpy` (or unset) preserves
the existing 2048-byte remote-to-local `memcpy`. Set it to `scalar` to copy the
same bytes using individual 8-byte source loads and local stores. Both modes
validate and publish the same `PostData`; this switch does not change the
wire format or the read path. Restart Post Storage after changing it, and
confirm `SN_UB_STORAGE_WRITE_COPY_MODE=memcpy|scalar` in `post_storage.log`.
The source inside the packed request may be unaligned, so the scalar path
handles the leading/trailing bytes before and after aligned word loads.

Run the same write workload in both modes with
`ERPC_SN_STORAGE_WRITE_PROBE`, `ERPC_SN_SLOW_TRACE`, `ERPC_SN_PROFILE`, and
`ERPC_UB_PROFILE` unset for end-to-end P50/P90/P99. A separate diagnostic run
with `ERPC_SN_SLOW_TRACE=1` can compare `storage_write_copy` with the
unchanged response stages; its latency histogram is not the final A/B result.
The scalar-copy microbenchmark used an aligned source, so application results
must be measured rather than inferred from it. A local byte-equivalence check
can be run before deploying:

```bash
c++ -std=c++11 -Wall -Wextra -Werror -pedantic \
  apps/social_network_cxl/tests/post_copy_test.cpp -o /tmp/post_copy_test
/tmp/post_copy_test
```

## 10. Shutdown and cleanup

Stop worker processes before stopping their local `erpc_ub_manager`. A graceful
shutdown gives sessions time to disconnect and remote mappings time to unmap.
The current convenience launcher owns both the manager and workers; pressing
Ctrl-C asks all of them to exit, so always inspect the manager log for a clean
region teardown afterward. Do not manually remove `/dev/obmm_shmdev*` objects.

During manager shutdown, region deletion retries `UBSM_ERR_IN_USING` for 30
seconds by default. Stop the launchers on both machines within this window so
that each peer can release its imported mapping. The timeout and retry interval
can be adjusted on both machines before launch:

```bash
export ERPC_UB_SHUTDOWN_TIMEOUT_MS=30000
export ERPC_UB_SHUTDOWN_RETRY_MS=500
```

Other deletion errors are not retried. If the timeout expires, `run_ub.sh`
reports that manager cleanup failed and the manager log contains the exact
region name and UBSM error code.

After all application processes have exited, stop MongoDB on machine 98:

```bash
mongod --dbpath "$SN_MONGO_ROOT/user" --shutdown
mongod --dbpath "$SN_MONGO_ROOT/user_timeline" --shutdown
mongod --dbpath "$SN_MONGO_ROOT/post_storage" --shutdown
```

If port 20013 was started, also run:

```bash
mongod --dbpath "$SN_MONGO_ROOT/social_network" --shutdown
```

After an abnormal application exit, query a region by its exact name before
performing any explicit cleanup:

```bash
./tests/ubs-mem-two-node/build-ubsm/ubsm_shm_admin \
  --query erpc_ub_rx_0000000000000062
./tests/ubs-mem-two-node/build-ubsm/ubsm_shm_admin \
  --query erpc_ub_rx_0000000000000063
```

Only remove a stale region after confirming that no process still maps it.
