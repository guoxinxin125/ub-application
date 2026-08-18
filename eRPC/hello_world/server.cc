#include "common.h"
#include <signal.h>
#include <chrono>
#include <cstdint>
#include <cstring>

erpc::Rpc<erpc::CTransport> *rpc;
volatile sig_atomic_t ctrl_c_pressed = 0;
int request_count = 0;
uint64_t request_checksum = 0;

void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

void req_handler(erpc::ReqHandle *req_handle, void *) {
  request_count++;

  const erpc::MsgBuffer *req = req_handle->get_req_msgbuf();
  int req_id = 0;
  if (req != nullptr && req->buf_ != nullptr &&
      req->get_data_size() >= sizeof(req_id)) {
    req_id = hello_world_read_request_int(*req);
    request_checksum += static_cast<uint64_t>(req_id);
  }

  const size_t resp_size =
      req != nullptr ? req->get_data_size() : kDefaultMsgSize;
  auto &resp = req_handle->dyn_resp_msgbuf_;
  resp = rpc->alloc_msg_buffer_or_die(resp_size);
  *reinterpret_cast<int *>(resp.buf_) = req_id;
  hello_world_flush_after_write(resp);
  rpc->enqueue_response(req_handle, &resp);
}

int main() {
  signal(SIGINT, ctrl_c_handler);

  std::string server_uri = kServerHostname + ":" + std::to_string(kServerUDPPort);
  printf("Server: Starting on %s\n", server_uri.c_str());

  erpc::Nexus nexus(server_uri);

  nexus.register_req_func(kReqType, req_handler);

  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, nullptr, 0);

  while (ctrl_c_pressed != 1) {
    rpc->run_event_loop(100);
  }

  printf("Server: Shutting down after processing %d requests, checksum %lu\n",
         request_count, request_checksum);
  delete rpc;
  return 0;
}
