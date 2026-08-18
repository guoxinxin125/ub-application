#pragma once

#include <nlohmann/json.hpp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <cstring>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <utility>
#include <cstdint>

#ifdef CACHE_LINE_SIZE
#undef CACHE_LINE_SIZE
#endif

#include "atomic_queue/atomic_queue.h"
#include "social_network_rpc_type.h"
#include "util/numautils.h"
#include "utils/bypass_cache.h"

#include "pkthdr.h"

#include "rpc.h"

#ifdef ERPC_CXL
#include "transport_impl/cxl/cxl_transport.h"
#include "transport_impl/cxl/cxl_shared_allocator.h"
#include "social_network_cxl.h"
#endif

#include <gflags/gflags.h>

DECLARE_string(config_file);
DECLARE_uint64(test_loop);
DECLARE_uint64(concurrency);
DECLARE_string(numa_0_ports);
DECLARE_string(numa_1_ports);
DECLARE_string(server_addr);
DECLARE_uint64(client_num);
DECLARE_uint64(server_num);
DECLARE_uint64(numa_client_node);
DECLARE_uint64(numa_server_node);
DECLARE_uint64(bind_core_offset);
DECLARE_uint64(timeout_second);
DECLARE_string(latency_file);
DECLARE_string(bandwidth_file);
DECLARE_uint64(rpc_id);

using json = nlohmann::json;

#define my_assert(expr, ...) assert(expr)

using SPSC_QUEUE = atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, false>;
using MPMC_QUEUE = atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, false>;

static constexpr size_t kAppMaxConcurrency = 128;
static constexpr size_t kAppMaxRPC = 12;

static constexpr size_t kAppMaxBuffer = kAppMaxConcurrency * kAppMaxRPC * 10;

static constexpr size_t kAppEvLoopMs = 1000;

volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

json config_json_all;

inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline int load_config_file(const std::string& file_name, json* config_json) {
    std::ifstream json_file;
    json_file.open(file_name);
    if (json_file.is_open()) {
        json_file >> *config_json;
        json_file.close();
        return 0;
    }
    fprintf(stderr, "Failed to open config file: %s\n", file_name.c_str());
    return -1;
}

inline uint8_t get_remote_rpc_id(const std::string& target_addr) {
    for (auto& el : config_json_all.items()) {
        if (el.value().is_object() && el.value().contains("server_addr") && el.value().contains("rpc_id")) {
            if (el.value()["server_addr"].get<std::string>() == target_addr) {
                return static_cast<uint8_t>(el.value()["rpc_id"].get<uint64_t>());
            }
        }
    }
    return 0; // fallback
}

inline void init_service_config(const std::string& file_path, const std::string& service_name) {
    if (load_config_file(file_path, &config_json_all) != 0) {
        exit(1);
    }


    auto config = config_json_all["common"];
    if (config.is_null()) {
        fprintf(stderr, "Failed to find common config\n");
        exit(1);
    }

    FLAGS_test_loop = config.value("test_loop", 10);
    FLAGS_concurrency = config.value("concurrency", 0);
    FLAGS_numa_0_ports = config.value("numa_0_ports", "");
    FLAGS_numa_1_ports = config.value("numa_1_ports", "");
    FLAGS_client_num = config.value("client_num", 1);
    FLAGS_server_num = config.value("server_num", 1);
    FLAGS_numa_client_node = config.value("numa_client_node", 0);
    FLAGS_numa_server_node = config.value("numa_server_node", 0);
    FLAGS_bind_core_offset = config.value("bind_core_offset", 0);
    FLAGS_timeout_second = config.value("timeout_second", UINT64_MAX);
    FLAGS_latency_file = config.value("latency_file", "latency.txt");
    FLAGS_bandwidth_file = config.value("bandwidth_file", "bandwidth.txt");

    auto server_config = config_json_all[service_name];
    if (server_config.is_null()) {
        fprintf(stderr, "Failed to find %s config\n", service_name.c_str());
        exit(1);
    }

    if (server_config.contains("server_addr")) {
        FLAGS_server_addr = server_config["server_addr"].get<std::string>();
    }

    if (server_config.contains("rpc_id")) {
        FLAGS_rpc_id = server_config["rpc_id"].get<uint64_t>();
    }

    if (server_config.contains("bind_core_offset")) {
        FLAGS_bind_core_offset = server_config["bind_core_offset"].get<uint64_t>();
    }
}

inline std::vector<size_t> flags_get_cxl_ports(size_t numa_node) {
    std::vector<size_t> ret;
    std::string port_str = numa_node == 0 ? FLAGS_numa_0_ports : FLAGS_numa_1_ports;
    if (port_str.empty()) {
        ret.push_back(0);
        return ret;
    }
    std::vector<std::string> split_vec = split(port_str, ',');
    for (auto &s : split_vec)
        ret.push_back(std::stoull(s));
    return ret;
} 

#ifdef ERPC_CXL
inline erpc::MsgBuffer clone_msgbuf(erpc::Rpc<erpc::CXLTransport> *rpc,
                                   const erpc::MsgBuffer &src) {
    size_t size = src.get_data_size();
    erpc::MsgBuffer dst = rpc->alloc_msg_buffer_or_die(size);
    if (size != 0) {
        std::memcpy(dst.buf_, src.buf_, size);
    }
    return dst;
}

inline void *msgbuf_to_cxl_ptr(const erpc::MsgBuffer &msgbuf) {
    return static_cast<void *>(msgbuf.buf_ - sizeof(erpc::pkthdr_t));
}
 
inline erpc::MsgBuffer pin_msgbuf(erpc::Rpc<erpc::CXLTransport> *rpc,
                                  const erpc::MsgBuffer &src) {
    void *base = msgbuf_to_cxl_ptr(src);
    if (social_network_cxl::is_shared_ptr(rpc, base)) {
        social_network_cxl::add_ref_cxl_buffer(rpc, base);
        erpc::MsgBuffer pinned = src;
        pinned.own_pkthdr_copy();
        return pinned;
    }
    return clone_msgbuf(rpc, src);
}

inline void release_msgbuf(erpc::Rpc<erpc::CXLTransport> *rpc,
                           const erpc::MsgBuffer &msgbuf) {
    if (msgbuf.buf_ == nullptr) {
        return;
    }
    void *base = msgbuf_to_cxl_ptr(msgbuf);
    if (social_network_cxl::is_shared_ptr(rpc, base)) {
        size_t total_size = msgbuf.get_data_size() + sizeof(erpc::pkthdr_t);
        social_network_cxl::free_cxl_buffer(rpc, base, total_size);
        return;
    }
    rpc->free_msg_buffer(msgbuf);
}

inline uint8_t msgbuf_owner_rpc_id(erpc::Rpc<erpc::CXLTransport> *rpc,
                                   const erpc::MsgBuffer &msgbuf) {
    void *base = msgbuf_to_cxl_ptr(msgbuf);
    if (!social_network_cxl::is_shared_ptr(rpc, base)) {
        return UINT8_MAX;
    }
    return social_network_cxl::get_cxl_allocator(rpc)->get_owner_rpc_id(base);
}

inline bool msgbuf_needs_no_cc_flush(erpc::Rpc<erpc::CXLTransport> *rpc,
                                     const erpc::MsgBuffer &msgbuf) {
#ifdef USE_NO_CC_QUEUE
    if (msgbuf.buf_ == nullptr || msgbuf.get_data_size() == 0) {
        return false;
    }
#ifdef USE_ONE_SIDE_READ
    uint8_t owner_rpc_id = msgbuf_owner_rpc_id(rpc, msgbuf);
    return owner_rpc_id == UINT8_MAX || owner_rpc_id != rpc->get_rpc_id();
#else
    _unused(rpc);
    return true;
#endif
#else
    _unused(rpc);
    _unused(msgbuf);
    return false;
#endif
}

inline void flush_msgbuf_before_send(erpc::Rpc<erpc::CXLTransport> *rpc,
                                     const erpc::MsgBuffer &msgbuf) {
#ifdef USE_NO_CC_QUEUE
    if (msgbuf_needs_no_cc_flush(rpc, msgbuf)) {
        clwb(msgbuf.buf_, msgbuf.get_data_size());
    }
#else
    _unused(rpc);
    _unused(msgbuf);
#endif
}

inline void invalidate_msgbuf_before_read(erpc::Rpc<erpc::CXLTransport> *rpc,
                                          const erpc::MsgBuffer &msgbuf) {
#ifdef USE_NO_CC_QUEUE
    if (msgbuf_needs_no_cc_flush(rpc, msgbuf)) {
        clflush(msgbuf.buf_, msgbuf.get_data_size());
    }
#else
    _unused(rpc);
    _unused(msgbuf);
#endif
}

inline bool cxl_buffer_needs_no_cc_flush(erpc::Rpc<erpc::CXLTransport> *rpc,
                                         void *ptr, size_t size) {
#ifdef USE_NO_CC_QUEUE
    if (ptr == nullptr || size == 0) {
        return false;
    }
#ifdef USE_ONE_SIDE_READ
    uint8_t owner_rpc_id =
        social_network_cxl::get_cxl_allocator(rpc)->get_owner_rpc_id(ptr);
    return owner_rpc_id == UINT8_MAX || owner_rpc_id != rpc->get_rpc_id();
#else
    _unused(rpc);
    return true;
#endif
#else
    _unused(rpc);
    _unused(ptr);
    _unused(size);
    return false;
#endif
}

inline void flush_cxl_buffer_before_send(erpc::Rpc<erpc::CXLTransport> *rpc,
                                         void *ptr, size_t size) {
#ifdef USE_NO_CC_QUEUE
    if (cxl_buffer_needs_no_cc_flush(rpc, ptr, size)) {
        clwb(ptr, size);
    }
#else
    _unused(rpc);
    _unused(ptr);
    _unused(size);
#endif
}

inline void invalidate_cxl_buffer_before_read(erpc::Rpc<erpc::CXLTransport> *rpc,
                                              void *ptr, size_t size) {
#ifdef USE_NO_CC_QUEUE
    if (cxl_buffer_needs_no_cc_flush(rpc, ptr, size)) {
        clflush(ptr, size);
    }
#else
    _unused(rpc);
    _unused(ptr);
    _unused(size);
#endif
}

inline uint64_t get_timestamp_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

class BasicContext {
public:
    erpc::Rpc<erpc::CXLTransport> *rpc_ = nullptr;
    std::vector<int> session_num_vec_;
    size_t num_sm_resps_ = 0;
};

namespace erpc {
extern ::MemoryManager* erpc_global_cacheable_ptrs[256];
}

inline void link_worker_cacheable(uint8_t server_rpc_id) {
#ifdef ERPC_CXL
    while (erpc::erpc_global_cacheable_ptrs[server_rpc_id] == nullptr) {
        std::this_thread::yield();
    }
    auto* parent = erpc::erpc_global_cacheable_ptrs[server_rpc_id];
    cacheable.memkind_pool = parent->memkind_pool;
    cacheable.base = parent->base;
    cacheable.size = parent->size;
    cacheable.allocator = parent->allocator;
#endif
}

inline void unlink_worker_cacheable() {
#ifdef ERPC_CXL
    cacheable.memkind_pool = nullptr;
    cacheable.base = nullptr;
    cacheable.size = 0;
    cacheable.allocator = nullptr;
#endif
}

inline void basic_sm_handler_client(int session_num, erpc::SmEventType sm_event_type,
                             erpc::SmErrType sm_err_type, void *_context) {
    // // printf("client sm_handler receive: session_num:%d\n", session_num);
    auto *c = static_cast<BasicContext *>(_context);
    c->num_sm_resps_++;
    my_assert(sm_err_type == erpc::SmErrType::kNoError);
    if (!(sm_event_type == erpc::SmEventType::kConnected ||
          sm_event_type == erpc::SmEventType::kDisconnected)) {
        throw std::runtime_error("Received unexpected SM event.");
    }
}

inline void basic_sm_handler_server(int session_num, erpc::SmEventType sm_event_type,
                             erpc::SmErrType sm_err_type, void *_context) {
    auto *c = static_cast<BasicContext *>(_context);
    c->num_sm_resps_++;
    my_assert(sm_err_type == erpc::SmErrType::kNoError);
    if (!(sm_event_type == erpc::SmEventType::kConnected ||
          sm_event_type == erpc::SmEventType::kDisconnected)) {
        throw std::runtime_error("Received unexpected SM event.");
    }
    c->session_num_vec_.push_back(session_num);
    // // printf("Server id %" PRIu8 ": Got session %d\n", c->rpc_->get_rpc_id(), session_num);
}

#endif  // ERPC_CXL

inline size_t get_bind_core(size_t numa) {
    static size_t numa0_core = 0;
    static size_t numa1_core = 0;
    static std::mutex core_mutex;
    size_t res;
    core_mutex.lock();
    my_assert(numa == 0 || numa == 1);
    if (numa == 0) {
        my_assert(numa0_core <= erpc::num_lcores_per_numa_node());
        res = numa0_core++;
    } else {
        my_assert(numa1_core <= erpc::num_lcores_per_numa_node());
        res = numa1_core++;
    }
    core_mutex.unlock();
    return res;
}
