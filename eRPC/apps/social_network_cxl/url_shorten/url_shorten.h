#pragma once
#include "../social_network_commons.h"
#include "../social_network.pb.h"
#include <regex>
#ifdef ERPC_CXL
#include "../social_network_cxl.h"
#endif
#include "../post_data.h"

#define HOSTNAME "http://short-url/"

std::string compose_post_addr;
 
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
    erpc::MsgBuffer req_backward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_backward_msgbuf[kAppMaxBuffer];

    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;
    int backward_session_num_;
    SPSC_QUEUE *forward_spsc_queue;
    SPSC_QUEUE *backward_spsc_queue;
};

class ServerContext : public BasicContext {
public:
    explicit ServerContext(size_t sid) : server_id_(sid) {}
    ~ServerContext() = default;
    size_t server_id_{};
    size_t stat_req_ping_tot{};
    size_t stat_req_url_shorten_tot{};
    size_t stat_req_err_tot{};

    void reset_stat() {
        stat_req_ping_tot = 0;
        stat_req_url_shorten_tot = 0;
        stat_req_err_tot = 0;
    }

    SPSC_QUEUE *forward_spsc_queue{};
};

class AppContext {
public:
    AppContext() {
        for (size_t i = 0; i < FLAGS_client_num; i++) {
            client_contexts_.push_back(new ClientContext(i, i % FLAGS_server_num, i % FLAGS_server_num));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++) {
            auto *ctx = new ServerContext(i);
            ctx->forward_spsc_queue = client_contexts_[i]->forward_spsc_queue;
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
    auto value = config_json_all["compose_post"]["server_addr"];
    my_assert(!value.is_null(), "value is null");
    compose_post_addr = value.get<std::string>();
}

std::string generate_random_string() {
    static const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr size_t kLength = 10;

    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string result(kLength, '\0');
    for (size_t i = 0; i < kLength; ++i) {
        result[i] = charset[dis(gen)];
    }
    return result;
}

social_network::UrlShortenResp generate_shorten_urls(const social_network::UrlShortenReq& req) {
    social_network::UrlShortenResp resp;
    for (auto url : req.urls()) {
        auto new_url = resp.add_urls();
        new_url->set_shortened_url(std::move(url));
        new_url->set_expanded_url(HOSTNAME + generate_random_string());
    }
    return resp;
}
