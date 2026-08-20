/**
 * @file ub_transport.h
 * @brief UBSM-backed eRPC transport using machine-level endpoint inboxes.
 */
#pragma once

#ifdef ERPC_UB

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <string>

#include "transport.h"
#include "transport_impl/ub/ub_config.h"
#include "transport_impl/ub/ub_machine.h"
#include "transport_impl/ub/ub_shared_allocator.h"

namespace erpc {

struct UBRoutingInfo {
  uint32_t magic;
  uint16_t version;
  uint8_t rpc_id;
  uint8_t endpoint_slot;
  uint32_t endpoint_generation;
  uint32_t reserved;
  uint64_t machine_id;
  uint64_t inbox_offset;
  uint64_t arena_offset;
  uint64_t arena_size;
};

static_assert(sizeof(UBRoutingInfo) <= Transport::kMaxRoutingInfoSize,
              "UB routing info exceeds eRPC routing-info storage");

class UBTransport : public Transport {
 public:
  static constexpr TransportType kTransportType = TransportType::kUB;
  static constexpr size_t kMTU = ub_config::kMTU;
  static constexpr size_t kPostlist = 32;
  static constexpr size_t kUnsigBatch = 64;
  static constexpr size_t kMaxDataPerPkt = kMTU - sizeof(pkthdr_t);
  static constexpr size_t kNumRxRingEntries = Transport::kNumRxRingEntries;
  static constexpr size_t kBandwidthGbps = ub_config::kBandwidthGbps;
  static constexpr size_t kMaxPendingTxPerEndpoint = ub_config::kSpscQueueDepth;

  UBTransport(uint16_t sm_udp_port, uint8_t rpc_id, uint8_t phy_port,
              size_t numa_node, FILE *trace_file);
  ~UBTransport();

  void init_hugepage_structures(HugeAlloc *huge_alloc, uint8_t **rx_ring);
  void fill_local_routing_info(routing_info_t *routing_info) const;
  bool resolve_remote_routing_info(routing_info_t *routing_info) const;
  size_t get_bandwidth() const;
  static std::string routing_info_str(routing_info_t *routing_info);

  void tx_burst(const tx_burst_item_t *tx_burst_arr, size_t num_pkts);
  void tx_flush();
  size_t rx_burst();
  void post_recvs(size_t num_recvs);
  void register_rx_peer(uint8_t peer_rpc_id);
  void prepare_disconnect(uint8_t peer_rpc_id);
  void unregister_rx_peer(uint8_t peer_rpc_id);

  Buffer alloc_shared_buffer(size_t size);
  void free_shared_buffer(Buffer buffer);
  bool is_in_shared_memory(void *ptr) const {
    return shared_allocator_->is_shared_ptr(ptr);
  }
  uint8_t *get_rx_payload(const pkthdr_t *pkthdr) const;

 private:
  struct ProfileCounter {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> ticks{0};

    void record(size_t elapsed_ticks) {
      calls.fetch_add(1, std::memory_order_relaxed);
      ticks.fetch_add(static_cast<uint64_t>(elapsed_ticks),
                      std::memory_order_relaxed);
    }
  };

  struct ProfileStats {
    ProfileCounter alloc;
    ProfileCounter free;
    ProfileCounter endpoint_resolve;
    ProfileCounter add_ref;
    ProfileCounter tx_queue;
    ProfileCounter tx_burst;
    ProfileCounter rx_queue_empty;
    ProfileCounter rx_queue_hit;
    ProfileCounter rx_resolve_bounds;
    ProfileCounter rx_resolve_state;
    ProfileCounter rx_resolve_metadata;
    ProfileCounter rx_resolve_checks;
    ProfileCounter rx_resolve;
    ProfileCounter rx_burst;
  };

  struct PendingTx {
    UBMessageDescriptor descriptor;
    Buffer backing_buffer;

    PendingTx(const UBMessageDescriptor &message_descriptor,
              Buffer message_buffer)
        : descriptor(message_descriptor), backing_buffer(message_buffer) {}
  };

  struct RemoteEndpoint {
    void *machine_base = nullptr;
    UBEndpointHandle endpoint;
    UBEndpointInbox *inbox = nullptr;
    uint64_t machine_id = 0;
    uint64_t producer_tail = 0;
    uint64_t cached_consumer_head = 0;
    std::deque<PendingTx> pending_tx;
  };

  bool ensure_remote_endpoint(const UBRoutingInfo &route) const;
  void drain_pending(RemoteEndpoint &remote);
  void drain_all_pending();
  void discard_descriptor_noexcept(
      void *machine_base, const UBMessageDescriptor &descriptor) noexcept;
  void release_pending_noexcept(RemoteEndpoint &remote) noexcept;
  void release_remote_endpoint_noexcept(uint8_t peer_rpc_id) noexcept;
  void cleanup_noexcept() noexcept;
  size_t profile_start() const;
  void profile_record(ProfileCounter &counter, size_t start_ticks) const;
  void print_profile() const;

  uint8_t **rx_ring_;
  pkthdr_t rx_pkthdr_ring_[kNumRxRingEntries];
  uint8_t *rx_payload_ring_[kNumRxRingEntries];
  size_t rx_ring_head_;
  size_t next_poll_active_index_;
  size_t active_rx_source_count_;

  UBMachineContext *machine_context_;
  UBEndpointHandle local_endpoint_;
  UBEndpointInbox *local_inbox_;
  std::unique_ptr<UBSharedAllocator> shared_allocator_;

  mutable std::array<RemoteEndpoint, ub_config::kMaxRpcEndpoints>
      remote_endpoints_;
  std::array<uint64_t, ub_config::kMaxRpcEndpoints> consumer_heads_;
  std::array<uint16_t, ub_config::kMaxRpcEndpoints> rx_peer_refcounts_;
  std::array<uint8_t, ub_config::kMaxRpcEndpoints> active_rx_sources_;
  bool profile_enabled_;
  double profile_freq_ghz_;
  size_t profile_timestamp_overhead_ticks_;
  mutable ProfileStats profile_stats_;
};

}  // namespace erpc

#endif  // ERPC_UB
