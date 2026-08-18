/**
 * @file social_network_cxl.cc
 * @brief CXL-specific implementations for social_network
 */

#ifdef ERPC_CXL
#include "social_network_cxl.h"
#include "rpc.h"

namespace social_network_cxl {

erpc::CXLSharedAllocator* get_cxl_allocator(erpc::Rpc<erpc::CXLTransport>* rpc) {
    if (rpc == nullptr) return nullptr;
    auto* transport = rpc->get_transport();
    if (transport == nullptr) return nullptr;
    return transport->get_shared_allocator();
}

void* alloc_cxl_buffer(erpc::Rpc<erpc::CXLTransport>* rpc, size_t size) {
    erpc::CXLSharedAllocator* allocator = get_cxl_allocator(rpc);
    if (allocator == nullptr) {
        printf("CRITICAL ERROR: get_cxl_allocator returned nullptr! rpc=%p, transport=%p\n", rpc, rpc ? rpc->get_transport() : nullptr);
        return nullptr;
    }
    erpc::Buffer buf = allocator->alloc(size, 1);
    return buf.buf_;
}

void free_cxl_buffer(erpc::Rpc<erpc::CXLTransport>* rpc, void* ptr, size_t size) {
    erpc::CXLSharedAllocator* allocator = get_cxl_allocator(rpc);
    if (allocator == nullptr || ptr == nullptr) return;
    erpc::Buffer buf(static_cast<uint8_t*>(ptr), size, size);
    allocator->free(buf);
}

void add_ref_cxl_buffer(erpc::Rpc<erpc::CXLTransport>* rpc, void* ptr) {
    erpc::CXLSharedAllocator* allocator = get_cxl_allocator(rpc);
    if (allocator == nullptr || ptr == nullptr) return;
    erpc::Buffer buf(static_cast<uint8_t*>(ptr), 0, 0);
    allocator->add_ref(buf);
}

bool is_shared_ptr(erpc::Rpc<erpc::CXLTransport>* rpc, void* ptr) {
    erpc::CXLSharedAllocator* allocator = get_cxl_allocator(rpc);
    if (allocator == nullptr) return false;
    return allocator->is_shared_ptr(ptr);
} 

void print_cxl_info(erpc::Rpc<erpc::CXLTransport>* rpc) {
    erpc::CXLSharedAllocator* allocator = get_cxl_allocator(rpc);
    if (allocator == nullptr) {
        // printf("[CXL] Allocator not available\n");
        return;
    }
    // // printf("[CXL] Base address: %p\n", allocator->get_base_addr());
    // // printf("[CXL] Memory size: %zu MB\n", allocator->get_memory_size() / (1024 * 1024));
    // // printf("[CXL] NUMA node: %zu\n", allocator->get_numa_node());
}

}  // namespace social_network_cxl
#endif  // ERPC_CXL