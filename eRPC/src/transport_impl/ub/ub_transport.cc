#ifdef ERPC_UB

#include "transport_impl/ub/ub_transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "util/huge_alloc.h"
#include "util/logger.h"
#include "util/timer.h"

namespace erpc {
namespace {

UBRoutingInfo decode_routing_info(
    const Transport::routing_info_t *routing_info) {
  UBRoutingInfo decoded{};
  std::memcpy(&decoded, routing_info->buf_, sizeof(decoded));
  return decoded;
}

UBEndpointHandle endpoint_from_route(const UBRoutingInfo &route) {
  return UBEndpointHandle(route.endpoint_slot, route.rpc_id,
                          route.endpoint_generation, route.inbox_offset,
                          route.arena_offset, route.arena_size);
}

bool profile_enabled_from_env() {
  const char *value = std::getenv("ERPC_UB_PROFILE");
  return value != nullptr && std::strcmp(value, "0") != 0 &&
         std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0;
}

}  // namespace

constexpr TransportType UBTransport::kTransportType;
constexpr size_t UBTransport::kMTU;
constexpr size_t UBTransport::kPostlist;
constexpr size_t UBTransport::kUnsigBatch;
constexpr size_t UBTransport::kMaxDataPerPkt;
constexpr size_t UBTransport::kNumRxRingEntries;
constexpr size_t UBTransport::kBandwidthGbps;
constexpr size_t UBTransport::kMaxPendingTxPerEndpoint;

UBTransport::UBTransport(uint16_t sm_udp_port, uint8_t rpc_id, uint8_t phy_port,
                         size_t numa_node, FILE *trace_file)
    : Transport(kTransportType, rpc_id, phy_port, numa_node, trace_file),
      rx_ring_(nullptr),
      rx_ring_head_(0),
      next_poll_active_index_(0),
      active_rx_source_count_(0),
      machine_context_(nullptr),
      local_inbox_(nullptr),
      profile_enabled_(profile_enabled_from_env()),
      profile_freq_ghz_(0.0),
      profile_timestamp_overhead_ticks_(0) {
  rt_assert(rpc_id < ub_config::kMaxRpcEndpoints,
            "UBTransport rpc_id exceeds SPSC queue matrix");
  consumer_heads_.fill(0);
  rx_peer_refcounts_.fill(0);
  active_rx_sources_.fill(UINT8_MAX);

  if (profile_enabled_) {
    profile_freq_ghz_ = measure_rdtsc_freq();
    profile_timestamp_overhead_ticks_ = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < 1000; ++i) {
      const size_t start = rdtsc();
      const size_t elapsed = rdtsc() - start;
      if (elapsed < profile_timestamp_overhead_ticks_) {
        profile_timestamp_overhead_ticks_ = elapsed;
      }
    }
  }

  try {
    machine_context_ = UBMachineContext::acquire(numa_node);
    local_endpoint_ = machine_context_->register_endpoint(sm_udp_port, rpc_id);
    local_inbox_ = ub_inbox_at(machine_context_->local_base(),
                               local_endpoint_.inbox_offset);
    shared_allocator_.reset(
        new UBSharedAllocator(machine_context_, local_endpoint_));
  } catch (...) {
    cleanup_noexcept();
    throw;
  }
}

UBTransport::~UBTransport() {
  print_profile();
  cleanup_noexcept();
}

size_t UBTransport::profile_start() const {
  return profile_enabled_ ? rdtsc() : 0;
}

void UBTransport::profile_record(ProfileCounter &counter,
                                 size_t start_ticks) const {
  if (!profile_enabled_) return;
  counter.record(rdtsc() - start_ticks);
}

void UBTransport::print_profile() const {
  if (!profile_enabled_) return;
  std::fprintf(stderr, "UB_PROFILE rpc_id=%u timestamp_overhead_ns=%.1f\n",
               rpc_id_,
               to_nsec(profile_timestamp_overhead_ticks_, profile_freq_ghz_));
  const auto print_counter = [this](const char *name,
                                    const ProfileCounter &counter) {
    const uint64_t calls = counter.calls.load(std::memory_order_relaxed);
    const uint64_t ticks = counter.ticks.load(std::memory_order_relaxed);
    if (calls == 0) return;
    const double average_ticks =
        static_cast<double>(ticks) / static_cast<double>(calls);
    const double adjusted_ticks =
        average_ticks > static_cast<double>(profile_timestamp_overhead_ticks_)
            ? average_ticks -
                  static_cast<double>(profile_timestamp_overhead_ticks_)
            : 0.0;
    std::fprintf(stderr,
                 "UB_PROFILE rpc_id=%u stage=%s calls=%llu avg_ns=%.1f "
                 "adjusted_avg_ns=%.1f\n",
                 rpc_id_, name, static_cast<unsigned long long>(calls),
                 average_ticks / profile_freq_ghz_,
                 adjusted_ticks / profile_freq_ghz_);
  };
  print_counter("alloc", profile_stats_.alloc);
  print_counter("free", profile_stats_.free);
  print_counter("endpoint_lookup", profile_stats_.endpoint_resolve);
  print_counter("add_ref", profile_stats_.add_ref);
  print_counter("tx_queue", profile_stats_.tx_queue);
  print_counter("tx_burst", profile_stats_.tx_burst);
  print_counter("rx_queue_empty", profile_stats_.rx_queue_empty);
  print_counter("rx_queue_hit", profile_stats_.rx_queue_hit);
  print_counter("rx_resolve", profile_stats_.rx_resolve);
  print_counter("rx_burst", profile_stats_.rx_burst);
}

Buffer UBTransport::alloc_shared_buffer(size_t size) {
  const size_t start = profile_start();
  const Buffer buffer = shared_allocator_->alloc(size);
  profile_record(profile_stats_.alloc, start);
  return buffer;
}

void UBTransport::free_shared_buffer(Buffer buffer) {
  const size_t start = profile_start();
  shared_allocator_->free(buffer);
  profile_record(profile_stats_.free, start);
}

void UBTransport::cleanup_noexcept() noexcept {
  // Give already accepted eRPC sends one final chance to enter their shared
  // SPSC queues. Any descriptor that is still pending afterwards was never
  // published, so its in-flight allocator reference is still ours to release.
  drain_all_pending();
  for (RemoteEndpoint &remote : remote_endpoints_) {
    release_pending_noexcept(remote);
  }
  remote_endpoints_ = {};
  shared_allocator_.reset();
  local_inbox_ = nullptr;
  if (machine_context_ != nullptr) {
    machine_context_->unregister_endpoint(local_endpoint_);
    local_endpoint_ = UBEndpointHandle{};
    UBMachineContext::release(machine_context_);
    machine_context_ = nullptr;
  }
}

void UBTransport::init_hugepage_structures(HugeAlloc *huge_alloc,
                                           uint8_t **rx_ring) {
  _unused(huge_alloc);
  rt_assert(rx_ring != nullptr, "UBTransport RX ring cannot be null");
  rx_ring_ = rx_ring;
  for (size_t i = 0; i < kNumRxRingEntries; ++i) {
    rx_ring_[i] = reinterpret_cast<uint8_t *>(&rx_pkthdr_ring_[i]);
    rx_payload_ring_[i] = nullptr;
  }

  reg_mr_func_ = [](void *address, size_t /*size*/) {
    return Transport::mem_reg_info(address, 0);
  };
  dereg_mr_func_ = [](Transport::mem_reg_info info) { _unused(info); };
}

void UBTransport::fill_local_routing_info(routing_info_t *routing_info) const {
  rt_assert(routing_info != nullptr, "UB routing info cannot be null");
  std::memset(routing_info->buf_, 0, sizeof(routing_info->buf_));
  UBRoutingInfo info{};
  info.magic = ub_config::kRegionMagic;
  info.version = ub_config::kRegionVersion;
  info.rpc_id = rpc_id_;
  info.endpoint_slot = local_endpoint_.slot;
  info.endpoint_generation = local_endpoint_.generation;
  info.machine_id = machine_context_->local_machine_id();
  info.inbox_offset = local_endpoint_.inbox_offset;
  info.arena_offset = local_endpoint_.arena_offset;
  info.arena_size = local_endpoint_.arena_size;
  std::memcpy(routing_info->buf_, &info, sizeof(info));
}

bool UBTransport::ensure_remote_endpoint(const UBRoutingInfo &route) const {
  if (route.magic != ub_config::kRegionMagic ||
      route.version != ub_config::kRegionVersion ||
      route.rpc_id >= ub_config::kMaxRpcEndpoints) {
    return false;
  }

  const UBEndpointHandle endpoint = endpoint_from_route(route);
  RemoteEndpoint &remote = remote_endpoints_[route.rpc_id];
  // Session setup validates the registry entry before caching this endpoint.
  // Keep the steady-state send path local; a different route generation or
  // layout falls through to validation and refresh below.
  if (remote.inbox != nullptr && remote.machine_id == route.machine_id &&
      remote.endpoint.rpc_id == endpoint.rpc_id &&
      remote.endpoint.generation == endpoint.generation &&
      remote.endpoint.slot == endpoint.slot &&
      remote.endpoint.inbox_offset == endpoint.inbox_offset &&
      remote.endpoint.arena_offset == endpoint.arena_offset &&
      remote.endpoint.arena_size == endpoint.arena_size) {
    return true;
  }

  // A route generation change denotes a different endpoint incarnation. Do
  // not move descriptors queued for the old incarnation to the new inbox.
  if (!remote.pending_tx.empty()) return false;

  void *machine_base = machine_context_->map_remote_machine(route.machine_id);
  if (machine_base == nullptr ||
      !machine_context_->validate_endpoint(machine_base, route.machine_id,
                                           endpoint)) {
    return false;
  }

  remote.machine_base = machine_base;
  remote.endpoint = endpoint;
  remote.inbox = ub_inbox_at(machine_base, endpoint.inbox_offset);
  remote.machine_id = route.machine_id;
  remote.producer_tail =
      ub_atomic::load(&remote.inbox->queues[rpc_id_].tail.value,
                      ub_atomic::MemoryOrder::kAcquire);
  remote.cached_consumer_head =
      ub_atomic::load(&remote.inbox->queues[rpc_id_].head.value,
                      ub_atomic::MemoryOrder::kAcquire);
  return true;
}

void UBTransport::drain_pending(RemoteEndpoint &remote) {
  if (remote.inbox == nullptr || remote.pending_tx.empty()) return;
  if (!machine_context_->validate_endpoint(
          remote.machine_base, remote.machine_id, remote.endpoint)) {
    release_pending_noexcept(remote);
    remote.inbox = nullptr;
    return;
  }
  UBSpscQueue &queue = remote.inbox->queues[rpc_id_];
  while (!remote.pending_tx.empty()) {
    const PendingTx &pending = remote.pending_tx.front();
    if (!queue.try_enqueue(remote.producer_tail, remote.cached_consumer_head,
                           pending.descriptor))
      return;
    remote.pending_tx.pop_front();
  }
}

void UBTransport::drain_all_pending() {
  for (RemoteEndpoint &remote : remote_endpoints_) drain_pending(remote);
}

void UBTransport::discard_descriptor_noexcept(
    const UBMessageDescriptor &descriptor) noexcept {
  if (shared_allocator_ == nullptr || descriptor.block_offset == 0 ||
      descriptor.payload_offset == 0 || descriptor.block_generation == 0) {
    return;
  }

  // A UB MsgBuffer stores one detached eRPC header immediately before the
  // payload. Only release a malformed descriptor when its coordinates still
  // identify that exact buffer start; otherwise leave the invalid reference
  // untouched rather than deriving an unsafe allocator pointer.
  if (descriptor.payload_offset < descriptor.block_offset ||
      descriptor.payload_offset - descriptor.block_offset !=
          sizeof(UBBlockMetadata) + sizeof(pkthdr_t)) {
    return;
  }

  try {
    uint8_t *payload = shared_allocator_->resolve_payload(
        descriptor.machine_id, descriptor.block_offset,
        descriptor.payload_offset, descriptor.block_generation, 0);
    if (payload != nullptr) {
      shared_allocator_->free(
          Buffer(payload - sizeof(pkthdr_t), sizeof(pkthdr_t), 0));
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "UB: failed to discard invalid descriptor: %s\n",
                 error.what());
  }
}

void UBTransport::release_pending_noexcept(RemoteEndpoint &remote) noexcept {
  while (!remote.pending_tx.empty()) {
    const Buffer buffer = remote.pending_tx.front().backing_buffer;
    remote.pending_tx.pop_front();
    if (buffer.buf_ == nullptr || shared_allocator_ == nullptr) continue;
    try {
      shared_allocator_->free(buffer);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "UB: failed to release pending TX reference: %s\n",
                   error.what());
    }
  }
}

void UBTransport::release_remote_endpoint_noexcept(
    uint8_t peer_rpc_id) noexcept {
  RemoteEndpoint &remote = remote_endpoints_[peer_rpc_id];
  release_pending_noexcept(remote);
  remote = RemoteEndpoint{};
}

bool UBTransport::resolve_remote_routing_info(
    routing_info_t *routing_info) const {
  if (routing_info == nullptr) return false;
  return ensure_remote_endpoint(decode_routing_info(routing_info));
}

void UBTransport::register_rx_peer(uint8_t peer_rpc_id) {
  rt_assert(peer_rpc_id < ub_config::kMaxRpcEndpoints,
            "UB RX peer rpc_id exceeds SPSC queue matrix");
  uint16_t &references = rx_peer_refcounts_[peer_rpc_id];
  rt_assert(references != UINT16_MAX, "UB RX peer reference count overflow");
  if (references++ != 0) return;

  rt_assert(active_rx_source_count_ < ub_config::kMaxRpcEndpoints,
            "UB active RX source set is full");
  active_rx_sources_[active_rx_source_count_++] = peer_rpc_id;
}

void UBTransport::prepare_disconnect(uint8_t peer_rpc_id) {
  rt_assert(peer_rpc_id < ub_config::kMaxRpcEndpoints,
            "UB disconnect peer rpc_id exceeds SPSC queue matrix");
  rt_assert(rx_peer_refcounts_[peer_rpc_id] > 0,
            "UB disconnect peer is not registered");

  // The disconnect control packet uses UDP. Once the last session for this
  // peer has no in-flight RPCs, its UB mapping can be dropped before sending
  // the disconnect request, allowing the peer to delete its owner region.
  if (rx_peer_refcounts_[peer_rpc_id] != 1) return;
  release_remote_endpoint_noexcept(peer_rpc_id);
  if (active_rx_source_count_ == 1) {
    machine_context_->unmap_remote_machines();
  }
}

void UBTransport::unregister_rx_peer(uint8_t peer_rpc_id) {
  rt_assert(peer_rpc_id < ub_config::kMaxRpcEndpoints,
            "UB RX peer rpc_id exceeds SPSC queue matrix");
  uint16_t &references = rx_peer_refcounts_[peer_rpc_id];
  rt_assert(references > 0, "UB RX peer reference count underflow");
  if (--references != 0) return;

  size_t index = 0;
  while (index < active_rx_source_count_ &&
         active_rx_sources_[index] != peer_rpc_id) {
    ++index;
  }
  rt_assert(index < active_rx_source_count_,
            "UB RX peer is absent from active source set");

  const size_t last = active_rx_source_count_ - 1;
  active_rx_sources_[index] = active_rx_sources_[last];
  active_rx_sources_[last] = UINT8_MAX;
  --active_rx_source_count_;
  next_poll_active_index_ =
      active_rx_source_count_ == 0
          ? 0
          : next_poll_active_index_ % active_rx_source_count_;
  release_remote_endpoint_noexcept(peer_rpc_id);
  if (active_rx_source_count_ == 0) {
    machine_context_->unmap_remote_machines();
  }
}

size_t UBTransport::get_bandwidth() const {
  return kBandwidthGbps * 1000000000ULL / 8;
}

std::string UBTransport::routing_info_str(routing_info_t *routing_info) {
  if (routing_info == nullptr) return "[UB invalid routing info]";
  const UBRoutingInfo info = decode_routing_info(routing_info);
  return "[UB machine " + std::to_string(info.machine_id) + ", RPC " +
         std::to_string(info.rpc_id) + ", slot " +
         std::to_string(info.endpoint_slot) + ", generation " +
         std::to_string(info.endpoint_generation) + "]";
}

void UBTransport::tx_burst(const tx_burst_item_t *tx_burst_arr,
                           size_t num_pkts) {
  const size_t tx_burst_start = profile_start();
  rt_assert(tx_burst_arr != nullptr, "UB tx burst cannot be null");
  rt_assert(num_pkts <= kPostlist, "UB tx burst exceeds postlist");

  drain_all_pending();

  for (size_t i = 0; i < num_pkts; ++i) {
    const tx_burst_item_t &item = tx_burst_arr[i];
    if (kTesting && unlikely(item.drop_)) continue;
    rt_assert(item.routing_info_ != nullptr, "UB routing info is null");
    rt_assert(item.msg_buffer_ != nullptr, "UB message buffer is null");
    rt_assert(item.pkt_idx_ == 0,
              "UB transport uses one descriptor per complete message");

    const UBRoutingInfo route = decode_routing_info(item.routing_info_);
    const size_t endpoint_start = profile_start();
    const bool endpoint_available = ensure_remote_endpoint(route);
    profile_record(profile_stats_.endpoint_resolve, endpoint_start);
    rt_assert(endpoint_available, "UB destination endpoint is unavailable");

    RemoteEndpoint &remote = remote_endpoints_[route.rpc_id];
    if (remote.pending_tx.size() >= kMaxPendingTxPerEndpoint) {
      // The transport API cannot synchronously block the eRPC event loop.
      // Bound local retry state and model this send attempt as packet loss;
      // eRPC's normal timeout path will retry after older descriptors drain.
      continue;
    }

    MsgBuffer *msg_buffer = item.msg_buffer_;
    pkthdr_t *pkthdr = msg_buffer->get_pkthdr_n(0);
    const size_t data_size = pkthdr->msg_size_;
    rt_assert(
        data_size == 0 || shared_allocator_->is_shared_ptr(msg_buffer->buf_),
        "UB payload must come from the endpoint shared arena");

    UBMessageDescriptor descriptor{};
    descriptor.pkthdr = *pkthdr;
    descriptor.machine_id = machine_context_->local_machine_id();
    descriptor.payload_length = static_cast<uint32_t>(data_size);
    Buffer descriptor_buffer;
    if (data_size > 0) {
      uint8_t *block_payload = msg_buffer->buf_ - sizeof(pkthdr_t);
      descriptor.payload_offset =
          shared_allocator_->offset_of(msg_buffer->buf_);
      descriptor.block_offset =
          shared_allocator_->offset_of(block_payload) - sizeof(UBBlockMetadata);
      descriptor.block_generation =
          shared_allocator_->generation_of(block_payload);
      const size_t add_ref_start = profile_start();
      shared_allocator_->add_ref(msg_buffer->buffer_);
      profile_record(profile_stats_.add_ref, add_ref_start);
      descriptor_buffer = msg_buffer->buffer_;
      descriptor.pkthdr.is_zerocopy_ = 1;
    }

    UBSpscQueue &queue = remote.inbox->queues[rpc_id_];
    const size_t tx_queue_start = profile_start();
    if (!remote.pending_tx.empty() ||
        !queue.try_enqueue(remote.producer_tail, remote.cached_consumer_head,
                           descriptor)) {
      // Preserve per-destination order. The descriptor reference remains live
      // while it waits here and is transferred to the receiver only when the
      // descriptor is successfully published to the shared queue.
      remote.pending_tx.emplace_back(descriptor, descriptor_buffer);
    }
    profile_record(profile_stats_.tx_queue, tx_queue_start);
  }
  profile_record(profile_stats_.tx_burst, tx_burst_start);
}

void UBTransport::tx_flush() {
  drain_all_pending();
  testing_.tx_flush_count_++;
}

size_t UBTransport::rx_burst() {
  const size_t rx_burst_start = profile_start();
  rt_assert(rx_ring_ != nullptr, "UB RX ring is not initialized");
  rt_assert(local_inbox_ != nullptr, "UB endpoint inbox is unavailable");
  // rx_burst() runs once per event-loop iteration, so it is also the retry
  // point for descriptors deferred because a remote SPSC queue was full.
  drain_all_pending();
  size_t received = 0;

  while (received < kPostlist) {
    bool found = false;
    for (size_t n = 0; n < active_rx_source_count_; ++n) {
      const size_t active_index =
          (next_poll_active_index_ + n) % active_rx_source_count_;
      const size_t source = active_rx_sources_[active_index];
      UBMessageDescriptor descriptor{};
      const size_t rx_queue_start = profile_start();
      if (!local_inbox_->queues[source].try_dequeue(consumer_heads_[source],
                                                    &descriptor)) {
        profile_record(profile_stats_.rx_queue_empty, rx_queue_start);
        continue;
      }
      profile_record(profile_stats_.rx_queue_hit, rx_queue_start);
      const bool zero_length_coordinates = descriptor.block_offset == 0 &&
                                           descriptor.payload_offset == 0 &&
                                           descriptor.block_generation == 0;
      if (descriptor.payload_length != descriptor.pkthdr.msg_size_ ||
          (descriptor.payload_length == 0 && !zero_length_coordinates)) {
        std::fprintf(stderr,
                     "UB: dropping descriptor with inconsistent payload "
                     "length or zero-length coordinates\n");
        discard_descriptor_noexcept(descriptor);
        continue;
      }
      uint8_t *payload = nullptr;
      if (descriptor.payload_length > 0) {
        const size_t resolve_start = profile_start();
        payload = shared_allocator_->resolve_payload(
            descriptor.machine_id, descriptor.block_offset,
            descriptor.payload_offset, descriptor.block_generation,
            descriptor.payload_length);
        profile_record(profile_stats_.rx_resolve, resolve_start);
        rt_assert(payload != nullptr,
                  "UB descriptor references an invalid block");
      }
      rx_pkthdr_ring_[rx_ring_head_] = descriptor.pkthdr;
      rx_payload_ring_[rx_ring_head_] = payload;
      rx_ring_[rx_ring_head_] =
          reinterpret_cast<uint8_t *>(&rx_pkthdr_ring_[rx_ring_head_]);
      rx_ring_head_ = (rx_ring_head_ + 1) % kNumRxRingEntries;
      next_poll_active_index_ = (active_index + 1) % active_rx_source_count_;
      ++received;
      found = true;
      break;
    }
    if (!found) break;
  }
  profile_record(profile_stats_.rx_burst, rx_burst_start);
  return received;
}

void UBTransport::post_recvs(size_t num_recvs) { _unused(num_recvs); }

uint8_t *UBTransport::get_rx_payload(const pkthdr_t *pkthdr) const {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pkthdr);
  const uintptr_t begin = reinterpret_cast<uintptr_t>(rx_pkthdr_ring_);
  const uintptr_t end =
      reinterpret_cast<uintptr_t>(rx_pkthdr_ring_ + kNumRxRingEntries);
  if (address < begin || address >= end) return nullptr;
  return rx_payload_ring_[(address - begin) / sizeof(pkthdr_t)];
}

}  // namespace erpc

#endif  // ERPC_UB
