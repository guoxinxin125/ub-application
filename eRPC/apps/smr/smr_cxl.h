/**
 * @file smr_cxl.h
 * @brief CXL-specific configuration for SMR
 */

#pragma once

// CXL 零拷贝优化开关
#ifdef ERPC_CXL
static constexpr bool kUseCXLZeroCopy = true;
#else
static constexpr bool kUseCXLZeroCopy = false;
#endif

// 使用 eRPC 的 CTransport（由 config.h 定义）
#define SMR_TRANSPORT erpc::CTransport

#ifdef ERPC_CXL
#include "transport_impl/cxl/cxl_transport.h"
#include "transport_impl/cxl/cxl_shared_allocator.h"

// CXL 零拷贝相关的辅助函数
namespace smr_cxl {

// 从共享内存分配 entry 数据缓冲区
inline void* alloc_entry_data(erpc::Rpc<SMR_TRANSPORT>* rpc, size_t size) {
    auto* allocator = rpc->get_transport()->get_shared_allocator();
    if (allocator) {
        erpc::Buffer buf = allocator->alloc(size, 1);
        return buf.buf_;
    }
    return nullptr;
}

// 释放共享内存中的 entry 数据缓冲区（只有 Leader 调用）
inline void free_entry_data(erpc::Rpc<SMR_TRANSPORT>* rpc, void* ptr, size_t size) {
    auto* allocator = rpc->get_transport()->get_shared_allocator();
    if (allocator && ptr) {
        erpc::Buffer buf(static_cast<uint8_t*>(ptr), size, size);
        allocator->free(buf);
    }
}

// 检查指针是否在共享内存中
inline bool is_shared_ptr(erpc::Rpc<SMR_TRANSPORT>* rpc, void* ptr) {
    auto* transport = dynamic_cast<erpc::CXLTransport*>(rpc->get_transport());
    if (transport) {
        return transport->is_in_shared_memory(ptr);
    }
    return false;
}

inline uint64_t ptr_to_offset(erpc::Rpc<SMR_TRANSPORT>* rpc, void* ptr) {
    auto* allocator = rpc->get_transport()->get_shared_allocator();
    if (allocator && ptr) {
        return allocator->ptr_to_offset(ptr);
    }
    return 0;
}

inline void* offset_to_ptr(erpc::Rpc<SMR_TRANSPORT>* rpc, uint64_t offset) {
    auto* allocator = rpc->get_transport()->get_shared_allocator();
    if (allocator && offset != 0) {
        return allocator->offset_to_ptr(offset);
    }
    return nullptr;
}

}  // namespace smr_cxl

#endif  // ERPC_CXL