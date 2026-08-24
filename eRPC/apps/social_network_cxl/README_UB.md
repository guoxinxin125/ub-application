# UB reference-mode support

Build the application with UB and C++17-enabled service targets:

```bash
echo social_network_cxl > scripts/autorun_app_file
cmake . -DTRANSPORT=ub -DPERF=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
make -j
```

The request path carries request data directly in each eRPC `MsgBuffer`. Every
intermediate hop copies an incoming UB payload into a `MsgBuffer` owned by its
outgoing endpoint before forwarding it. The transport does not forward a
borrowed remote `MsgBuffer`.

Post Storage keeps its index in the process-local map and stores each
`PostData` object in its endpoint's shared arena. Read responses carry a
32-byte `{machine_id, block_offset, payload_offset, payload_length}` handle.
Post Storage transfers one reference with each returned handle; the final
client copies the complete `PostData` into local memory and releases that
reference. Intermediate services only copy the handle list.

For multi-process deployment, assign globally unique RPC IDs in the range
0-63. The existing configuration assumes one server and one client RPC thread
per service: service server IDs are the configured `rpc_id`, and client-side
IDs use `rpc_id + 20`. Update every `server_addr` in the JSON configuration to
the reachable control-plane address of the machine hosting that service.

Run only the services assigned to the current machine, for example:

```bash
export ERPC_UB_MACHINE_ID=1
export SN_CONFIG=/path/to/config.ub.json
export SN_SERVICES="client load_balance nginx"
./apps/social_network_cxl/run_ub.sh
```

Run the remaining services on the other machine with a different nonzero
`ERPC_UB_MACHINE_ID`. `run_ub.sh` starts one local UB manager and defaults to a
1 GiB machine region because the full application creates many endpoints.
Use the same memory mode on every machine.
