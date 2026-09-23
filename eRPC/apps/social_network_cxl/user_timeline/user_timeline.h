#pragma once
#include "../social_network_commons.h"
#include "../ub_breakdown.h"
#include "../utils_mongodb.h"
#include <social_network.pb.h>
#include "../spinlock_mutex.h"
#include <iterator>
#include <hdr_histogram.h>
#include <future>
#include <map>
#include <set>
 
std::string compose_post_addr;
std::string nginx_addr;
std::string post_storage_addr;
// std::string client_addr;

int mongodb_conns_num;
mongoc_client_pool_t* mongodb_client_pool;
std::unordered_map<int64_t, std::set<int64_t>> user_timeline_map;


class ClientContext : public BasicContext
{
public:
    ClientContext(size_t cid, size_t sid, size_t rid) : client_id_(cid), server_sender_id_(sid), server_receiver_id_(rid)
    {
        forward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        forward_all_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        backward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        nginx_session_number = 0;
        // client_session_number = 0;
        compose_post_session_number = 0;
        post_storage_session_number = 0;
    }
    ~ClientContext()
    {
        delete forward_mpmc_queue;
        delete forward_all_mpmc_queue;
        delete backward_mpmc_queue;
    }
    erpc::MsgBuffer req_forward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer req_backward_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer resp_forward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_backward_msgbuf[kAppMaxBuffer];
    uint64_t post_storage_start_ns[kAppMaxBuffer]{};
    std::atomic<uint64_t> read_queue_start_ns[kAppMaxBuffer]{};
    std::atomic<uint64_t> forward_queue_start_ns[kAppMaxBuffer]{};

    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;

    int nginx_session_number;
    // int client_session_number;
    int compose_post_session_number;
    int post_storage_session_number;

    MPMC_QUEUE *forward_mpmc_queue;
    MPMC_QUEUE *forward_all_mpmc_queue;
    MPMC_QUEUE *backward_mpmc_queue;
};

class ServerContext : public BasicContext
{
public:
    explicit ServerContext(size_t sid) : server_id_(sid)
    {
    }
    ~ServerContext()
    = default;
    size_t server_id_{};
    size_t stat_req_ping_tot{};
    size_t stat_req_user_timeline_write_req_tot{};
    size_t stat_req_user_timeline_read_req_tot{};
    size_t stat_req_post_storage_read_resp_tot{};
    size_t stat_req_err_tot{};

    spinlock_mutex init_mutex;
    bool is_pinged{false};
    uint32_t ping_req_number{0};
    std::atomic<bool> mongodb_init_finished{false};

    void reset_stat()
    {
        stat_req_ping_tot = 0;
        stat_req_user_timeline_write_req_tot = 0;
        stat_req_user_timeline_read_req_tot = 0;
        stat_req_post_storage_read_resp_tot = 0;
        stat_req_err_tot = 0;
    }

    MPMC_QUEUE *forward_all_mpmc_queue{};
    std::atomic<uint64_t> *read_queue_start_ns{};

    erpc::MsgBuffer *req_forward_msgbuf_ptr{};
    erpc::MsgBuffer *req_backward_msgbuf_ptr{};
};

class AppContext
{
public:
    AppContext()
    {
        // Performance profiling disabled.
        // int ret = hdr_init(1, 1000 * 1000 * 10, 3,
        //                    &latency_hist_);
        // my_assert(ret == 0);
        for (size_t i = 0; i < FLAGS_client_num; i++)
        {
            client_contexts_.push_back(new ClientContext(i, (i % FLAGS_server_num) + kAppMaxRPC, i % FLAGS_server_num));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++)
        {
            auto *ctx = new ServerContext(i);
            ctx->forward_all_mpmc_queue = client_contexts_[i]->forward_all_mpmc_queue;
            ctx->read_queue_start_ns = client_contexts_[i]->read_queue_start_ns;
            ctx->req_forward_msgbuf_ptr = client_contexts_[i]->req_forward_msgbuf;
            ctx->req_backward_msgbuf_ptr = client_contexts_[i]->req_backward_msgbuf;
            server_contexts_.push_back(ctx);
        }
    }
    ~AppContext()
    {
        // Performance profiling disabled.
        // hdr_close(latency_hist_);

        for (auto &ctx : client_contexts_)
        {
            delete ctx;
        }
        for (auto &ctx : server_contexts_)
        {
            delete ctx;
        }
    }

    [[maybe_unused]] [[nodiscard]] bool write_latency_and_reset(const std::string &filename) const
    {

        // Performance profiling disabled.
        (void)filename;
        return true;
    }

    std::vector<ClientContext *> client_contexts_;
    std::vector<ServerContext *> server_contexts_;

    hdr_histogram *latency_hist_{};
};

// must be used after init_service_config

void init_specific_config(){
    auto value = config_json_all["compose_post"]["server_addr"];
    my_assert(!value.is_null());
    compose_post_addr = value;

    value = config_json_all["nginx"]["server_addr"];
    my_assert(!value.is_null());
    nginx_addr = value;

    value = config_json_all["post_storage"]["server_addr"];
    my_assert(!value.is_null());
    post_storage_addr = value;

    // value = config_json_all["client"]["server_addr"];
    // my_assert(!value.is_null());
    // client_addr = value;

    auto conns = config_json_all["user_timeline_mongodb"]["connections"];
    my_assert(!conns.is_null());
    mongodb_conns_num = conns;
}

void read_post_details(void *buf_, AppRpc *rpc_, MPMC_QUEUE *consumer_fwd, MPMC_QUEUE *consumer_back, std::atomic<uint64_t> *forward_queue_start_ns) {
    auto* req = static_cast<RPCMsgReq<UserTimeLineReq> *>(buf_);

    bool is_fwd = true;

    if(req->req_control.stop <= req->req_control.start || req->req_control.start < 0){
//        // printf("start stop %d %d\n", req->req_control.start, req->req_control.stop);
        is_fwd = false;
    }

    auto timeline_it = user_timeline_map.find(req->req_control.user_id);
    if(timeline_it == user_timeline_map.end()){
//        // printf("user id %ld not exist\n", req->req_control.user_id);
        is_fwd = false;
    }
    if (is_fwd && req->req_control.start >=
                      static_cast<int>(timeline_it->second.size())) {
//        for(size_t tmp: post_ids){
//            // printf("%ld ", tmp);
//        }
//        // printf(" start %d \n", req->req_control.start);
        is_fwd = false;
    }

    if(!is_fwd){
//        // printf("don't fwd, req number is %u\n", req->req_common.req_number);
        erpc::MsgBuffer resp_buf = rpc_->alloc_msg_buffer_or_die(
            sizeof(RPCMsgReq<PostStorageReadCXLResp>));
        new (resp_buf.buf_) RPCMsgReq<PostStorageReadCXLResp>(
            RPC_TYPE::RPC_USER_TIMELINE_READ_RESP,
            req->req_common.req_number, {});
        consumer_back->push(resp_buf);
        return;
    }

    erpc::MsgBuffer fwd_req = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageReadCXLReq>));
    auto* fwd_req_msg = new (fwd_req.buf_) RPCMsgReq<PostStorageReadCXLReq>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ, req->req_common.req_number, {});
    auto post_it = timeline_it->second.begin();
    std::advance(post_it, req->req_control.start);
    fwd_req_msg->req_control.post_id = *post_it;
    const uint64_t queue_start = sn_profile::start();
    if (queue_start != 0) {
        const size_t slot = req->req_common.req_number % kAppMaxBuffer;
        forward_queue_start_ns[slot].store(queue_start,
                                            std::memory_order_relaxed);
    }
    consumer_fwd->push(fwd_req);
}

void write_post_ids_and_return(void *buf_, AppRpc *rpc_, MPMC_QUEUE *consumer_back) {
    auto* req = static_cast<RPCMsgReq<UserTimeLineWriteReq> *>(buf_);

    erpc::MsgBuffer resp_buf = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<UserTimeLineWriteReq>));

    auto* resp = new (resp_buf.buf_) RPCMsgReq<UserTimeLineWriteReq>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_RESP, req->req_common.req_number,
                                                                    {req->req_control.post_id, req->req_control.user_id, req->req_control.timestamp });

    std::set<int64_t> &post_ids = user_timeline_map[req->req_control.user_id];
    if(post_ids.count(req->req_control.post_id)){
        resp->req_control.timestamp++;
        consumer_back->push(resp_buf);
        return;
    }

    UserTimeLineWriteReq user_timeline_write_req = req->req_control;

    consumer_back->push(resp_buf); // Return immediately to avoid MongoDB latency on the critical path

    std::thread( [=](UserTimeLineWriteReq r) {
        mongoc_client_t *mongodb_client = mongoc_client_pool_pop(mongodb_client_pool);
        auto collection = mongoc_client_get_collection(mongodb_client, "user-timeline-write", "user-timeline-write");

        bson_t *query = bson_new();

        BSON_APPEND_INT64(query, "user_id", r.user_id);
        bson_t *update =
                BCON_NEW("$push", "{", "posts", "{", "$each", "[", "{", "post_id",
                         BCON_INT64(r.post_id), "timestamp", BCON_INT64(r.timestamp), "}",
                         "]", "$position", BCON_INT32(0), "}", "}");

        bson_error_t error;
        bson_t reply;

        bool updated = mongoc_collection_find_and_modify(collection, query, nullptr,
                                                         update, nullptr, false, true,
                                                         true, &reply, &error);

        if (!updated) {
            // update the newly inserted document (upsert: false)
            updated = mongoc_collection_find_and_modify(collection, query, nullptr,
                                                        update, nullptr, false, false,
                                                        true, &reply, &error);
            if (!updated) {
                fprintf(stderr, "Error: mongodb update error! %ld %ld %ld\n", r.user_id, r.post_id, r.timestamp);
                exit(1);
            }
        }
        bson_destroy(update);
        bson_destroy(&reply);
        bson_destroy(query);
        mongoc_collection_destroy(collection);
        mongoc_client_pool_push(mongodb_client_pool, mongodb_client);

    },user_timeline_write_req).detach();
}
