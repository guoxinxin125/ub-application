#pragma once
#include "../social_network_commons.h"

DEFINE_string(load_balance_servers, "", "load balance servers, split by ','");

std::string client_addr;

#include <atomic>

std::atomic<size_t> send_ping_resp_num[10000];

class ClientContext : public BasicContext {
public:
    ClientContext(size_t cid, size_t sid, size_t rid) : client_id_(cid), server_sender_id_(sid), server_receiver_id_(rid), backward_session_num_(-1) {
        forward_spsc_queue = new SPSC_QUEUE(kAppMaxBuffer);
        backward_spsc_queue = new SPSC_QUEUE(kAppMaxBuffer);
    }
    ~ClientContext() {
        delete forward_spsc_queue;
        delete backward_spsc_queue;
    }
    erpc::MsgBuffer req_forward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer req_backward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_forward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_backward_msgbuf[kAppMaxBuffer];

    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;
    int backward_session_num_;
    int servers_num_{};

    SPSC_QUEUE *forward_spsc_queue;
    SPSC_QUEUE *backward_spsc_queue;
};

class ServerContext : public BasicContext {
public:
    explicit ServerContext(size_t sid) : server_id_(sid) {}
    ~ServerContext() = default;
    size_t server_id_{};
    size_t stat_req_ping_tot{};
    size_t stat_req_ping_resp_tot{};
    size_t stat_req_compose_post_tot{};
    size_t stat_req_compose_post_resp_tot{};
    size_t stat_req_user_timeline_tot{};
    size_t stat_req_user_timeline_resp_tot{};
    size_t stat_req_home_timeline_tot{};
    size_t stat_req_home_timeline_resp_tot{};
    size_t stat_req_err_tot{};

    void reset_stat() {
        stat_req_ping_tot = 0;
        stat_req_ping_resp_tot = 0;
        stat_req_compose_post_tot = 0;
        stat_req_compose_post_resp_tot = 0;
        stat_req_user_timeline_tot = 0;
        stat_req_user_timeline_resp_tot = 0;
        stat_req_home_timeline_tot = 0;
        stat_req_home_timeline_resp_tot = 0;
        stat_req_err_tot = 0;
    }
 
    SPSC_QUEUE *forward_spsc_queue{};
    SPSC_QUEUE *backward_spsc_queue{};
    erpc::MsgBuffer *req_forward_msgbuf_ptr{};
    erpc::MsgBuffer *req_backward_msgbuf_ptr{};
    int servers_num_{};
};

class AppContext {
public:
    AppContext() {
        for (size_t i = 0; i < FLAGS_client_num; i++) {
            client_contexts_.push_back(new ClientContext(i, i % FLAGS_server_num, (i % FLAGS_server_num) + kAppMaxRPC));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++) {
            auto *ctx = new ServerContext(i);
            ctx->forward_spsc_queue = client_contexts_[i]->forward_spsc_queue;
            ctx->backward_spsc_queue = client_contexts_[i]->backward_spsc_queue;
            ctx->req_forward_msgbuf_ptr = client_contexts_[i]->req_forward_msgbuf;
            ctx->req_backward_msgbuf_ptr = client_contexts_[i]->req_backward_msgbuf;
            server_contexts_.push_back(ctx);
        }
    }
    ~AppContext() {
        for (auto &ctx : client_contexts_) {
            delete ctx;
        }
        for (auto &ctx : server_contexts_) {
            delete ctx;
        }
    }

    std::vector<ClientContext *> client_contexts_;
    std::vector<ServerContext *> server_contexts_;
};

void init_specific_config() {
    my_assert(!config_json_all.is_null());
    auto value = config_json_all["nginx"]["server_addr"];
    my_assert(!value.is_null());
    FLAGS_load_balance_servers = value.get<std::string>();

    value = config_json_all["client"]["server_addr"];
    my_assert(!value.is_null());
    client_addr = value.get<std::string>();
}

std::vector<std::string> flags_get_balance_servers_index() {
    my_assert(!FLAGS_load_balance_servers.empty());
    std::vector<std::string> split_vec = split(FLAGS_load_balance_servers, ',');
    my_assert(!split_vec.empty());
    return split_vec;
}
