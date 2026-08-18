#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "rpc.h"

static const std::string kServerHostname = "192.168.12.108";
static const std::string kClientHostname = "192.168.12.176";

static constexpr uint16_t kServerUDPPort = 31850;
static constexpr uint16_t kClientUDPPort = 31851;
static constexpr uint8_t kReqType = 2;
static constexpr size_t kMsgSize = 64;
static constexpr int kWarmupRequests = 100;
static constexpr int kMeasuredRequests = 100000;

inline void write_req_id(erpc::MsgBuffer &msgbuf, int value) {
  *reinterpret_cast<int *>(msgbuf.buf_) = value;
}

inline int read_req_id(const erpc::MsgBuffer &msgbuf) {
  return *reinterpret_cast<const int *>(msgbuf.buf_);
}
