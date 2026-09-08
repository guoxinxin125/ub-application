#ifndef ERPC_LRPC_ERPC_COMPAT_H
#define ERPC_LRPC_ERPC_COMPAT_H

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <lrpc/lrpc.h>

/* Minimal eRPC application-facing request shape used by the validation app. */
namespace erpc {
struct MsgBuffer {
	uint8_t *buf_;
	size_t data_size_;
};
using cont_func_t = void (*)(void *, void *);

class Rpc {
 public:
	explicit Rpc(const char *dev, uint32_t procedure_id = 1,
		     uint64_t epoch = 1)
		: procedure_id_(procedure_id) {
		ok_ = lrpc_bind(&h_, dev, procedure_id, epoch) == 0;
	}
	~Rpc() { lrpc_close(&h_); }
	bool ok() const { return ok_; }
	void enqueue_request(int, uint8_t req_type, MsgBuffer *req, MsgBuffer *resp,
			     cont_func_t cont, void *tag) {
		lrpc_astack call;
		memset(&call, 0, offsetof(lrpc_astack, payload));
		call.abi = LRPC_ASTACK_ABI;
		call.procedure_id = req_type;
		int rc = 0;
		if (!ok_ || req_type != procedure_id_ || !req || !resp ||
		    req->data_size_ > LRPC_PAYLOAD_MAX) {
			rc = -EINVAL;
		} else {
			call.request_size = req->data_size_;
			memcpy(call.payload, req->buf_, req->data_size_);
			rc = lrpc_invoke(&h_, &call);
		}
		if (rc == 0 && call.status == 0) {
			memcpy(resp->buf_, call.payload, call.response_size);
			resp->data_size_ = call.response_size;
		}
		last_cpu_before_ = call.caller_cpu_before;
		last_cpu_service_ = call.caller_cpu_in_service;
		last_cpu_after_ = call.caller_cpu_after;
		last_shadow_pid_ = call.shadow_pid;
		last_caller_pid_ = call.caller_pid;
		call_enter_ns_ = call.call_enter_ns;
		shadow_dispatch_ns_ = call.shadow_dispatch_ns;
		shadow_return_ns_ = call.shadow_return_ns;
		caller_resume_ns_ = call.caller_resume_ns;
		last_rc_ = rc ? rc : static_cast<int>(call.status);
		if (cont) cont(nullptr, tag);
	}
	uint64_t last_cpu_before() const { return last_cpu_before_; }
	uint64_t last_cpu_service() const { return last_cpu_service_; }
	uint64_t last_cpu_after() const { return last_cpu_after_; }
	uint64_t last_shadow_pid() const { return last_shadow_pid_; }
	uint64_t last_caller_pid() const { return last_caller_pid_; }
	uint64_t call_enter_ns() const { return call_enter_ns_; }
	uint64_t shadow_dispatch_ns() const { return shadow_dispatch_ns_; }
	uint64_t shadow_return_ns() const { return shadow_return_ns_; }
	uint64_t caller_resume_ns() const { return caller_resume_ns_; }
	int last_rc() const { return last_rc_; }
	int native_handle_for_test() const { return h_.fd; }
 private:
	lrpc_handle h_{};
	uint32_t procedure_id_ = 0;
	bool ok_ = false;
	int last_rc_ = -1;
	uint64_t last_cpu_before_ = ~0ull, last_cpu_service_ = ~0ull;
	uint64_t last_cpu_after_ = ~0ull;
	uint64_t last_shadow_pid_ = 0, last_caller_pid_ = 0;
	uint64_t call_enter_ns_ = 0, shadow_dispatch_ns_ = 0;
	uint64_t shadow_return_ns_ = 0, caller_resume_ns_ = 0;
};
}  // namespace erpc
#endif
