#pragma once
#include "domain_proxy_runtime.h"

// Control-plane readiness only. Legacy proxy data handlers stay unchanged.
namespace sn_measure {
inline std::atomic<bool> &proxy_ready() { static std::atomic<bool> v{false}; return v; }
inline std::atomic<uint64_t> &proxy_forward() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t> &proxy_reverse() { static std::atomic<uint64_t> v{0}; return v; }
template<class Context>
void proxy_ready_request(erpc::ReqHandle *handle, void *context, const char *role) {
  auto *ctx = static_cast<Context *>(context);
  const auto *buffer = handle->get_req_msgbuf();
  invalidate_msgbuf_before_read(ctx->rpc_, *buffer);
  if (buffer->get_data_size() != sizeof(RPCMsgReq<PingRPCReq>))
    sn_domain::fail(role, 0, "ready-size", 0, EPROTO);
  RPCMsgReq<PingRPCReq> request(RPC_TYPE::RPC_DOMAIN_READY, 0, {});
  std::memcpy(&request, buffer->buf_, sizeof(request));
  const int status = request.req_common.type != RPC_TYPE::RPC_DOMAIN_READY ||
      request.req_common.req_number || request.req_control.timestamp != sn_domain::kWireAbi ? EPROTO :
      (proxy_ready().load(std::memory_order_acquire) ? 0 : EAGAIN);
  new (handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(RPC_TYPE::RPC_DOMAIN_READY, 0, status, {sn_domain::kWireAbi});
  ctx->rpc_->resize_msg_buffer(&handle->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));
  ctx->rpc_->enqueue_response(handle, &handle->pre_resp_msgbuf_);
}
inline void proxy_report(const char *role) {
  if (rpc_baseline()) std::fprintf(stderr, "SN_CXL_PROXY_DONE role=%s forward=%llu reverse=%llu\n", role,
      static_cast<unsigned long long>(proxy_forward().load()), static_cast<unsigned long long>(proxy_reverse().load()));
}
}  // namespace sn_measure
