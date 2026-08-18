#pragma once
#include "atomic_queue/atomic_queue.h"
#include "../social_network_commons.h"
#include "../social_network.pb.h"
#include "../spinlock_mutex.h"
#include <future>
#include <mutex>

std::string client_addr;
std::string compose_post_addr;
std::string user_timeline_addr;
std::string home_timeline_addr;

size_t read_total_size_per_thread;
size_t write_total_size_per_thread;

bool write_to_mongodb;

class StorageHandler {
public:
    uint32_t req_number{};
    uint32_t rpc_type{};
    bool is_read{};
    std::vector<int64_t> post_ids;
    std::vector<std::pair<void*, size_t>> buffers;
    social_network::PostStorageReadResp resp;
};
using STORAGE_QUEUE = atomic_queue::AtomicQueueB2<StorageHandler*, std::allocator<StorageHandler*>, true, false, false>;

std::vector<STORAGE_QUEUE *> storage_queues;

class ClientContext : public BasicContext {
public:
    ClientContext(size_t cid, size_t sid, size_t rid) : client_id_(cid), server_sender_id_(sid), server_receiver_id_(rid) {
        forward_all_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        backward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        backward_mpmc_home_timeline_queue = new MPMC_QUEUE(kAppMaxBuffer);
    }
    ~ClientContext() {
        delete forward_all_mpmc_queue;
        delete backward_mpmc_queue;
        delete backward_mpmc_home_timeline_queue;
    }

    erpc::MsgBuffer req_backward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_backward_msgbuf[kAppMaxBuffer];

    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;

    int user_timeline_session_num_;
    int home_timeline_session_num_;
    int compose_post_session_num_;
    int now_session_;
    int client_session_num_;

    MPMC_QUEUE *forward_all_mpmc_queue;
    MPMC_QUEUE *backward_mpmc_queue;
    MPMC_QUEUE *backward_mpmc_home_timeline_queue;
};

class ServerContext : public BasicContext {
public:
    explicit ServerContext(size_t sid) : server_id_(sid) {}
    ~ServerContext() = default;
    size_t server_id_{};
    size_t stat_req_ping_tot{};
    size_t stat_req_post_storage_read_tot{};
    size_t stat_req_post_storage_write_tot{};
    size_t stat_req_err_tot{};

    void reset_stat() {
        stat_req_ping_tot = 0;
        stat_req_post_storage_read_tot = 0;
        stat_req_post_storage_write_tot = 0;
        stat_req_err_tot = 0;
    }
  
    std::unordered_map<int64_t, std::pair<void*, size_t>> post_storage_map;
    spinlock_mutex map_mutex;
};

class AppContext {
public:
    AppContext() {
        for (size_t i = 0; i < FLAGS_client_num; i++) {
            client_contexts_.push_back(new ClientContext(i, i % FLAGS_server_num, (i % FLAGS_server_num) + kAppMaxRPC));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++) {
            server_contexts_.push_back(new ServerContext(i));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++) {
            storage_queues.push_back(new STORAGE_QUEUE(kAppMaxBuffer));
        }
    }
    ~AppContext() {
        for (auto &ctx : client_contexts_) delete ctx;
        for (auto &ctx : server_contexts_) delete ctx;
        for (auto &q : storage_queues) delete q;
    }

    std::vector<ClientContext *> client_contexts_;
    std::vector<ServerContext *> server_contexts_;
};

void init_specific_config() {
    auto value = config_json_all["client"]["server_addr"];
    my_assert(!value.is_null(), "value is null");
    client_addr = value.get<std::string>();

    value = config_json_all["user_timeline"]["server_addr"];
    my_assert(!value.is_null(), "value is null");
    user_timeline_addr = value.get<std::string>();
    value = config_json_all["compose_post"]["server_addr"];    my_assert(!value.is_null(), "value is null");    compose_post_addr = value.get<std::string>();

    value = config_json_all["home_timeline"]["server_addr"];
    my_assert(!value.is_null(), "value is null");
    home_timeline_addr = value.get<std::string>();

    value = config_json_all["post_storage"]["read_total_size"];
    my_assert(!value.is_null(), "value is null");
    read_total_size_per_thread = value;

    value = config_json_all["post_storage"]["write_total_size"];
    my_assert(!value.is_null(), "value is null");
    write_total_size_per_thread = value;

    value = config_json_all["post_storage"]["write_to_mongodb"];
    my_assert(!value.is_null(), "value is null");
    write_to_mongodb = value;
}
