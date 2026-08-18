#pragma once
#include "../social_network_commons.h"
#include "../social_network.pb.h"
#include "../spinlock_mutex.h"
#ifdef ERPC_CXL
#include "../social_network_cxl.h"
#endif
#include "../post_data.h"
#include <hdr/hdr_histogram.h>
#include <future>
#include <map>
#include <set>

std::string compose_post_addr;
std::string nginx_addr;
std::string post_storage_addr;
// std::string client_addr;
std::string data_file_path;

int mongodb_conns_num;
mongoc_client_pool_t* mongodb_client_pool;
std::unordered_map<int64_t, std::set<int64_t>> user_followers_map;
std::unordered_map<int64_t, std::vector<int64_t>> user_home_timeline_map;

std::queue<int64_t> post_ids_queue;

static int64_t per_read_posts = 3;

class ClientContext : public BasicContext
{
public:
    ClientContext(size_t cid, size_t sid, size_t rid) : client_id_(cid), server_sender_id_(sid), server_receiver_id_(rid)
    {
        forward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        forward_all_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
        backward_mpmc_queue = new MPMC_QUEUE(kAppMaxBuffer);
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

    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;

    int nginx_session_number{};
    // int client_session_number{};
    int compose_post_session_number{};
    int post_storage_session_number{};

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
    size_t stat_req_home_timeline_write_req_tot{};
    size_t stat_req_home_timeline_read_req_tot{};
    size_t stat_req_post_storage_read_resp_tot{};
    size_t stat_req_err_tot{};

    spinlock_mutex init_mutex;
    bool is_pinged{false};
    uint32_t ping_req_number{0};
    bool mongodb_init_finished{false};

    void reset_stat()
    {
        stat_req_ping_tot = 0;
        stat_req_home_timeline_write_req_tot = 0;
        stat_req_home_timeline_read_req_tot = 0;
        stat_req_post_storage_read_resp_tot = 0;
        stat_req_err_tot = 0;
    }

    MPMC_QUEUE *forward_all_mpmc_queue{};

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

//void load_timeline_storage(const std::string& file_path)
//{
//    social_network::HomeTimelineStorage home_timeline_storage;
//    std::ifstream input(file_path, std::ios::binary);
//    my_assert(input.is_open());
//
//    if (!home_timeline_storage.ParseFromIstream(&input)) {
//        RMEM_ERROR("Failed to parse home_timeline_storage.");
//        exit(-1);
//    }
//
//    for(const auto& pair : home_timeline_storage.users_to_posts()) {
//        std::vector<int64_t >vec;
//        for(const auto& value : pair.second.post_ids()) {
//            vec.push_back(value);
//        }
//        user_home_timeline_map[pair.first] = std::move(vec);
//    }
//    RMEM_INFO("Timeline storage load finished! Total init %ld users", user_home_timeline_map.size());
//
//}
//
//void store_timeline_storage(const std::string& file_path)
//{
//    social_network::HomeTimelineStorage home_timeline_storage;
//    for (const auto& pair : user_home_timeline_map) {
//        social_network::VecPostID &vec =  home_timeline_storage.mutable_users_to_posts()->at(pair.first) ;
//        for (const auto& value : pair.second) {
//            vec.add_post_ids(value);
//        }
//    }
//
//    std::ofstream output(file_path, std::ios::binary);
//    my_assert(output.is_open());
//
//    if (!home_timeline_storage.SerializeToOstream(&output)) {
//        RMEM_ERROR("Failed to serial home_timeline_storage.");
//        exit(-1);
//    }
//
//    RMEM_INFO("Timeline storage store finished! Total init %ld users", user_home_timeline_map.size());
//}

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

    value = config_json_all["home_timeline"]["data_file_path"];
    my_assert(!value.is_null());
    data_file_path = value;

    value = config_json_all["home_timeline"];
    if(value.find("per_read_posts") != value.end()){
        per_read_posts = value["per_read_posts"];
        // // printf("[home_timeline] per_read_posts is %ld\n", per_read_posts);
    }

    auto conns = config_json_all["social_graph_mongodb"]["connections"];
    my_assert(!conns.is_null());
    mongodb_conns_num = conns;
}

void read_home_time_line_post_details(void *buf_, erpc::Rpc<erpc::CXLTransport> *rpc_, MPMC_QUEUE *consumer_fwd, MPMC_QUEUE *consumer_back) {
    auto* req = static_cast<RPCMsgReq<HomeTimeLineReq> *>(buf_);

    bool is_fwd = true;

    if(req->req_control.stop_idx <= req->req_control.start_idx || req->req_control.start_idx < 0){
        is_fwd = false;
    }

//    if(!user_home_timeline_map.contains(req->req_control.user_id)){
//        is_fwd = false;
//    } 
    std::vector<int64_t> post_ids;
    if (!post_ids_queue.empty()) {
        int64_t num = std::min((int64_t)per_read_posts, (int64_t)post_ids_queue.size());
        while(num--){
            int64_t tmp = post_ids_queue.front();
            post_ids.push_back(tmp);
            post_ids_queue.pop();
            post_ids_queue.push(tmp);
        }
    }

    if(req->req_control.start_idx >= static_cast<int>(post_ids.size())){
        is_fwd = false;
    }

    if(!is_fwd){
        erpc::MsgBuffer resp_buf = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<CommonRPCReq>));
        new (resp_buf.buf_) RPCMsgReq<CommonRPCReq>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP, req->req_common.req_number, {0});
        resp_buf.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP));
        resp_buf.set_hdr_req_num(req->req_common.req_number);
        __sync_synchronize();
        consumer_back->push(resp_buf);
        return;
    }

    erpc::MsgBuffer fwd_req = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageReadCXLReq>));
    auto* fwd_req_msg = new (fwd_req.buf_) RPCMsgReq<PostStorageReadCXLReq>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ, req->req_common.req_number, {});
    fwd_req.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ));
    fwd_req.set_hdr_req_num(req->req_common.req_number);
    fwd_req_msg->req_control.rpc_type = static_cast<uint32_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ);
    fwd_req_msg->req_control.count = 0;

    int now_index = 0;
    for(int64_t & post_id : post_ids){
        if(req->req_control.start_idx<=now_index && now_index<req->req_control.stop_idx){
            fwd_req_msg->req_control.post_ids[fwd_req_msg->req_control.count++] = post_id;
        }
        now_index++;
        if(now_index==req->req_control.stop_idx){
            break;
        }
    }

    __sync_synchronize();
    consumer_fwd->push(fwd_req);
}

void write_home_timeline_and_return(void *buf_, erpc::Rpc<erpc::CXLTransport> *rpc_, MPMC_QUEUE *consumer_back) {
    auto* req = static_cast<RPCMsgReq<PostStorageWriteCXLReq> *>(buf_);

#ifdef ERPC_CXL
    uint64_t cxl_offset = req->req_control.offset;
    void *cxl_ptr = social_network_cxl::get_cxl_allocator(rpc_)->offset_to_ptr(cxl_offset);
    PostData *cxl_post_ptr = reinterpret_cast<PostData *>(cxl_ptr);

    auto it = user_followers_map.find(cxl_post_ptr->creator_user_id);
    if (it != user_followers_map.end()) {
        for(auto m: it->second) {
            user_home_timeline_map[m].push_back(cxl_post_ptr->post_id);
        }
    }

    for(size_t mi = 0; mi < cxl_post_ptr->mentions_count; mi++) {
        int64_t m = cxl_post_ptr->mentions_ids[mi];
        if (it == user_followers_map.end() || it->second.find(m) == it->second.end()) {
            user_home_timeline_map[m].push_back(cxl_post_ptr->post_id);
        }
    }
#endif

    erpc::MsgBuffer resp_buf = rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<CommonRPCReq>));
    new (resp_buf.buf_) RPCMsgReq<CommonRPCReq>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP, req->req_common.req_number, {0});
    resp_buf.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP));
    resp_buf.set_hdr_req_num(req->req_common.req_number);
    __sync_synchronize();
    consumer_back->push(resp_buf);
}
