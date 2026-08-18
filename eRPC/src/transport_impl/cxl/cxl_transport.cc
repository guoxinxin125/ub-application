#ifdef ERPC_CXL
#include "cxl_transport.h"

#include "pkthdr.h"
#include "util/huge_alloc.h"
#include "util/logger.h"
#include "util/timer.h"

#include <cstring>

namespace erpc {
constexpr TransportType CXLTransport::kTransportType;
constexpr size_t CXLTransport::kMTU;
constexpr size_t CXLTransport::kPostlist;
constexpr size_t CXLTransport::kUnsigBatch;
constexpr size_t CXLTransport::kMaxDataPerPkt;
constexpr size_t CXLTransport::kNumRxRingEntries;
constexpr size_t CXLTransport::kCXLMemorySize;
constexpr size_t CXLTransport::kBandwidthGbps;

CXLTransport::CXLTransport(uint16_t /* sm_udp_port */, uint8_t rpc_id,
                           uint8_t phy_port, size_t numa_node,
                           FILE *trace_file)
    : Transport(TransportType::kCXL, rpc_id, phy_port, numa_node, trace_file),
      rx_ring_(nullptr),
      rx_ring_head_(0),
      shared_allocator_(nullptr) {
    try {
        shared_allocator_.reset(new CXLSharedAllocator(
            rpc_id, cxl_config::get_cacheable_memory_size(), numa_node));
    } catch (const std::exception& e) {
        printf("CXLTransport: Initialization failed: %s\n", e.what());
        throw;
    }
}

CXLTransport::~CXLTransport() = default;

void CXLTransport::init_hugepage_structures(HugeAlloc *huge_alloc,
                                            uint8_t **rx_ring) {
    _unused(huge_alloc);
    rx_ring_ = rx_ring;

    rt_assert(rx_ring != nullptr, "RX ring cannot be null");
    for (size_t i = 0; i < kNumRxRingEntries; i++) {
        rx_ring[i] = reinterpret_cast<uint8_t*>(&rx_pkthdr_ring_[i]);
        rx_payload_ring_[i] = nullptr;
    }

    reg_mr_func_ = [](void* addr, size_t /*size*/) -> Transport::mem_reg_info {
        return Transport::mem_reg_info(addr, 0);
    };

    dereg_mr_func_ = [](Transport::mem_reg_info mr_info) {
        _unused(mr_info);
    };
}

void CXLTransport::fill_local_routing_info(
    routing_info_t *routing_info) const {
    rt_assert(routing_info != nullptr, "Routing info cannot be null");

    // [0]     保留，resolve_remote_routing_info() 中会写入目标 RPC ID。
    // [1..4]  本地 rpc_id_，也就是发送者 RPC ID。
    // [5..12] 时间戳，仅用于调试和观测。
    uint8_t* buf = reinterpret_cast<uint8_t*>(routing_info->buf_);

    buf[0] = 0;  // 占位符，resolve 阶段会设置为目标队列 ID。
    *reinterpret_cast<uint32_t*>(buf + 1) = rpc_id_;
    *reinterpret_cast<uint64_t*>(buf + 5) = rdtsc();
}

bool CXLTransport::resolve_remote_routing_info(
    routing_info_t *routing_info) const {
    rt_assert(routing_info != nullptr, "Routing info cannot be null");

    // SM 握手返回的 routing_info 中，[1..4] 保存远端服务器 RPC ID。
    // CXL 发送路径用 buf[0] 作为目标指针队列索引。
    uint8_t* buf = reinterpret_cast<uint8_t*>(routing_info->buf_);
    uint32_t target_rpc_id = *reinterpret_cast<uint32_t*>(buf + 1);

    buf[0] = static_cast<uint8_t>(target_rpc_id);
    *reinterpret_cast<uint64_t*>(buf + 5) = rdtsc();

    return true;
}

size_t CXLTransport::get_bandwidth() const {
    return kBandwidthGbps * 1000000000ULL / 8;
}

std::string CXLTransport::routing_info_str(routing_info_t *routing_info) {
    uint32_t rpc_id = *reinterpret_cast<uint32_t*>(routing_info->buf_);
    uint64_t timestamp = *reinterpret_cast<uint64_t*>(routing_info->buf_ + 4);
    return "[CXL RPC " + std::to_string(rpc_id) + ", TS " +
           std::to_string(timestamp) + "]";
}

Buffer CXLTransport::alloc_shared_buffer(size_t size) {
    return shared_allocator_->alloc(size);
}

void CXLTransport::free_shared_buffer(Buffer buffer) {
    shared_allocator_->free(buffer);
}

void CXLTransport::register_rx_peer(uint8_t peer_rpc_id) {
#ifdef USE_SPSC_QUEUE
    shared_allocator_->register_rx_peer(peer_rpc_id);
#else
    _unused(peer_rpc_id);
#endif
}

void CXLTransport::tx_burst(const tx_burst_item_t *tx_burst_arr,
                            size_t num_pkts) {
    for (size_t i = 0; i < num_pkts; i++) {
        const tx_burst_item_t& item = tx_burst_arr[i];
        if (unlikely(item.drop_)) continue;

        MsgBuffer* msg_buf = item.msg_buffer_;
        pkthdr_t* pkthdr = msg_buf->get_pkthdr_n(item.pkt_idx_);

        rt_assert(item.routing_info_ != nullptr,
                  "routing info is null in tx_burst");
        const uint8_t* buf =
            reinterpret_cast<const uint8_t*>(item.routing_info_->buf_);
        uint8_t target_queue_id = buf[0];  // 目标队列是接收方 RPC ID。

        void* data_ptr = nullptr;
        bool need_free = false;
        bool added_queue_ref = false;
        bool payload_has_msgbuf_header = false;
        Buffer queue_ref_buffer;
        Buffer temp_buffer;

        rt_assert(item.pkt_idx_ == 0,
                  "CXL transport expects one queue entry per message");
        const size_t payload_offset = 0;
        const size_t payload_len = pkthdr->msg_size_;

        if (shared_allocator_->is_shared_ptr(msg_buf->buf_)) {
            // payload 已经位于 CXL 共享内存中，直接传递其地址。
            data_ptr = msg_buf->buf_ + payload_offset;
            payload_has_msgbuf_header = true;
        } else {
            // payload 不在 CXL 共享内存中，需要先复制到临时 CXL buffer。
            temp_buffer = payload_len == 0
                ? Buffer(nullptr, 0, 0)
                : shared_allocator_->alloc(payload_len);

            if (payload_len > 0 && temp_buffer.buf_ == nullptr) {
                continue;
            }

            if (payload_len > 0) {
                void* src_data = msg_buf->buf_ + payload_offset;
                memcpy(temp_buffer.buf_, src_data, payload_len);
            }

            data_ptr = temp_buffer.buf_;
            need_free = true;
        }

        // 队列项携带一份 packet header，接收方无需先读 payload 即可路由。
        pkthdr_t entry_pkthdr = *pkthdr;
        entry_pkthdr.is_zerocopy_ =
            payload_len > 0 && payload_has_msgbuf_header;

        if (payload_len > 0) {
            queue_ref_buffer = entry_pkthdr.is_zerocopy_ != 0
                ? Buffer(static_cast<uint8_t*>(data_ptr) - sizeof(pkthdr_t),
                         payload_len + sizeof(pkthdr_t), 0)
                : Buffer(static_cast<uint8_t*>(data_ptr), payload_len, 0);
            shared_allocator_->add_ref(queue_ref_buffer);
            added_queue_ref = true;
        }

        bool success = shared_allocator_->enqueue_ptr(
            target_queue_id,
            data_ptr,
            kCXLQueueEntryRpc,
            &entry_pkthdr);

        if (success) {
            if (need_free) {
                shared_allocator_->free(temp_buffer);
            }
        } else {
            if (added_queue_ref) {
                shared_allocator_->free(queue_ref_buffer);
            }
            if (need_free) {
                shared_allocator_->free(temp_buffer);
            }
        }
    }
}

size_t CXLTransport::rx_burst() {
    size_t num_received = 0;
    uint8_t my_queue_id = rpc_id_;

    void* data_ptr;
    uint8_t entry_type;
    pkthdr_t pkthdr;

    while (num_received < kNumRxRingEntries &&
           shared_allocator_->dequeue_ptr(my_queue_id, &data_ptr, &entry_type,
                                          &pkthdr)) {
        if (entry_type == kCXLQueueEntryFree) {
            // 跨 endpoint 回收消息，由分配者最终释放元数据和 payload。
            auto* metadata =
                cxl_block_metadata_t::from_data(static_cast<uint8_t*>(data_ptr));
            cacheable.free(metadata);
            continue;
        }

        rx_pkthdr_ring_[rx_ring_head_] = pkthdr;
        rx_payload_ring_[rx_ring_head_] = static_cast<uint8_t*>(data_ptr);
        rx_ring_[rx_ring_head_] =
            reinterpret_cast<uint8_t*>(&rx_pkthdr_ring_[rx_ring_head_]);

        rx_ring_head_ = (rx_ring_head_ + 1) % kNumRxRingEntries;
        num_received++;
    }

    return num_received;
}

void CXLTransport::tx_flush() {
    // CXL 使用共享内存队列，不需要额外的 transport flush。
}

void CXLTransport::post_recvs(size_t num_recvs) {
    // CXL 使用共享内存接收队列，不需要 post receive。
    _unused(num_recvs);
}

} // namespace erpc

#endif // ERPC_CXL
