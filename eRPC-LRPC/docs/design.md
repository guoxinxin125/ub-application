# Design and patent-to-prototype mapping

| Patent element | Prototype implementation |
|---|---|
| Procedure descriptor and code epoch | Shared metadata page, published and validated through ioctls |
| Binding object | Per-open kernel file context bound to one service channel after `UB_LRPC_IOC_BIND` |
| Remote RX code | ivshmem BAR2 code window; publisher is RW/NX, shadow is RO/X, caller cannot map it |
| Shared A-stack | One isolated 64 KiB BAR2 slot per service; 60 KiB serialized payload |
| Local E-stack | anonymous 256 KiB mapping private to C/C++ callback shadows, used by `switch_stack.S`; Go callbacks retain their cgo system stack |
| Shadow registration | `UB_LRPC_IOC_REGISTER_SHADOW` records the passive task, `mm_struct`, PID, and pinned CPU |
| Address-space switch | caller sleeps in `CALL`; scheduler runs the distinct shadow task/mm on the same CPU |
| Return-domain restoration | `RETURN` makes the shadow passive and completes the sleeping caller |
| eRPC transparency | Upstream eRPC `Rpc::enqueue_request`; its LRPC transport invokes the code image and injects a normal response completion |
| Nested calls | A callback shadow may bind another channel and synchronously invoke it; each channel preserves its own caller and frame |
| gRPC transparency | Unary gRPC-Go `ClientConnInterface` plus `ServeUnary` shadow dispatcher marshal protobuf messages through `lrpc_invoke_bytes()` |

The service image is intentionally tiny and relocation-free.  Its only input
is the A-stack pointer; it cannot address caller globals. The demonstration
dereferences `service_data`, which is valid only in the shadow mm, to compute
`100 + 20 + 22 = 142`. Linux rejects any caller mapping of code/service data,
rejects writable executable code, and rejects executable A-stack/data mappings.

## What is not claimed

The current code relies on Linux's scheduler to switch between two tasks and
does not add a ChCore-style direct `sched_to_thread()` primitive. It does not
yet provide queued concurrent calls to the same service, cancellation-safe
interrupted calls, gRPC streaming, an
unforgeable cross-process capability fd, or UB fault emulation. The
upstream integration covers the synchronous, single-packet request shape used
by the validation program and retains eRPC's UDP session-management plane.
Congestion control, fragmentation, and background handlers are not silently
claimed as implemented. The QEMU server handler emits
`ERROR_REMOTE_CPU_HANDLER_RAN` if the normal remote-CPU datapath is accidentally
used; the end-to-end test rejects that marker.
