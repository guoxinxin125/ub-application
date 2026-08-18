/**
 * @file cxl_shared_allocator.cc
 * @brief CXL shared memory allocator implementation
 */
#ifdef ERPC_CXL
#include "cxl_shared_allocator.h"
#include "util/logger.h"
#include "utils/thread_local_context.h"
#include <cstring>

namespace erpc {

MemoryManager* erpc_global_cacheable_ptrs[256] = {nullptr};

CXLSharedAllocator::CXLSharedAllocator(uint8_t rpc_id, size_t memory_size,
                                       size_t numa_node)
    : rpc_id_(rpc_id),
      memory_size_(memory_size),
      numa_node_(numa_node),
      shm_base_addr_(nullptr)
#ifdef USE_SPSC_QUEUE
      , next_poll_src_(0)
#endif
{

    const size_t worker_count = cxl_config::get_worker_count();
    rt_assert(worker_count > rpc_id_,
              "ERPC_CXL_WORKER_COUNT must be greater than rpc_id");
    rt_assert(worker_count <= cxl_config::kMaxRpcEndpoints,
              "ERPC_CXL_WORKER_COUNT exceeds max CXL RPC endpoints");
    SimThreadInfo::worker_machine_count = static_cast<uint32_t>(worker_count);
    SimThreadInfo::worker_machine_id = rpc_id;
#ifdef USE_SPSC_QUEUE
    memset(active_src_bitmap_, 0, sizeof(active_src_bitmap_));
    active_src_count_ = 0;
    next_poll_src_ = 0;
#endif

    init_shared_memory();
    init_ptr_queues();
}

CXLSharedAllocator::~CXLSharedAllocator() {
#ifdef USE_SPSC_QUEUE
    for (size_t dst = 0; dst < kMaxSessions; dst++) {
        for (size_t src = 0; src < kMaxSessions; src++) {
            spsc_queues_[dst][src] = nullptr;
        }
    }
#else
    for (size_t i = 0; i < kMaxSessions; i++) {
        ptr_queues_[i] = nullptr;
    }
#endif

    shm_base_addr_ = nullptr;
}

void CXLSharedAllocator::init_shared_memory() {
    const char* device_path = cxl_config::kCacheableDevicePath;

    void* fixed_base =
        reinterpret_cast<void*>(cxl_config::kCacheableBaseAddr);
    init_cxl_erpc_cacheable_allocator(device_path, fixed_base, memory_size_);

#if defined(ENABLE_UNCACHE_MEM)
    const char* uncache_device_path = cxl_config::kUncacheableDevicePath;
    void* fixed_uncache_base =
        reinterpret_cast<void*>(cxl_config::kUncacheableBaseAddr);
    init_uncacheable_allocator(uncache_device_path, fixed_uncache_base,
                               cxl_config::kUncacheableMemorySize);
#endif

    // Register the thread_local cacheable pointer for this rpc_id_ so worker threads can share it
    extern MemoryManager* erpc_global_cacheable_ptrs[256];
    erpc_global_cacheable_ptrs[rpc_id_] = &cacheable;

    shm_base_addr_ = cacheable.allocator->mmap_base;
    rt_assert(shm_base_addr_ != nullptr, "Failed to get CXL memory");

    memory_size_ = cacheable.allocator->mmap_size;
    // printf("[CXLSharedAllocator %u] mmap_base=%p, size=%zu\n",
    //        rpc_id_, shm_base_addr_, memory_size_);
    // fflush(stdout);
}

void CXLSharedAllocator::init_ptr_queues() {
    // 为每个 RPC endpoint 分配固定大小的指针队列空间。
#if defined(USE_SPSC_QUEUE)
#if defined(ENABLE_UNCACHE_MEM)
    void* reserved_base = uncacheable.allocator->get_mmap_base();
#else
    void* reserved_base = shm_base_addr_;
#endif

    constexpr size_t kPairSpace = cxl_config::kSPSCQueuePairSpace;
    constexpr size_t kBufferOffset = cxl_config::kSPSCQueueBufferOffset;
    constexpr size_t kRequiredBytes =
        kMaxSessions * kMaxSessions * kPairSpace;
    static_assert(kBufferOffset >= sizeof(SPSCQueue<cxl_ptr_queue_entry_t>),
                  "SPSC queue buffer overlaps queue object");
    static_assert(kBufferOffset +
                      kSPSCQueueSize *
                          sizeof(SPSCQueue<cxl_ptr_queue_entry_t>::T_wrapper) <=
                  kPairSpace,
                  "SPSC queue pair space is too small");
    static_assert(kRequiredBytes <= cxl_config::kReservedQueueBytes,
                  "SPSC queue matrix exceeds reserved CXL queue region");

    for (size_t dst = 0; dst < kMaxSessions; dst++) {
        for (size_t src = 0; src < kMaxSessions; src++) {
            const size_t pair_index = dst * kMaxSessions + src;
            auto* pair_base = reinterpret_cast<uint8_t*>(reserved_base) +
                              pair_index * kPairSpace;
            void* queue_base = pair_base;
            void* queue_buffer_base = pair_base + kBufferOffset;

            spsc_queues_[dst][src] =
                reinterpret_cast<SPSCQueue<cxl_ptr_queue_entry_t>*>(
                    queue_base);

            if (dst == rpc_id_) {
                new (queue_base) SPSCQueue<cxl_ptr_queue_entry_t>(
                    kSPSCQueueSize, queue_buffer_base);
            }
        }
    }
#else
#if defined(ENABLE_UNCACHE_MEM)
    constexpr size_t kSessionSpace = cxl_config::kUncacheableSessionSpace;
#if defined(USE_NO_CC_QUEUE)
    constexpr size_t kQueueBufferOffset =
        cxl_config::kUncacheableQueueBufferOffset;
#endif
#else
    constexpr size_t kSessionSpace = cxl_config::kCacheableQueueSessionSpace;
#endif

    // pccshm-sdk 中保留了 16MB 用于全局指针队列。
    // 这里需要保证保留区域大小 >= kMaxSessions * kSessionSpace。
#if defined(ENABLE_UNCACHE_MEM)
    void* reserved_base = uncacheable.allocator->get_mmap_base();
#else
    void* reserved_base = shm_base_addr_;
#endif

    for (size_t i = 0; i < kMaxSessions; i++) {
        void* session_base = (void*)((uintptr_t)reserved_base + i * kSessionSpace);
        void* queue_base = session_base;
#if defined(USE_NO_CC_QUEUE) && defined(ENABLE_UNCACHE_MEM)
        void* queue_buffer_base =
            (void*)((uintptr_t)session_base + kQueueBufferOffset);
#endif

        ptr_queues_[i] = reinterpret_cast<MPSCQueue<cxl_ptr_queue_entry_t>*>(
            queue_base);

        if (i == rpc_id_) {
#if defined(USE_NO_CC_QUEUE) && defined(ENABLE_UNCACHE_MEM)
            new (queue_base) MPSCQueue<cxl_ptr_queue_entry_t>(
                kPtrQueueSize, queue_buffer_base);
#else
            new (queue_base) MPSCQueue<cxl_ptr_queue_entry_t>(kPtrQueueSize);
#endif
            // printf("Init queue %zu, queue=%p\n", i, ptr_queues_[i]); fflush(stdout);
        }
    }
#endif
}

Buffer CXLSharedAllocator::alloc(size_t size, uint32_t initial_ref_count) {
    if (size == 0 || size > kMaxBlockSize) {
        ERPC_WARN("CXLSharedAllocator: Invalid allocation size %zu\n", size);
        return Buffer(nullptr, 0, 0);
    }

    size_t total_size = sizeof(cxl_block_metadata_t) + size;

    void* ptr = cacheable.malloc(total_size);

    if (ptr == nullptr) {
        ERPC_WARN("CXLSharedAllocator: Out of memory, requested %zu bytes\n", size);
        return Buffer(nullptr, 0, 0);
    }

    auto* metadata = new (ptr) cxl_block_metadata_t();
    metadata->ref_count.store(initial_ref_count, std::memory_order_relaxed);
    metadata->magic = cxl_block_metadata_t::kMagic;
    metadata->owner_rpc_id = rpc_id_;

    Buffer buffer;
    buffer.buf_ = metadata->get_data();
    buffer.class_size_ = total_size;

    return buffer;
}

void CXLSharedAllocator::free(Buffer buffer) {
    if (buffer.buf_ == nullptr) return;

    auto* metadata = cxl_block_metadata_t::from_data(buffer.buf_);

    if (metadata->magic != cxl_block_metadata_t::kMagic) {
        ERPC_ERROR("CXLSharedAllocator: Invalid magic number in free(): magic=0x%x, expected=0x%x\n",
                   metadata->magic, cxl_block_metadata_t::kMagic);
        return;
    }

    uint32_t ref = metadata->ref_count.fetch_sub(1, std::memory_order_acq_rel);

    if (ref == 1) {
        if (metadata->owner_rpc_id == rpc_id_) {
            cacheable.free(metadata);
        } else {
            // Cross-process free: send back to owner
            enqueue_ptr(metadata->owner_rpc_id, metadata->get_data(),
                        kCXLQueueEntryFree);
        }
    }
}

void CXLSharedAllocator::add_ref(Buffer buffer) {
    if (buffer.buf_ == nullptr) return;

    auto* metadata = cxl_block_metadata_t::from_data(buffer.buf_);

    if (metadata->magic != cxl_block_metadata_t::kMagic) {
        ERPC_ERROR("CXLSharedAllocator: Invalid magic number in add_ref()\n");
        return;
    }

    // 增加引用计数
    uint32_t old_ref = metadata->ref_count.fetch_add(1, std::memory_order_relaxed);

    // ERPC_INFO("CXLSharedAllocator: add_ref() called, ref_count %u -> %u\n",
    //           old_ref, old_ref + 1);
    _unused(old_ref);
}

bool CXLSharedAllocator::enqueue_ptr(uint8_t session_id, void* data_ptr,
                                     uint8_t entry_type,
                                     const pkthdr_t* pkthdr) {
    if (session_id >= kMaxSessions) {
        ERPC_WARN("CXLSharedAllocator: Invalid session_id %u (max: %zu)\n",
                  session_id, kMaxSessions);
        return false;
    }
    // printf("CXLSharedAllocator: Enqueueing pointer to session %u, data_ptr=%p, entry_type=%u\n",
    //        session_id, data_ptr, entry_type);

    cxl_ptr_queue_entry_t entry{};
    entry.data_ptr = ptr_to_offset(data_ptr);
    entry.entry_type = entry_type;
    if (pkthdr != nullptr) {
        entry.pkthdr = *pkthdr;
    } else {
        memset(&entry.pkthdr, 0, sizeof(entry.pkthdr));
    }

#ifdef USE_SPSC_QUEUE
    if (spsc_queues_[session_id][rpc_id_] == nullptr) {
        ERPC_ERROR("CXLSharedAllocator: null SPSC queue in enqueue_ptr "
                   "(dst=%u, src=%u, rpc_id=%u)\n",
                   session_id, rpc_id_, rpc_id_);
        rt_assert(false, "CXLSharedAllocator: null SPSC queue in enqueue_ptr");
    }
    no_cc_context::ScopedOwnerMachineId owner_scope(session_id);
    bool success = spsc_queues_[session_id][rpc_id_]->enqueue(
        std::move(entry));
    return success;
#else
    if (ptr_queues_[session_id] == nullptr) {
        // printf("\[ENQUEUE\] FATAL: queue %u is NULL!\n", session_id);
        // fflush(stdout);
        ERPC_ERROR("CXLSharedAllocator: null MPSC queue in enqueue_ptr "
                   "(dst=%u, rpc_id=%u)\n",
                   session_id, rpc_id_);
        rt_assert(false, "CXLSharedAllocator: null MPSC queue in enqueue_ptr");
    }
    no_cc_context::ScopedOwnerMachineId owner_scope(session_id);
    bool success = ptr_queues_[session_id]->enqueue(std::move(entry));
    return success;
#endif
}

#ifdef USE_SPSC_QUEUE
void CXLSharedAllocator::register_rx_peer(uint8_t peer_rpc_id) {
    if (peer_rpc_id >= kMaxSessions || peer_rpc_id == rpc_id_) {
        return;
    }

    if (active_src_bitmap_[peer_rpc_id]) {
        return;
    }

    active_src_bitmap_[peer_rpc_id] = true;
    active_srcs_[active_src_count_++] = peer_rpc_id;
}
#endif

bool CXLSharedAllocator::dequeue_ptr(uint8_t session_id, void** data_ptr,
                                     uint8_t* entry_type,
                                     pkthdr_t* pkthdr) {
    if (session_id >= kMaxSessions) {
        ERPC_WARN("CXLSharedAllocator: Invalid session_id %u\n", session_id);
        return false;
    }
    // printf("CXLSharedAllocator: Dequeueing pointer from session %u\n", session_id);

#ifdef USE_SPSC_QUEUE
    const size_t worker_count = cxl_config::get_worker_count();
    rt_assert(worker_count <= kMaxSessions,
              "ERPC_CXL_WORKER_COUNT exceeds max CXL RPC endpoints");
    no_cc_context::ScopedOwnerMachineId owner_scope(session_id);
    const size_t poll_count =
        active_src_count_ > 0 ? active_src_count_ : worker_count;
    for (size_t n = 0; n < poll_count; n++) {
        const size_t poll_idx = (next_poll_src_ + n) % poll_count;
        const size_t src =
            active_src_count_ > 0 ? active_srcs_[poll_idx] : poll_idx;
        if (src == rpc_id_) {
            continue;
        }
        auto* queue = spsc_queues_[session_id][src];
        if (queue == nullptr) {
            ERPC_ERROR("CXLSharedAllocator: null SPSC queue in dequeue_ptr "
                       "(dst=%u, src=%zu, rpc_id=%u, worker_count=%zu)\n",
                       session_id, src, rpc_id_, worker_count);
            rt_assert(false,
                      "CXLSharedAllocator: null SPSC queue in dequeue_ptr");
        }

        cxl_ptr_queue_entry_t entry;
        if (!queue->try_dequeue(entry)) {
            continue;
        }

        next_poll_src_ = (poll_idx + 1) % poll_count;
        *data_ptr = offset_to_ptr(entry.data_ptr);
        *entry_type = entry.entry_type;
        if (pkthdr != nullptr) {
            *pkthdr = entry.pkthdr;
        }
        return true;
    }
    return false;
#else
    cxl_ptr_queue_entry_t entry;
    if (ptr_queues_[session_id] == nullptr) {
        // printf("\[DEQUEUE\] FATAL: queue %u is NULL!\n", session_id);
        // fflush(stdout);
        ERPC_ERROR("CXLSharedAllocator: null MPSC queue in dequeue_ptr "
                   "(session_id=%u, rpc_id=%u)\n",
                   session_id, rpc_id_);
        rt_assert(false, "CXLSharedAllocator: null MPSC queue in dequeue_ptr");
    }
    no_cc_context::ScopedOwnerMachineId owner_scope(session_id);
    bool success = ptr_queues_[session_id]->try_dequeue(entry);

    if (success) {
        *data_ptr = offset_to_ptr(entry.data_ptr);
        *entry_type = entry.entry_type;
        if (pkthdr != nullptr) {
            *pkthdr = entry.pkthdr;
        }
    }
    return success;
#endif
}

} // namespace erpc

#endif // ERPC_CXL
