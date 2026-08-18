/**
 * @file cxl_transport.h
 * @brief CXL shared-memory transport implementation.
 */
#pragma once

#ifdef ERPC_CXL

#include "common.h"
#include "transport.h"
#include "cxl_config.h"
#include "cxl_shared_allocator.h"

#include <memory>

namespace erpc {

class CXLTransport : public Transport {
 public:
  static constexpr TransportType kTransportType = TransportType::kCXL;
  static constexpr size_t kMTU = 2048;
  static constexpr size_t kPostlist = 32;
  static constexpr size_t kUnsigBatch = 64;
  static constexpr size_t kMaxDataPerPkt = kMTU - sizeof(pkthdr_t);
  static constexpr size_t kNumRxRingEntries = Transport::kNumRxRingEntries;
  static constexpr size_t kCXLMemorySize =
      cxl_config::kDefaultCacheableMemorySize;
  static constexpr size_t kBandwidthGbps = cxl_config::kBandwidthGbps;

  CXLTransport(uint16_t sm_udp_port, uint8_t rpc_id, uint8_t phy_port,
               size_t numa_node, FILE *trace_file);
  ~CXLTransport();

  void init_hugepage_structures(HugeAlloc *huge_alloc, uint8_t **rx_ring);
  void fill_local_routing_info(routing_info_t *routing_info) const;
  bool resolve_remote_routing_info(routing_info_t *routing_info) const;
  size_t get_bandwidth() const;
  static std::string routing_info_str(routing_info_t *routing_info);

  void tx_burst(const tx_burst_item_t *tx_burst_arr, size_t num_pkts);
  void tx_flush();
  size_t rx_burst();
  void post_recvs(size_t num_recvs);

  Buffer alloc_shared_buffer(size_t size);
  void free_shared_buffer(Buffer buffer);
  void register_rx_peer(uint8_t peer_rpc_id);

  CXLSharedAllocator *get_shared_allocator() { return shared_allocator_.get(); }

  bool is_in_shared_memory(void *ptr) const {
    return shared_allocator_->is_shared_ptr(ptr);
  }

  uint8_t *get_rx_payload(const pkthdr_t *pkthdr) const {
    const auto p = reinterpret_cast<uintptr_t>(pkthdr);
    const auto begin = reinterpret_cast<uintptr_t>(rx_pkthdr_ring_);
    const auto end =
        reinterpret_cast<uintptr_t>(rx_pkthdr_ring_ + kNumRxRingEntries);
    if (p < begin || p >= end) {
      return nullptr;
    }
    return rx_payload_ring_[(p - begin) / sizeof(pkthdr_t)];
  }

 private:
  uint8_t **rx_ring_;
  pkthdr_t rx_pkthdr_ring_[kNumRxRingEntries];
  uint8_t *rx_payload_ring_[kNumRxRingEntries];
  size_t rx_ring_head_;

  std::unique_ptr<CXLSharedAllocator> shared_allocator_;
};

}  // namespace erpc

#endif  // ERPC_CXL
