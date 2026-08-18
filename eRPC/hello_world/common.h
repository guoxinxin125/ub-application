#include <stdio.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include "rpc.h"

#ifdef ERPC_CXL
#include "utils/bypass_cache.h"
#endif

static const std::string kServerHostname = "127.0.0.1";
static const std::string kClientHostname = "127.0.0.1";

static constexpr uint16_t kServerUDPPort = 31850;
static constexpr uint16_t kClientUDPPort = 31851;
static constexpr uint8_t kReqType = 2;
static constexpr size_t kDefaultMsgSize = 4096;

inline void hello_world_flush_after_write(const erpc::MsgBuffer &msgbuf) {
#if defined(ERPC_CXL) && defined(USE_NO_CC_QUEUE) && !defined(USE_ONE_SIDE_READ)
  if (msgbuf.buf_ != nullptr && msgbuf.get_data_size() > 0) {
    clwb(msgbuf.buf_, msgbuf.get_data_size());
  }
#else
  (void)msgbuf;
#endif
}

inline void hello_world_flush_before_read(const erpc::MsgBuffer &msgbuf) {
#if defined(ERPC_CXL) && defined(USE_NO_CC_QUEUE) && \
    !defined(HELLO_WORLD_COPY_BEFORE_READ)
  if (msgbuf.buf_ != nullptr && msgbuf.get_data_size() > 0) {
    clflush(msgbuf.buf_, msgbuf.get_data_size());
  }
#else
  (void)msgbuf;
#endif
}

inline int hello_world_copy_then_read_int(const erpc::MsgBuffer &msgbuf) {
  const size_t size = msgbuf.get_data_size();
  if (size >= sizeof(int)) {
    std::vector<uint8_t> local(size);
    if (size % 16 == 0 &&
        reinterpret_cast<uintptr_t>(msgbuf.buf_) % 16 == 0) {
      memcpy_nt_read(reinterpret_cast<char *>(local.data()),
                     reinterpret_cast<const char *>(msgbuf.buf_), size);
    } else {
      std::memcpy(local.data(), msgbuf.buf_, size);
    }
    return *reinterpret_cast<const int *>(local.data());
  }
  return 0;
}

inline int hello_world_read_request_int(const erpc::MsgBuffer &msgbuf) {
#if defined(ERPC_CXL) && defined(USE_NO_CC_QUEUE) && \
    defined(HELLO_WORLD_COPY_BEFORE_READ)
  return hello_world_copy_then_read_int(msgbuf);
#else
  hello_world_flush_before_read(msgbuf);
  return *reinterpret_cast<const int *>(msgbuf.buf_);
#endif
}

inline int hello_world_read_response_int(const erpc::MsgBuffer &msgbuf) {
#if defined(ERPC_CXL) && defined(USE_NO_CC_QUEUE) && \
    defined(HELLO_WORLD_COPY_BEFORE_READ)
  return hello_world_copy_then_read_int(msgbuf);
#else
  hello_world_flush_before_read(msgbuf);
  return *reinterpret_cast<const int *>(msgbuf.buf_);
#endif
}
