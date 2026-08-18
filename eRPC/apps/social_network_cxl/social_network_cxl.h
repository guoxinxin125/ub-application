/**
 * @file social_network_cxl.h
 * @brief CXL-specific configuration and helpers for social_network
 */

#pragma once

#ifdef ERPC_CXL
#ifdef CACHE_LINE_SIZE
#undef CACHE_LINE_SIZE
#endif
#include "transport_impl/cxl/cxl_shared_allocator.h"
#include "transport_impl/cxl/cxl_transport.h"

namespace social_network_cxl {

erpc::CXLSharedAllocator* get_cxl_allocator(erpc::Rpc<erpc::CXLTransport>* rpc);

void* alloc_cxl_buffer(erpc::Rpc<erpc::CXLTransport>* rpc, size_t size);

void free_cxl_buffer(erpc::Rpc<erpc::CXLTransport>* rpc, void* ptr, size_t size);

void add_ref_cxl_buffer(erpc::Rpc<erpc::CXLTransport>* rpc, void* ptr);

bool is_shared_ptr(erpc::Rpc<erpc::CXLTransport>* rpc, void* ptr);

void print_cxl_info(erpc::Rpc<erpc::CXLTransport>* rpc);

}  // namespace social_network_cxl

#endif  // ERPC_CXL
