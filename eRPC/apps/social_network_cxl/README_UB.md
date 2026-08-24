# Social Network UB reference mode

This application can run directly on two UB machines. Docker and the other
DeathStarBench containers are not required. The application requires the UB
runtime, the eRPC service binaries, and the MongoDB instances used to load the
initial social-network data.

The recommended first deployment is:

- machine 81: `client`
- machine 82: all ten service processes and MongoDB
- one `erpc_ub_manager` process on each machine

Replace `IP81`, `IP82`, and `/path/to/ub-application` in the commands below
with the real values. The two machines must use the same source revision,
configuration file, UB memory mode, region size, arena size, and region prefix.
Their `ERPC_UB_MACHINE_ID` values must be different and nonzero.

## Data path and ownership

Requests are carried directly in eRPC `MsgBuffer` objects. Every intermediate
service reads the incoming UB payload and copies it into a `MsgBuffer` owned by
the outgoing endpoint before forwarding it. The transport does not forward a
borrowed remote `MsgBuffer`.

Post Storage keeps the index in its process-local map and stores every
`PostData` object in its shared UB arena. A read response carries a 32-byte
`{machine_id, block_offset, payload_offset, payload_length}` handle. Post
Storage transfers one reference with every returned handle. Intermediate
services copy the handle only; the final client copies the complete `PostData`
and releases the transferred reference.

## 1. Prerequisites

Both machines need:

- a working UB/UBSM installation and `libubsm_sdk.so`
- CMake and a C++17 compiler
- Protobuf and `protoc`
- gflags and libnuma
- pkg-config, libmongoc-1.0, and libbson-1.0

Check the build dependencies:

```bash
pkg-config --modversion libmongoc-1.0
pkg-config --modversion libbson-1.0
protoc --version
```

If the dependencies are installed under a private prefix, export it before
configuring CMake. For example:

```bash
export SN_DEPS=/path/to/social-network-deps
export PATH="$SN_DEPS/bin:$PATH"
export LD_LIBRARY_PATH="$SN_DEPS/lib:/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SN_DEPS/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$SN_DEPS:${CMAKE_PREFIX_PATH:-}"
```

When only the UBSM library is outside the default loader path, this is enough:

```bash
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
```

Before launching the full application, first verify that `hello_world_ub`
works between the same two machines. Also check that the selected NUMA node
has enough logical CPUs for the offsets in `config.ub.json`:

```bash
numactl -H
lscpu -e=CPU,NODE
```

The current configuration uses `bind_core_offset` values through 40. Reduce
or redistribute these offsets if the selected NUMA node does not contain that
many usable logical CPUs.

## 2. Build on both machines

Run from the `eRPC` directory on both machines:

```bash
cd /path/to/ub-application/eRPC

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

## 3. Start MongoDB on machine 82

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

When all database-using services run on machine 82, bind MongoDB to localhost:

```bash
export SN_MONGO_ROOT=/path/to/mongodb_data
mkdir -p \
  "$SN_MONGO_ROOT/user" \
  "$SN_MONGO_ROOT/user_timeline" \
  "$SN_MONGO_ROOT/post_storage"

mongod --port 20011 \
  --dbpath "$SN_MONGO_ROOT/user" \
  --logpath "$SN_MONGO_ROOT/user/mongod.log" \
  --bind_ip 127.0.0.1 --fork

mongod --port 20012 \
  --dbpath "$SN_MONGO_ROOT/user_timeline" \
  --logpath "$SN_MONGO_ROOT/user_timeline/mongod.log" \
  --bind_ip 127.0.0.1 --fork

mongod --port 20014 \
  --dbpath "$SN_MONGO_ROOT/post_storage" \
  --logpath "$SN_MONGO_ROOT/post_storage/mongod.log" \
  --bind_ip 127.0.0.1 --fork
```

If the original four-database layout is desired, also start port 20013:

```bash
mkdir -p "$SN_MONGO_ROOT/social_network"
mongod --port 20013 \
  --dbpath "$SN_MONGO_ROOT/social_network" \
  --logpath "$SN_MONGO_ROOT/social_network/mongod.log" \
  --bind_ip 127.0.0.1 --fork
```

Import the provided DeathStarBench archives once, if they have not already
been restored. Set `SN_MONGO_EXPORT` to the directory containing the archives:

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

Verify that the required listeners and data exist:

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

## 4. Create the shared two-machine configuration

Create one configuration and copy the identical file to both machines:

```bash
cd /path/to/ub-application/eRPC
cp apps/social_network_cxl/config/config.json \
  apps/social_network_cxl/config/config.ub.json
```

Edit `apps/social_network_cxl/config/config.ub.json` with this placement:

| Service | `server_addr` | `rpc_id` |
| --- | --- | --- |
| `load_balance` | `IP82:31850` | 0 |
| `client` | `IP81:31851` | 1 |
| `nginx` | `IP82:31852` | 2 |
| `unique_id` | `IP82:31853` | 3 |
| `url_shorten` | `IP82:31854` | 4 |
| `compose_post` | `IP82:31855` | 5 |
| `user_timeline` | `IP82:31856` | 6 |
| `home_timeline` | `IP82:31857` | 7 |
| `user_mention` | `IP82:31858` | 8 |
| `post_storage` | `IP82:31859` | 9 |
| `user_service` | `IP82:31860` | 10 |

Do not leave these addresses as `127.0.0.1`. They are eRPC control-plane
addresses and must be bindable on the hosting machine and reachable by the
other machine. Keep the MongoDB addresses as `127.0.0.1` when MongoDB and all
database-using services are on machine 82.

The RPC IDs must be globally unique in the range 0-63. With
`server_num=client_num=1`, the service server endpoints use the configured IDs
0-10 and their internal client endpoints use IDs 20-30.

Check the edited configuration:

```bash
grep -n 'server_addr\|rpc_id\|_mongodb\|"addr"\|"port"' \
  apps/social_network_cxl/config/config.ub.json
```

## 5. Start services on machine 82

Start machine 82 first. Run this command from a dedicated terminal or `tmux`
pane; `run_ub.sh` remains in the foreground and writes per-process logs.

```bash
cd /path/to/ub-application/eRPC

export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export ERPC_UB_MACHINE_ID=82
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_REGION_MB=1024
export ERPC_UB_ARENA_MB=16

export SN_BUILD_DIR="$(pwd)/build"
export SN_CONFIG="$(pwd)/apps/social_network_cxl/config/config.ub.json"
export SN_LOG_DIR="$(pwd)/apps/social_network_cxl/logs/ub-82"
export SN_SERVICES="post_storage unique_id url_shorten user_mention user_service user_timeline home_timeline compose_post nginx load_balance"

./apps/social_network_cxl/run_ub.sh
```

The expected default region name is:

```text
erpc_ub_rx_0000000000000052
```

## 6. Start the client on machine 81

After the services on machine 82 have started, run on machine 81:

```bash
cd /path/to/ub-application/eRPC

export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export ERPC_UB_MACHINE_ID=81
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_REGION_MB=1024
export ERPC_UB_ARENA_MB=16

export SN_BUILD_DIR="$(pwd)/build"
export SN_CONFIG="$(pwd)/apps/social_network_cxl/config/config.ub.json"
export SN_LOG_DIR="$(pwd)/apps/social_network_cxl/logs/ub-81"
export SN_SERVICES="client"

./apps/social_network_cxl/run_ub.sh
```

The expected default region name is:

```text
erpc_ub_rx_0000000000000051
```

A hostname-derived hash in the region name means that
`ERPC_UB_MACHINE_ID` did not reach the launched manager or worker process.

If the UBSM provider requires nondefault connection settings, export the same
working values used by `hello_world_ub`, such as `ERPC_UB_PROVIDER_HOST`,
`ERPC_UB_PROVIDER_SOCKET`, or `ERPC_UB_PROVIDER_PORT`, before invoking the
launcher.

## 7. Logs and startup checks

On machine 81:

```bash
tail -f apps/social_network_cxl/logs/ub-81/client.log
tail -f apps/social_network_cxl/logs/ub-81/erpc_ub_manager.log
```

On machine 82:

```bash
tail -f apps/social_network_cxl/logs/ub-82/load_balance.log
tail -f apps/social_network_cxl/logs/ub-82/post_storage.log
tail -f apps/social_network_cxl/logs/ub-82/erpc_ub_manager.log
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
  "server_addr": "IP81:31851",
  "rpc_id": 1,
  "generate_num": 5000,
  "user_num": 1,
  "home_num": 0,
  "write_num": 0,
  "bind_core_offset": 4
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

## 10. Shutdown and cleanup

Stop worker processes before stopping their local `erpc_ub_manager`. A graceful
shutdown gives sessions time to disconnect and remote mappings time to unmap.
The current convenience launcher owns both the manager and workers; pressing
Ctrl-C asks all of them to exit, so always inspect the manager log for a clean
region teardown afterward. Do not manually remove `/dev/obmm_shmdev*` objects.

After all application processes on a machine have exited, stop MongoDB on
machine 82 with the exact database paths used at startup:

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
  --query erpc_ub_rx_0000000000000051
./tests/ubs-mem-two-node/build-ubsm/ubsm_shm_admin \
  --query erpc_ub_rx_0000000000000052
```

Only remove a stale region after confirming that no process still maps it.
