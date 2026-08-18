#pragma once
#include "../social_network_commons.h"
#include "../social_network.pb.h"
#include "../spinlock_mutex.h"
#include <hdr/hdr_histogram.h>

#include <regex>
#ifdef ERPC_CXL
#include "../social_network_cxl.h"
#endif
#include "../post_data.h"

std::string nginx_addr;
std::string unique_id_addr;
std::string url_shorten_addr;
std::string user_mention_addr;
std::string user_timeline_addr;
std::string user_service_addr;
std::string home_timeline_addr;
std::string post_storage_addr;


class ReqState
{
public:

    explicit ReqState(uint32_t r_id, uint64_t cxl_offset, MPMC_QUEUE *queue, erpc::Rpc<erpc::CXLTransport> *rpc){
        req_id = r_id;
        rpc_ = rpc;
        
        void *cxl_ptr = social_network_cxl::get_cxl_allocator(rpc_)->offset_to_ptr(cxl_offset);
        cxl_post_ptr = reinterpret_cast<PostData *>(cxl_ptr);
        cxl_req_offset = cxl_offset;
        
        original_text = cxl_post_ptr->text;
        creator.set_user_id(cxl_post_ptr->creator_user_id);
        creator.set_username(cxl_post_ptr->creator_username);

        consumer_mpmc_queue = queue;
        finished_number = 0;
    }

    void send_first_step() {
        // unique id
        auto req_msgbuf1 = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
        auto *cxl_req_ptr1 = reinterpret_cast<PostStorageWriteCXLReq*>((req_msgbuf1.buf_) + sizeof(CommonReq));
        cxl_req_ptr1->offset = cxl_req_offset;
        new(req_msgbuf1.buf_) RPCMsgReq<PostStorageWriteCXLReq>(RPC_TYPE::RPC_UNIQUE_ID, req_id, *cxl_req_ptr1);
        consumer_mpmc_queue->push(req_msgbuf1);

        // user
        auto req_msgbuf2 = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
        auto *cxl_req_ptr2 = reinterpret_cast<PostStorageWriteCXLReq*>((req_msgbuf2.buf_) + sizeof(CommonReq));
        cxl_req_ptr2->offset = cxl_req_offset;
        new(req_msgbuf2.buf_) RPCMsgReq<PostStorageWriteCXLReq>(RPC_TYPE::RPC_COMPOSE_CREATOR_WITH_USER_ID, req_id, *cxl_req_ptr2);
        consumer_mpmc_queue->push(req_msgbuf2);

        // user_mention
        auto req_msgbuf3 = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
        auto *cxl_req_ptr3 = reinterpret_cast<PostStorageWriteCXLReq*>((req_msgbuf3.buf_) + sizeof(CommonReq));
        cxl_req_ptr3->offset = cxl_req_offset;
        new(req_msgbuf3.buf_) RPCMsgReq<PostStorageWriteCXLReq>(RPC_TYPE::RPC_USER_MENTION, req_id, *cxl_req_ptr3);
        consumer_mpmc_queue->push(req_msgbuf3);

        // url_shorten
        auto req_msgbuf4 = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
        auto *cxl_req_ptr4 = reinterpret_cast<PostStorageWriteCXLReq*>((req_msgbuf4.buf_) + sizeof(CommonReq));
        cxl_req_ptr4->offset = cxl_req_offset;
        new(req_msgbuf4.buf_) RPCMsgReq<PostStorageWriteCXLReq>(RPC_TYPE::RPC_URL_SHORTEN, req_id, *cxl_req_ptr4);
        consumer_mpmc_queue->push(req_msgbuf4);
    }

        void send_second_step() {
    //        // printf("ready to second step %u\n", req_id);
        size_t extra_length = sizeof(PostData);

        auto req_msgbuf = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
        auto *cxl_req_ptr = reinterpret_cast<PostStorageWriteCXLReq*>((req_msgbuf.buf_) + sizeof(CommonReq));
        cxl_req_ptr->post_id = cxl_post_ptr->post_id;
        cxl_req_ptr->offset = cxl_req_offset;
        cxl_req_ptr->size = extra_length;
        cxl_req_ptr->ref_count = 1;

        auto req1 = new(req_msgbuf.buf_) RPCMsgReq<PostStorageWriteCXLReq>(RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ, req_id, *cxl_req_ptr);
        consumer_mpmc_queue->push(req_msgbuf);

        auto req_msgbuf2 = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
        auto *cxl_req_ptr2 = reinterpret_cast<PostStorageWriteCXLReq*>((req_msgbuf2.buf_) + sizeof(CommonReq));
        cxl_req_ptr2->post_id = cxl_post_ptr->post_id;
        cxl_req_ptr2->offset = cxl_req_offset;
        cxl_req_ptr2->size = extra_length;
        cxl_req_ptr2->ref_count = 1;
        new(req_msgbuf2.buf_) RPCMsgReq<PostStorageWriteCXLReq>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_REQ, req_id, *cxl_req_ptr2);
        consumer_mpmc_queue->push(req_msgbuf2);

        req_msgbuf = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<UserTimeLineWriteReq>));
        new(req_msgbuf.buf_) RPCMsgReq<UserTimeLineWriteReq>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_REQ, req_id, {cxl_post_ptr->post_id, creator.user_id(), cxl_post_ptr->timestamp});
        consumer_mpmc_queue->push(req_msgbuf);
    }
    erpc::MsgBuffer generate_resp_msg(){
        erpc::MsgBuffer resp_buf = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<CommonRPCReq>));
        new (resp_buf.buf_) RPCMsgReq<CommonRPCReq>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP, req_id, {0});
        return resp_buf;
    }
    void  generate_next_step(){
        int now_number = finished_number.fetch_add(1);
        // // printf("%d\n",now_number);
        if(now_number == 3) {
            send_second_step();
        }
    }
 
    uint32_t req_id;
    std::string original_text;
    social_network::Creator creator;
    social_network::UrlShortenReq url_shorten_req;
    social_network::UserMentionReq user_mention_req;


    MPMC_QUEUE *consumer_mpmc_queue;
    erpc::Rpc<erpc::CXLTransport> *rpc_;

    PostData *cxl_post_ptr;
    uint64_t cxl_req_offset;

    // is always true after construct

    std::atomic<int> finished_number;
    spinlock_mutex mutex;
};

class ReqStateStore
{
public:
    std::map<uint32_t, ReqState*> req_state_map;
    spinlock_mutex mutex;
};

class ClientContext : public BasicContext
{
public:
    ClientContext(size_t cid, size_t sid, size_t rid) : client_id_(cid), server_sender_id_(sid), server_receiver_id_(rid)
    {
        forward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        forward_all_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        backward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);

        state_store = new ReqStateStore();
    }
    ~ClientContext()
    {
        delete forward_mpmc_queue;
        delete forward_all_mpmc_queue;
        delete backward_mpmc_queue;

        delete state_store;
    }
    erpc::MsgBuffer req_backward_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_backward_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_unique_id_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_unique_id_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_url_shorten_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_url_shorten_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_user_mention_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_user_mention_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_user_timeline_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_user_timeline_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_user_service_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_user_service_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_home_timeline_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_home_timeline_msgbuf[kAppMaxBuffer];

    erpc::MsgBuffer req_post_storage_msgbuf[kAppMaxBuffer];
    erpc::MsgBuffer resp_post_storage_msgbuf[kAppMaxBuffer];


    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;

    int nginx_session_number;
    int unique_id_session_number;
    int url_shorten_session_number;
    int user_mention_session_number;
    int user_timeline_session_number;
    int user_service_session_number;
    int home_timeline_session_number;
    int post_storage_session_number;

    MPMC_QUEUE *forward_mpmc_queue;
    MPMC_QUEUE *forward_all_mpmc_queue;
    MPMC_QUEUE *backward_mpmc_queue;

    ReqStateStore* state_store;
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
    size_t stat_req_compose_post_write_req_tot{};
    size_t stat_req_err_tot{};

    void reset_stat()
    {
        stat_req_ping_tot = 0;
        stat_req_compose_post_write_req_tot = 0;
        stat_req_err_tot = 0;
    }

    MPMC_QUEUE *forward_all_mpmc_queue{};

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
            client_contexts_.push_back(new ClientContext(i, (i % FLAGS_server_num) + kAppMaxRPC , i % FLAGS_server_num));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++)
        {
            auto *ctx = new ServerContext(i);
            ctx->forward_all_mpmc_queue = client_contexts_[i]->forward_all_mpmc_queue;
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
    auto value = config_json_all["nginx"]["server_addr"];
    my_assert(!value.is_null());
    nginx_addr = value;

    value= config_json_all["unique_id"]["server_addr"];
    my_assert(!value.is_null());
    unique_id_addr = value;

    value = config_json_all["url_shorten"]["server_addr"];
    my_assert(!value.is_null());
    url_shorten_addr = value;

    value = config_json_all["user_mention"]["server_addr"];
    my_assert(!value.is_null());
    user_mention_addr = value;

    value = config_json_all["user_timeline"]["server_addr"];
    my_assert(!value.is_null());
    user_timeline_addr = value;

    value = config_json_all["user_service"]["server_addr"];
    my_assert(!value.is_null());
    user_service_addr = value;

    value = config_json_all["home_timeline"]["server_addr"];
    my_assert(!value.is_null());
    home_timeline_addr = value;

    value = config_json_all["post_storage"]["server_addr"];
    my_assert(!value.is_null());
    post_storage_addr = value;
}

void compose_post_write_and_create(void *buf_, ReqStateStore* store,  MPMC_QUEUE *forward_queue, erpc::Rpc<erpc::CXLTransport> *c_rpc){
    auto* req = static_cast<RPCMsgReq<PostStorageWriteCXLReq> *>(buf_);

    store->mutex.lock();

    my_assert(!store->req_state_map.count(req->req_common.req_number));

    ReqState *req_state = new ReqState(req->req_common.req_number, req->req_control.offset, forward_queue, c_rpc);

    store->req_state_map[req->req_common.req_number] = req_state;

    store->mutex.unlock();

    req_state->send_first_step();

}