#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <urma_api.h>

namespace urma_test {

constexpr uint32_t kQueueDepth = 64;
constexpr uint32_t kJfcDepth = kQueueDepth * 2;
constexpr uint64_t kDefaultToken = 0xacfe;

[[noreturn]] inline void fail(const std::string &message) {
  throw std::runtime_error(message);
}

inline void check_status(urma_status_t status, const std::string &operation) {
  if (status != URMA_SUCCESS) {
    fail(operation +
         " failed, URMA status=" + std::to_string(static_cast<int>(status)));
  }
}

inline uint64_t parse_u64(const std::string &text, const std::string &option) {
  size_t consumed = 0;
  unsigned long long value = 0;
  try {
    value = std::stoull(text, &consumed, 0);
  } catch (const std::exception &) {
    fail("invalid value for " + option + ": " + text);
  }
  if (consumed != text.size())
    fail("invalid value for " + option + ": " + text);
  return static_cast<uint64_t>(value);
}

inline uint32_t parse_u32(const std::string &text, const std::string &option) {
  const uint64_t value = parse_u64(text, option);
  if (value > std::numeric_limits<uint32_t>::max())
    fail(option + " must fit in uint32_t");
  return static_cast<uint32_t>(value);
}

inline uint16_t parse_port(const std::string &text) {
  const uint64_t value = parse_u64(text, "--port");
  if (value == 0 || value > 65535)
    fail("--port must be in [1, 65535]");
  return static_cast<uint16_t>(value);
}

inline std::vector<size_t> parse_sizes(const std::string &text) {
  std::vector<size_t> result;
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t comma = text.find(',', begin);
    const std::string token = text.substr(begin, comma - begin);
    const uint64_t value = parse_u64(token, "--sizes");
    if (value == 0 || value > std::numeric_limits<uint32_t>::max())
      fail("each --sizes entry must be in [1, UINT32_MAX]");
    result.push_back(static_cast<size_t>(value));
    if (comma == std::string::npos)
      break;
    begin = comma + 1;
  }
  if (result.empty())
    fail("--sizes cannot be empty");
  return result;
}

inline void send_all(int fd, const void *data, size_t size) {
  const auto *cursor = static_cast<const uint8_t *>(data);
  while (size != 0) {
    const ssize_t sent = send(fd, cursor, size, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      fail("TCP send failed: " + std::string(std::strerror(errno)));
    cursor += sent;
    size -= static_cast<size_t>(sent);
  }
}

inline void recv_all(int fd, void *data, size_t size) {
  auto *cursor = static_cast<uint8_t *>(data);
  while (size != 0) {
    const ssize_t received = recv(fd, cursor, size, 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0)
      fail(received == 0
               ? "TCP peer closed the connection"
               : "TCP recv failed: " + std::string(std::strerror(errno)));
    cursor += received;
    size -= static_cast<size_t>(received);
  }
}

inline void send_stage(int fd, uint8_t stage) { send_all(fd, &stage, 1); }

inline void expect_stage(int fd, uint8_t expected) {
  uint8_t actual = 0;
  recv_all(fd, &actual, 1);
  if (actual != expected) {
    fail("unexpected stage byte: expected=" + std::to_string(expected) +
         ", actual=" + std::to_string(actual));
  }
}

inline int listen_and_accept(const std::string &bind_ip, uint16_t port) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0)
    fail("socket failed: " + std::string(std::strerror(errno)));
  int one = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_ip.c_str(), &address.sin_addr) != 1) {
    close(listener);
    fail("invalid --bind-ip: " + bind_ip);
  }
  if (bind(listener, reinterpret_cast<const sockaddr *>(&address),
           sizeof(address)) != 0 ||
      listen(listener, 1) != 0) {
    const std::string message = std::strerror(errno);
    close(listener);
    fail("TCP listen failed: " + message);
  }
  std::cout << "waiting_for_peer=" << bind_ip << ':' << port << std::endl;
  int fd;
  do {
    fd = accept(listener, nullptr, nullptr);
  } while (fd < 0 && errno == EINTR);
  const int saved_errno = errno;
  close(listener);
  if (fd < 0)
    fail("accept failed: " + std::string(std::strerror(saved_errno)));
  return fd;
}

inline int connect_with_retry(const std::string &server_ip, uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, server_ip.c_str(), &address.sin_addr) != 1)
    fail("invalid --server-ip: " + server_ip);
  int last_error = ECONNREFUSED;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      fail("socket failed: " + std::string(std::strerror(errno)));
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == 0)
      return fd;
    last_error = errno;
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  fail("connect failed: " + std::string(std::strerror(last_error)));
}

inline uint8_t pattern_byte(size_t index, uint8_t seed) {
  return static_cast<uint8_t>(seed ^ static_cast<uint8_t>(index * 131U) ^
                              static_cast<uint8_t>(index >> 7U));
}

inline void fill_pattern(void *address, size_t length, uint8_t seed) {
  auto *bytes = static_cast<uint8_t *>(address);
  for (size_t i = 0; i < length; ++i)
    bytes[i] = pattern_byte(i, seed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void verify_pattern(const void *address, size_t length, uint8_t seed,
                           const std::string &description) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const auto *bytes = static_cast<const uint8_t *>(address);
  for (size_t i = 0; i < length; ++i) {
    const uint8_t expected = pattern_byte(i, seed);
    if (bytes[i] != expected) {
      fail(description + " mismatch at byte " + std::to_string(i) +
           ": expected=" + std::to_string(expected) +
           ", actual=" + std::to_string(bytes[i]));
    }
  }
}

struct LatencyStats {
  double average_ns = 0.0;
  uint64_t p50_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t maximum_ns = 0;
};

inline LatencyStats summarize(std::vector<uint64_t> samples) {
  if (samples.empty())
    fail("cannot summarize empty latency samples");
  const uint64_t total =
      std::accumulate(samples.begin(), samples.end(), uint64_t{0});
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&](double fraction) {
    const size_t rank = static_cast<size_t>(
        std::ceil(fraction * static_cast<double>(samples.size())));
    return samples[std::max<size_t>(1, rank) - 1];
  };
  LatencyStats result;
  result.average_ns =
      static_cast<double>(total) / static_cast<double>(samples.size());
  result.p50_ns = percentile(0.50);
  result.p99_ns = percentile(0.99);
  result.maximum_ns = samples.back();
  return result;
}

inline void print_stats(const std::string &operation, size_t bytes,
                        const std::vector<uint64_t> &samples) {
  const LatencyStats stats = summarize(samples);
  std::cout << std::fixed << std::setprecision(1) << "operation=" << operation
            << " bytes=" << bytes << " iterations=" << samples.size()
            << " avg_ns=" << stats.average_ns << " p50_ns=" << stats.p50_ns
            << " p99_ns=" << stats.p99_ns << " max_ns=" << stats.maximum_ns
            << '\n';
}

struct PeerInfo {
  urma_seg_t segment;
  urma_jetty_id_t jetty_id;
  urma_token_t token;
};

class Session {
public:
  Session(const std::string &device_name, uint32_t requested_eid_index,
          urma_tp_type_t tp_type)
      : tp_type_(tp_type) {
    try {
      initialize(device_name, requested_eid_index);
    } catch (...) {
      cleanup();
      throw;
    }
  }

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  ~Session() { cleanup(); }

  void register_buffer(void *address, size_t length) {
    if (local_segment_ != nullptr)
      fail("URMA local segment is already registered");
    urma_seg_cfg_t cfg{};
    cfg.va = reinterpret_cast<uint64_t>(address);
    cfg.len = length;
    cfg.token_value = token_;
    cfg.flag.bs.token_policy = URMA_TOKEN_NONE;
    cfg.flag.bs.cacheable = URMA_NON_CACHEABLE;
    cfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    local_segment_ = urma_register_seg(context_, &cfg);
    if (local_segment_ == nullptr)
      fail("urma_register_seg failed for address=" +
           std::to_string(reinterpret_cast<uintptr_t>(address)) +
           ", length=" + std::to_string(length));
  }

  void connect_peer(int fd, bool server) {
    if (local_segment_ == nullptr)
      fail("register_buffer must run before connect_peer");
    PeerInfo local{};
    local.segment = local_segment_->seg;
    local.jetty_id = jetty_->jetty_id;
    local.token = token_;
    PeerInfo peer{};
    if (server) {
      recv_all(fd, &peer, sizeof(peer));
      send_all(fd, &local, sizeof(local));
    } else {
      send_all(fd, &local, sizeof(local));
      recv_all(fd, &peer, sizeof(peer));
    }
    remote_segment_descriptor_ = peer.segment;

    urma_import_seg_flag_t segment_flag{};
    segment_flag.bs.cacheable = URMA_NON_CACHEABLE;
    segment_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    segment_flag.bs.mapping = URMA_SEG_NOMAP;
    remote_segment_ = urma_import_seg(context_, &remote_segment_descriptor_,
                                      &peer.token, 0, segment_flag);
    if (remote_segment_ == nullptr)
      fail("urma_import_seg failed");

    urma_rjetty_t remote_jetty{};
    remote_jetty.jetty_id = peer.jetty_id;
    remote_jetty.trans_mode = URMA_TM_RC;
    remote_jetty.type = URMA_JETTY;
    remote_jetty.tp_type = tp_type_;
    target_jetty_ = urma_import_jetty(context_, &remote_jetty, &peer.token);
    if (target_jetty_ == nullptr)
      fail("urma_import_jetty failed");
    check_status(urma_bind_jetty(jetty_, target_jetty_), "urma_bind_jetty");
    bound_ = true;
  }

  uint64_t post_read(void *local_address, uint64_t remote_address,
                     size_t length) {
    return post_rw(URMA_OPC_READ, local_address, remote_address, length);
  }

  uint64_t post_write(void *local_address, uint64_t remote_address,
                      size_t length) {
    return post_rw(URMA_OPC_WRITE, local_address, remote_address, length);
  }

  uint64_t remote_base() const { return remote_segment_descriptor_.ubva.va; }

private:
  void initialize(const std::string &device_name,
                  uint32_t requested_eid_index) {
    urma_init_attr_t init_attr{};
    check_status(urma_init(&init_attr), "urma_init");
    initialized_ = true;

    urma_device_t *device = urma_get_device_by_name(device_name.c_str());
    if (device == nullptr)
      fail("urma_get_device_by_name failed for " + device_name);
    urma_device_attr_t attributes{};
    check_status(urma_query_device(device, &attributes), "urma_query_device");

    uint32_t eid_count = 0;
    urma_eid_info_t *eid_list = urma_get_eid_list(device, &eid_count);
    if (eid_list == nullptr)
      fail("urma_get_eid_list returned no EID");
    if (eid_count == 0) {
      urma_free_eid_list(eid_list);
      fail("urma_get_eid_list returned no EID");
    }
    uint32_t selected = eid_list[0].eid_index;
    if (requested_eid_index != std::numeric_limits<uint32_t>::max()) {
      bool found = false;
      for (uint32_t i = 0; i < eid_count; ++i) {
        if (eid_list[i].eid_index == requested_eid_index) {
          selected = requested_eid_index;
          found = true;
          break;
        }
      }
      if (!found) {
        urma_free_eid_list(eid_list);
        fail("requested EID index is unavailable");
      }
    }
    urma_free_eid_list(eid_list);

    context_ = urma_create_context(device, selected);
    if (context_ == nullptr)
      fail("urma_create_context failed");
    token_.token = kDefaultToken;

    jfce_ = urma_create_jfce(context_);
    if (jfce_ == nullptr)
      fail("urma_create_jfce failed");
    urma_jfc_cfg_t jfc_cfg{};
    jfc_cfg.depth = kJfcDepth;
    jfc_cfg.jfce = jfce_;
    jfc_ = urma_create_jfc(context_, &jfc_cfg);
    if (jfc_ == nullptr)
      fail("urma_create_jfc failed");

    urma_jfr_cfg_t jfr_cfg{};
    jfr_cfg.depth = kQueueDepth;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.trans_mode = URMA_TM_RC;
    jfr_cfg.max_sge = 1;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = jfc_;
    jfr_cfg.token_value = token_;
    jfr_ = urma_create_jfr(context_, &jfr_cfg);
    if (jfr_ == nullptr)
      fail("urma_create_jfr failed");

    urma_jfs_cfg_t jfs_cfg{};
    jfs_cfg.depth = kQueueDepth;
    jfs_cfg.trans_mode = URMA_TM_RC;
    jfs_cfg.priority = URMA_MAX_PRIORITY;
    jfs_cfg.max_sge = 1;
    jfs_cfg.max_rsge = 1;
    jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jfs_cfg.jfc = jfc_;
    urma_jetty_cfg_t jetty_cfg{};
    jetty_cfg.flag.bs.share_jfr = URMA_SHARE_JFR;
    jetty_cfg.jfs_cfg = jfs_cfg;
    jetty_cfg.shared.jfr = jfr_;
    jetty_ = urma_create_jetty(context_, &jetty_cfg);
    if (jetty_ == nullptr)
      fail("urma_create_jetty failed");
  }
  uint64_t post_rw(urma_opcode_t opcode, void *local_address,
                   uint64_t remote_address, size_t length) {
    if (length > std::numeric_limits<uint32_t>::max())
      fail("URMA operation length exceeds UINT32_MAX");
    urma_sge_t local_sge{};
    local_sge.addr = reinterpret_cast<uint64_t>(local_address);
    local_sge.len = static_cast<uint32_t>(length);
    local_sge.tseg = local_segment_;
    urma_sge_t remote_sge{};
    remote_sge.addr = remote_address;
    remote_sge.len = static_cast<uint32_t>(length);
    remote_sge.tseg = remote_segment_;
    urma_sg_t local_sg{};
    local_sg.sge = &local_sge;
    local_sg.num_sge = 1;
    urma_sg_t remote_sg{};
    remote_sg.sge = &remote_sge;
    remote_sg.num_sge = 1;

    urma_jfs_wr_t wr{};
    wr.opcode = opcode;
    wr.flag.bs.complete_enable = URMA_COMPLETE_ENABLE;
    wr.tjetty = target_jetty_;
    wr.user_ctx = ++next_wr_id_;
    if (opcode == URMA_OPC_READ) {
      wr.rw.src = remote_sg;
      wr.rw.dst = local_sg;
    } else {
      wr.rw.src = local_sg;
      wr.rw.dst = remote_sg;
    }
    urma_jfs_wr_t *bad_wr = nullptr;
    const auto start = std::chrono::steady_clock::now();
    check_status(urma_post_jetty_send_wr(jetty_, &wr, &bad_wr),
                 opcode == URMA_OPC_READ ? "post URMA READ"
                                         : "post URMA WRITE");
    poll_completion(wr.user_ctx);
    const auto end = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
  }

  void poll_completion(uint64_t expected_id) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      urma_cr_t completion{};
      const int count = urma_poll_jfc(jfc_, 1, &completion);
      if (count < 0)
        fail("urma_poll_jfc failed, result=" + std::to_string(count));
      if (count == 0)
        continue;
      if (completion.status != URMA_CR_SUCCESS)
        fail("URMA completion failed, status=" +
             std::to_string(static_cast<int>(completion.status)));
      if (completion.flag.bs.s_r != 0)
        continue;
      if (completion.user_ctx != expected_id)
        fail("unexpected URMA completion user_ctx");
      return;
    }
    fail("timed out polling URMA completion");
  }

  void cleanup() noexcept {
    if (bound_ && urma_unbind_jetty(jetty_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_unbind_jetty failed\n";
    bound_ = false;
    if (target_jetty_ != nullptr &&
        urma_unimport_jetty(target_jetty_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_unimport_jetty failed\n";
    target_jetty_ = nullptr;
    if (remote_segment_ != nullptr &&
        urma_unimport_seg(remote_segment_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_unimport_seg failed\n";
    remote_segment_ = nullptr;
    if (local_segment_ != nullptr &&
        urma_unregister_seg(local_segment_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_unregister_seg failed\n";
    local_segment_ = nullptr;
    if (jetty_ != nullptr && urma_delete_jetty(jetty_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_delete_jetty failed\n";
    jetty_ = nullptr;
    if (jfr_ != nullptr && urma_delete_jfr(jfr_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_delete_jfr failed\n";
    jfr_ = nullptr;
    if (jfc_ != nullptr && urma_delete_jfc(jfc_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_delete_jfc failed\n";
    jfc_ = nullptr;
    if (jfce_ != nullptr && urma_delete_jfce(jfce_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_delete_jfce failed\n";
    jfce_ = nullptr;
    if (context_ != nullptr && urma_delete_context(context_) != URMA_SUCCESS)
      std::cerr << "WARN: urma_delete_context failed\n";
    context_ = nullptr;
    if (initialized_ && urma_uninit() != URMA_SUCCESS)
      std::cerr << "WARN: urma_uninit failed\n";
    initialized_ = false;
  }

  urma_tp_type_t tp_type_ = URMA_RTP;
  bool initialized_ = false;
  bool bound_ = false;
  urma_context_t *context_ = nullptr;
  urma_jfce_t *jfce_ = nullptr;
  urma_jfc_t *jfc_ = nullptr;
  urma_jfr_t *jfr_ = nullptr;
  urma_jetty_t *jetty_ = nullptr;
  urma_target_seg_t *local_segment_ = nullptr;
  urma_target_seg_t *remote_segment_ = nullptr;
  urma_target_jetty_t *target_jetty_ = nullptr;
  urma_seg_t remote_segment_descriptor_{};
  urma_token_t token_{};
  uint64_t next_wr_id_ = 0;
};

class AlignedBuffer {
public:
  explicit AlignedBuffer(size_t size) : size_(size) {
    if (posix_memalign(&address_, 4096, size_) != 0 || address_ == nullptr)
      fail("posix_memalign failed");
    std::memset(address_, 0, size_);
  }
  ~AlignedBuffer() { std::free(address_); }
  AlignedBuffer(const AlignedBuffer &) = delete;
  AlignedBuffer &operator=(const AlignedBuffer &) = delete;
  void *data() const { return address_; }
  size_t size() const { return size_; }

private:
  void *address_ = nullptr;
  size_t size_ = 0;
};

inline urma_tp_type_t parse_tp_type(uint32_t value) {
  if (value > 2)
    fail("--tp-type must be 0 (RTP), 1 (CTP), or 2 (UTP)");
  return static_cast<urma_tp_type_t>(value);
}

} // namespace urma_test
