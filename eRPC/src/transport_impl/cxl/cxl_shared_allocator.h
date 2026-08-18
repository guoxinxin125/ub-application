/**
 * @file cxl_shared_allocator.h
 * @brief CXL 共享内存分配器，用于零拷贝 RPC。
 */
#pragma once

#ifdef ERPC_CXL

#include "common.h"
#include "transport.h"
#include "util/buffer.h"
#include <atomic>
#include <cstring>
#include <cstdint>

#include "shm/cxl_type.h"
#include "cxl_config.h"
#include "shm/mm.h"
#ifdef USE_SPSC_QUEUE
#include "msg/spsc_queue.h"
#else
#include "msg/mpsc_queue.h"
#endif
#include "utils/sim_id.h"

namespace erpc {

/**
 * @brief CXL 共享内存块的元数据。
 *
 * 每个分配出来的 CXL 内存块都在 payload 前放置一个元数据头，
 * 用于维护引用计数、校验魔数和记录 owner rpc_id。
 */
struct cxl_block_metadata_t {
    std::atomic<uint32_t> ref_count;  // 引用计数
    uint32_t magic;                   // 魔数校验
    uint8_t owner_rpc_id;             // 分配者的 RPC ID
    uint8_t padding[7];               // 对齐到 16 字节

    static constexpr uint32_t kMagic = 0xC0112345;  // CXL magic number

    // 获取 payload 数据区指针。
    uint8_t* get_data() {
        return reinterpret_cast<uint8_t*>(this + 1);
    }

    // 从 payload 数据区指针反推元数据地址。
    static cxl_block_metadata_t* from_data(uint8_t* data) {
        return reinterpret_cast<cxl_block_metadata_t*>(data) - 1;
    }
};

static constexpr uint8_t kCXLQueueEntryRpc = 0;
static constexpr uint8_t kCXLQueueEntryFree = 1;

/**
 * @brief 指针传递队列的条目。
 *
 * 队列项携带 payload 指针和一份 eRPC 包头。这样接收方或转发方可以
 * 先读取队列项完成路由判断，而不必立即读取远端 payload。
 */
struct cxl_ptr_queue_entry_t {
    pkthdr_t pkthdr;        // 每跳使用的 eRPC packet header
    uint64_t data_ptr;      // 共享内存中数据的偏移量，相对于 base_addr
    uint8_t entry_type;     // kCXLQueueEntryRpc 或 kCXLQueueEntryFree
    uint8_t padding[7];
};

/**
 * @brief CXL 共享内存分配器。
 *
 * 这个分配器管理一段 CXL 共享内存区域，并提供类似 malloc/free 的接口。
 * 所有分配出的内存都位于共享内存中，可由不同 RPC endpoint 通过偏移量访问。
 *
 * 核心功能：
 * 1. 零拷贝：发送端分配并写入 payload 后，只通过队列传递指针。
 * 2. 引用计数：管理跨 endpoint 的共享内存生命周期。
 * 3. 队列隔离：每个 RPC endpoint 拥有独立的指针接收队列。
 */
class CXLSharedAllocator {
public:
    // 配置常量
    static constexpr size_t kMaxBlockSize = cxl_config::kMaxBlockSize;
    static constexpr size_t kPtrQueueSize = cxl_config::kPtrQueueSize;
    static constexpr size_t kSPSCQueueSize = cxl_config::kSPSCQueueSize;
    static constexpr size_t kMaxSessions = cxl_config::kMaxRpcEndpoints;

    /**
     * @brief 构造函数。
     * @param rpc_id 当前 RPC 的 ID。
     * @param memory_size CXL 共享内存映射大小。
     * @param numa_node NUMA 节点。
     */
    CXLSharedAllocator(uint8_t rpc_id, size_t memory_size, size_t numa_node);

    /**
     * @brief 析构函数。
     */
    ~CXLSharedAllocator();

    /**
     * @brief 在 CXL 共享内存中分配一块 payload。
     * @param size payload 大小，单位为字节。
     * @param initial_ref_count 初始引用计数，默认是 1。
     * @return 分配成功时返回有效 Buffer，否则返回空 Buffer。
     *
     * 内存布局为：[metadata][payload...]。
     */
    Buffer alloc(size_t size, uint32_t initial_ref_count = 1);

    /**
     * @brief 释放一块 CXL 共享内存。
     * @param buffer 要释放的 Buffer。
     */
    void free(Buffer buffer);

    /**
     * @brief 增加共享内存块的引用计数。
     * @param buffer 要增加引用的 Buffer。
     */
    void add_ref(Buffer buffer);

    /**
     * @brief 将数据指针放入指定 RPC endpoint 的指针队列。
     * @param session_id 目标 RPC endpoint ID。
     * @param data_ptr 共享内存中的 payload 指针。
     * @param entry_type 队列条目类型。
     * @param pkthdr 可选的 eRPC packet header。
     * @return 入队是否成功。
     */
    bool enqueue_ptr(uint8_t session_id, void* data_ptr,
                     uint8_t entry_type = kCXLQueueEntryRpc,
                     const pkthdr_t* pkthdr = nullptr);

    /**
     * @brief 从指定 RPC endpoint 的指针队列中取出一条记录。
     * @param session_id 当前接收方 RPC endpoint ID。
     * @param data_ptr 输出 payload 指针。
     * @param entry_type 输出队列条目类型。
     * @param pkthdr 输出 eRPC packet header。
     * @return 是否成功取出队列项。
     */
    bool dequeue_ptr(uint8_t session_id, void** data_ptr,
                     uint8_t* entry_type, pkthdr_t* pkthdr = nullptr);

#ifdef USE_SPSC_QUEUE
    /**
     * @brief Register a remote RPC endpoint whose SPSC queue should be polled.
     */
    void register_rx_peer(uint8_t peer_rpc_id);
#endif

    /**
     * @brief 将本地共享内存指针转换为相对共享内存 base 的偏移量。
     */
    uint64_t ptr_to_offset(void* ptr) const {
        if (ptr == nullptr) return 0;
        uint8_t* p = static_cast<uint8_t*>(ptr);
        uint8_t* base = static_cast<uint8_t*>(shm_base_addr_);
        assert(p >= base && p < base + memory_size_);
        return static_cast<uint64_t>(p - base);
    }

    /**
     * @brief 将共享内存偏移量转换为本进程中的本地指针。
     */
    void* offset_to_ptr(uint64_t offset) const {
        if (offset == 0) return nullptr;
        assert(offset < memory_size_);
        return static_cast<uint8_t*>(shm_base_addr_) + offset;
    }

    /**
     * @brief 检查指针是否落在当前 CXL 共享内存映射范围内。
     */
    bool is_shared_ptr(void* ptr) const {
        if (ptr == nullptr) return false;
        uint8_t* p = static_cast<uint8_t*>(ptr);
        uint8_t* base = static_cast<uint8_t*>(shm_base_addr_);
        return p >= base && p < base + memory_size_;
    }

    uint8_t get_owner_rpc_id(void* data_ptr) const {
        if (data_ptr == nullptr || !is_shared_ptr(data_ptr)) {
            return UINT8_MAX;
        }
        auto* metadata =
            cxl_block_metadata_t::from_data(static_cast<uint8_t*>(data_ptr));
        if (metadata->magic != cxl_block_metadata_t::kMagic) {
            return UINT8_MAX;
        }
        return metadata->owner_rpc_id;
    }

    /**
     * @brief 获取 NUMA 节点。
     */
    size_t get_numa_node() const { return numa_node_; }

    /**
     * @brief 获取 CXL 共享内存映射基地址。
     */
    void* get_base_addr() const { return shm_base_addr_; }

    /**
     * @brief 获取 CXL 共享内存映射大小。
     */
    size_t get_memory_size() const { return memory_size_; }

private:
    /**
     * @brief 初始化 CXL 共享内存映射。
     */
    void init_shared_memory();

    /**
     * @brief 初始化每个 RPC endpoint 对应的指针队列。
     */
    void init_ptr_queues();

    uint8_t rpc_id_;       // 当前 RPC ID
    size_t memory_size_;   // CXL 共享内存映射大小
    size_t numa_node_;     // NUMA 节点

    void* shm_base_addr_;  // CXL 共享内存映射基地址

#ifdef USE_SPSC_QUEUE
    SPSCQueue<cxl_ptr_queue_entry_t>* spsc_queues_[kMaxSessions][kMaxSessions];
    bool active_src_bitmap_[kMaxSessions];
    uint8_t active_srcs_[kMaxSessions];
    size_t active_src_count_;
    size_t next_poll_src_;
#else
    MPSCQueue<cxl_ptr_queue_entry_t>* ptr_queues_[kMaxSessions];
#endif
};

} // namespace erpc

#endif // ERPC_CXL
