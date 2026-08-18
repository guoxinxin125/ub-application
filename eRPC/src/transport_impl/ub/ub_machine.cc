#ifdef ERPC_UB

#include "transport_impl/ub/ub_machine.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <ubs_mem.h>
#include <ubs_mem_def.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "transport_impl/ub/ub_manager_protocol.h"
#include "transport_impl/ub/ub_shared_allocator.h"

namespace erpc {
namespace {

std::mutex g_context_mutex;
UBMachineContext *g_context = nullptr;
size_t g_context_references = 0;

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error("UB machine context: " + message);
}

void check_ub(const std::string &operation, int result) {
  if (result != UBSM_OK) {
    fail(operation + " failed, UBSM error=" + std::to_string(result));
  }
}

std::string provider_host() {
  const char *configured = std::getenv("ERPC_UB_PROVIDER_HOST");
  if (configured != nullptr && configured[0] != '\0') return configured;
  char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
  if (gethostname(hostname, sizeof(hostname)) != 0 ||
      hostname[sizeof(hostname) - 1] != '\0') {
    fail("failed to determine provider hostname");
  }
  return hostname;
}

uint64_t fnv1a64(const char *text) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
       *p != 0; ++p) {
    hash ^= *p;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool send_all(int fd, const void *data, size_t length) {
  const uint8_t *cursor = static_cast<const uint8_t *>(data);
  while (length > 0) {
    const ssize_t sent = send(fd, cursor, length, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return false;
    cursor += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}

bool receive_all(int fd, void *data, size_t length) {
  uint8_t *cursor = static_cast<uint8_t *>(data);
  while (length > 0) {
    const ssize_t received = recv(fd, cursor, length, 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return false;
    cursor += received;
    length -= static_cast<size_t>(received);
  }
  return true;
}

UBManagerResponse manager_request(const UBManagerRequest &request) {
  const std::string socket_path = ub_config::manager_socket_path();
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) fail("failed to create manager socket");

  sockaddr_un address{};
  if (socket_path.size() >= sizeof(address.sun_path)) {
    close(fd);
    fail("ERPC_UB_MANAGER_SOCKET is too long");
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (connect(fd, reinterpret_cast<const sockaddr *>(&address),
              static_cast<socklen_t>(sizeof(address))) != 0) {
    const int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    fail("cannot connect to ub_manager at " + socket_path);
  }

  UBManagerResponse response{};
  const bool ok = send_all(fd, &request, sizeof(request)) &&
                  receive_all(fd, &response, sizeof(response));
  close(fd);
  if (!ok || response.magic != kUBManagerProtocolMagic ||
      response.version != kUBManagerProtocolVersion) {
    fail("invalid response from ub_manager");
  }
  return response;
}

}  // namespace

void ub_sdk_initialize() {
  check_ub("ubsmem_set_logger_level",
           ubsmem_set_logger_level(ub_config::kSdkLogLevel));
  ubsmem_options_t options{};
  check_ub("ubsmem_init_attributes", ubsmem_init_attributes(&options));
  check_ub("ubsmem_initialize", ubsmem_initialize(&options));
}

void ub_sdk_finalize_noexcept() noexcept {
  const int result = ubsmem_finalize();
  if (result != UBSM_OK) {
    std::fprintf(stderr, "UB: ubsmem_finalize failed, error=%d\n", result);
  }
}

uint64_t ub_local_machine_id() {
  const char *configured = std::getenv("ERPC_UB_MACHINE_ID");
  if (configured != nullptr && configured[0] != '\0') {
    char *end = nullptr;
    const unsigned long long value = std::strtoull(configured, &end, 0);
    if (end == configured || *end != '\0' || value == 0) {
      fail("ERPC_UB_MACHINE_ID must be a nonzero integer");
    }
    return static_cast<uint64_t>(value);
  }

  char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
  if (gethostname(hostname, sizeof(hostname)) != 0 ||
      hostname[sizeof(hostname) - 1] != '\0') {
    fail("failed to determine hostname for machine ID");
  }
  return fnv1a64(hostname);
}

std::string ub_machine_region_name(uint64_t machine_id) {
  std::ostringstream stream;
  stream << ub_config::region_prefix() << '_' << std::hex << std::setw(16)
         << std::setfill('0') << machine_id;
  const std::string name = stream.str();
  if (name.empty() || name.size() >= MAX_SHM_NAME_LENGTH) {
    fail("machine region name exceeds UBSM limit");
  }
  return name;
}

UBMachineRegionOwner::UBMachineRegionOwner(ub_config::ProcessMode mode,
                                           size_t numa_node)
    : mode_(mode),
      numa_node_(numa_node),
      region_bytes_(ub_config::machine_region_bytes()),
      machine_id_(ub_local_machine_id()),
      name_(ub_machine_region_name(machine_id_)),
      mapping_(nullptr),
      metadata_(nullptr),
      allocated_(false) {
  const size_t minimum_size = kUBMachineDataOffset + sizeof(UBEndpointInbox) +
                              ub_config::endpoint_arena_bytes();
  if (region_bytes_ < minimum_size) {
    fail("ERPC_UB_REGION_MB is too small for one endpoint inbox");
  }

  const std::string host = provider_host();
  if (host.size() >= MAX_HOST_NAME_DESC_LENGTH) {
    fail("provider hostname is too long");
  }
  ubs_mem_provider_t provider{};
  std::memcpy(provider.host_name, host.c_str(), host.size() + 1);
  provider.socket_id = ub_config::provider_socket();
  provider.numa_id = ub_config::provider_numa(numa_node_);
  provider.port_id = ub_config::provider_port();
  const uint64_t flags = ub_config::allocation_flags();

  check_ub("ubsmem_shmem_allocate_with_provider(" + name_ + ")",
           ubsmem_shmem_allocate_with_provider(
               &provider, name_.c_str(), region_bytes_,
               S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, flags));
  allocated_ = true;
  try {
    check_ub("ubsmem_shmem_map(" + name_ + ")",
             ubsmem_shmem_map(nullptr, region_bytes_, PROT_READ | PROT_WRITE,
                              MAP_SHARED, name_.c_str(), 0, &mapping_));
    if (mapping_ == nullptr || mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      fail("UBSM returned an invalid machine-region mapping");
    }
  } catch (...) {
    (void)ubsmem_shmem_deallocate(name_.c_str());
    allocated_ = false;
    throw;
  }

  std::memset(mapping_, 0, kUBMachineDataOffset);
  metadata_ = static_cast<UBMachineRegionMetadata *>(mapping_);
  metadata_->header.version = ub_config::kRegionVersion;
  metadata_->header.process_mode = static_cast<uint8_t>(mode_);
  metadata_->header.max_endpoints = ub_config::kMaxRpcEndpoints;
  metadata_->header.queue_depth = ub_config::kSpscQueueDepth;
  metadata_->header.mtu = ub_config::kMTU;
  metadata_->header.machine_id = machine_id_;
  metadata_->header.allocation_flags = flags;
  metadata_->header.region_bytes = region_bytes_;
  metadata_->header.next_free_offset = kUBMachineDataOffset;
  metadata_->header.manager_generation = 1;
  ub_atomic::fence(ub_atomic::MemoryOrder::kRelease);
  ub_atomic::store(&metadata_->header.magic, ub_config::kRegionMagic,
                   ub_atomic::MemoryOrder::kRelease);
}

UBMachineRegionOwner::~UBMachineRegionOwner() {
  if (mapping_ != nullptr) {
    (void)ubsmem_shmem_unmap(mapping_, region_bytes_);
    mapping_ = nullptr;
  }
  if (allocated_) {
    (void)ubsmem_shmem_deallocate(name_.c_str());
    allocated_ = false;
  }
}

UBEndpointHandle UBMachineRegionOwner::register_endpoint(uint32_t process_id,
                                                         uint16_t sm_udp_port,
                                                         uint8_t rpc_id) {
  if (rpc_id >= ub_config::kMaxRpcEndpoints) {
    fail("rpc_id exceeds UB source queue index range");
  }
  std::lock_guard<std::mutex> lock(registry_mutex_);

  size_t free_slot = ub_config::kMaxRpcEndpoints;
  for (size_t slot = 0; slot < ub_config::kMaxRpcEndpoints; ++slot) {
    UBEndpointRegistryEntry &entry = metadata_->registry[slot];
    const auto state = static_cast<UBEndpointState>(
        ub_atomic::load(&entry.state, ub_atomic::MemoryOrder::kAcquire));
    if (state == UBEndpointState::kActive && entry.rpc_id == rpc_id) {
      fail("rpc_id is already registered on this machine");
    }
    if (state == UBEndpointState::kFree &&
        free_slot == ub_config::kMaxRpcEndpoints) {
      free_slot = slot;
    }
  }
  if (free_slot == ub_config::kMaxRpcEndpoints) {
    fail("machine endpoint registry is full");
  }

  UBEndpointRegistryEntry &entry = metadata_->registry[free_slot];
  uint64_t inbox_offset = entry.inbox_offset;
  uint64_t arena_offset = entry.arena_offset;
  uint64_t arena_size = entry.arena_size;
  const bool newly_assigned_storage = inbox_offset == 0;
  if (inbox_offset == 0) {
    inbox_offset = metadata_->header.next_free_offset;
    arena_offset = (inbox_offset + sizeof(UBEndpointInbox) + 63ULL) & ~63ULL;
    arena_size = ub_config::endpoint_arena_bytes();
    const uint64_t next_offset = arena_offset + arena_size;
    if (next_offset > region_bytes_) {
      fail("machine region has no space for another endpoint inbox");
    }
    metadata_->header.next_free_offset = next_offset;
    entry.inbox_offset = inbox_offset;
    entry.arena_offset = arena_offset;
    entry.arena_size = arena_size;
  }
  ub_initialize_inbox(ub_inbox_at(mapping_, inbox_offset));
  if (newly_assigned_storage) {
    // UBSM does not promise that a newly allocated region contains zeroes.
    // Initialize every future block-generation field once. Do not repeat this
    // when an endpoint slot is reused: retaining old block generations is what
    // prevents delayed descriptors from aliasing newly allocated blocks.
    std::memset(static_cast<uint8_t *>(mapping_) + arena_offset, 0, arena_size);
  }
  ub_initialize_arena(mapping_, machine_id_, arena_offset, arena_size);

  uint32_t generation = entry.generation + 1;
  if (generation == 0) generation = 1;
  entry.generation = generation;
  entry.process_id = process_id;
  entry.sm_udp_port = sm_udp_port;
  entry.rpc_id = rpc_id;
  ub_atomic::fence(ub_atomic::MemoryOrder::kRelease);
  ub_atomic::store(&entry.state,
                   static_cast<uint32_t>(UBEndpointState::kActive),
                   ub_atomic::MemoryOrder::kRelease);
  return UBEndpointHandle(static_cast<uint8_t>(free_slot), rpc_id, generation,
                          inbox_offset, arena_offset, arena_size);
}

bool UBMachineRegionOwner::unregister_endpoint(const UBEndpointHandle &endpoint,
                                               uint32_t process_id) {
  if (!endpoint.valid()) return false;
  std::lock_guard<std::mutex> lock(registry_mutex_);
  UBEndpointRegistryEntry &entry = metadata_->registry[endpoint.slot];
  if (entry.process_id != process_id ||
      entry.generation != endpoint.generation ||
      entry.inbox_offset != endpoint.inbox_offset ||
      entry.arena_offset != endpoint.arena_offset ||
      entry.arena_size != endpoint.arena_size) {
    return false;
  }
  auto *arena = reinterpret_cast<UBArenaHeader *>(
      static_cast<uint8_t *>(mapping_) + entry.arena_offset);
  if (ub_atomic::load(&arena->active_blocks,
                      ub_atomic::MemoryOrder::kAcquire) != 0) {
    return false;
  }
  ub_atomic::store(&entry.state, static_cast<uint32_t>(UBEndpointState::kFree),
                   ub_atomic::MemoryOrder::kRelease);
  return true;
}

UBMachineContext *UBMachineContext::acquire(size_t numa_node) {
  std::lock_guard<std::mutex> lock(g_context_mutex);
  if (g_context == nullptr) g_context = new UBMachineContext(numa_node);
  ++g_context_references;
  return g_context;
}

void UBMachineContext::release(UBMachineContext *context) noexcept {
  std::lock_guard<std::mutex> lock(g_context_mutex);
  if (context == nullptr || context != g_context || g_context_references == 0) {
    return;
  }
  --g_context_references;
  if (g_context_references == 0) {
    delete g_context;
    g_context = nullptr;
  }
}

UBMachineContext::UBMachineContext(size_t numa_node)
    : mode_(ub_config::process_mode()),
      numa_node_(numa_node),
      region_bytes_(ub_config::machine_region_bytes()),
      machine_id_(ub_local_machine_id()),
      local_base_(nullptr) {
  ub_sdk_initialize();
  try {
    if (mode_ == ub_config::ProcessMode::kSingle) {
      single_owner_.reset(new UBMachineRegionOwner(mode_, numa_node_));
      local_base_ = single_owner_->base();
      region_bytes_ = single_owner_->size();
      machine_id_ = single_owner_->machine_id();
    } else {
      map_local_manager_region();
    }
  } catch (...) {
    ub_sdk_finalize_noexcept();
    throw;
  }
}

UBMachineContext::~UBMachineContext() {
  for (const RemoteMapping &remote : remote_mappings_) {
    if (remote.base != nullptr && remote.base != local_base_) {
      (void)ubsmem_shmem_unmap(remote.base, region_bytes_);
    }
  }
  remote_mappings_.clear();
  if (mode_ == ub_config::ProcessMode::kMulti && local_base_ != nullptr) {
    (void)ubsmem_shmem_unmap(local_base_, region_bytes_);
  }
  local_base_ = nullptr;
  single_owner_.reset();
  ub_sdk_finalize_noexcept();
}

void UBMachineContext::validate_region(void *base,
                                       uint64_t expected_machine_id) const {
  if (base == nullptr || base == MAP_FAILED) fail("invalid machine mapping");
  auto *metadata = static_cast<UBMachineRegionMetadata *>(base);
  const uint32_t magic = ub_atomic::load(&metadata->header.magic,
                                         ub_atomic::MemoryOrder::kAcquire);
  if (magic != ub_config::kRegionMagic ||
      metadata->header.version != ub_config::kRegionVersion ||
      metadata->header.machine_id != expected_machine_id ||
      metadata->header.max_endpoints != ub_config::kMaxRpcEndpoints ||
      metadata->header.queue_depth != ub_config::kSpscQueueDepth ||
      metadata->header.mtu != ub_config::kMTU ||
      metadata->header.region_bytes != region_bytes_ ||
      metadata->header.allocation_flags != ub_config::allocation_flags()) {
    fail("machine region metadata does not match local configuration");
  }
}

void UBMachineContext::map_local_manager_region() {
  const std::string name = ub_machine_region_name(machine_id_);
  check_ub("ubsmem_shmem_map(" + name + ")",
           ubsmem_shmem_map(nullptr, region_bytes_, PROT_READ | PROT_WRITE,
                            MAP_SHARED, name.c_str(), 0, &local_base_));
  try {
    validate_region(local_base_, machine_id_);
    auto *metadata = static_cast<UBMachineRegionMetadata *>(local_base_);
    if (metadata->header.process_mode !=
        static_cast<uint8_t>(ub_config::ProcessMode::kMulti)) {
      fail("multi-process worker mapped a non-manager region");
    }
  } catch (...) {
    if (local_base_ != nullptr && local_base_ != MAP_FAILED) {
      (void)ubsmem_shmem_unmap(local_base_, region_bytes_);
    }
    local_base_ = nullptr;
    throw;
  }
}

UBEndpointHandle UBMachineContext::manager_register(uint16_t sm_udp_port,
                                                    uint8_t rpc_id) {
  UBManagerRequest request{};
  request.operation =
      static_cast<uint16_t>(UBManagerOperation::kRegisterEndpoint);
  request.process_id = static_cast<uint32_t>(getpid());
  request.sm_udp_port = sm_udp_port;
  request.rpc_id = rpc_id;
  const UBManagerResponse response = manager_request(request);
  if (response.status != 0 || !response.endpoint.valid()) {
    fail("ub_manager rejected endpoint registration");
  }
  if (response.machine_id != machine_id_ ||
      response.region_bytes != region_bytes_ ||
      response.allocation_flags != ub_config::allocation_flags()) {
    manager_unregister(response.endpoint);
    fail("ub_manager configuration does not match worker configuration");
  }
  return response.endpoint;
}

void UBMachineContext::manager_unregister(
    const UBEndpointHandle &endpoint) noexcept {
  try {
    UBManagerRequest request{};
    request.operation =
        static_cast<uint16_t>(UBManagerOperation::kUnregisterEndpoint);
    request.process_id = static_cast<uint32_t>(getpid());
    request.rpc_id = endpoint.rpc_id;
    request.slot = endpoint.slot;
    request.generation = endpoint.generation;
    request.inbox_offset = endpoint.inbox_offset;
    request.arena_offset = endpoint.arena_offset;
    request.arena_size = endpoint.arena_size;
    const UBManagerResponse response = manager_request(request);
    if (response.status != 0) {
      std::fprintf(stderr, "UB: ub_manager rejected endpoint unregister\n");
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "UB: endpoint unregister failed: %s\n", error.what());
  }
}

UBEndpointHandle UBMachineContext::register_endpoint(uint16_t sm_udp_port,
                                                     uint8_t rpc_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mode_ == ub_config::ProcessMode::kSingle) {
    return single_owner_->register_endpoint(static_cast<uint32_t>(getpid()),
                                            sm_udp_port, rpc_id);
  }
  return manager_register(sm_udp_port, rpc_id);
}

void UBMachineContext::unregister_endpoint(
    const UBEndpointHandle &endpoint) noexcept {
  if (!endpoint.valid()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (mode_ == ub_config::ProcessMode::kSingle) {
    (void)single_owner_->unregister_endpoint(endpoint,
                                             static_cast<uint32_t>(getpid()));
  } else {
    manager_unregister(endpoint);
  }
}

void *UBMachineContext::map_remote_machine(uint64_t machine_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (machine_id == machine_id_) return local_base_;
  for (const RemoteMapping &remote : remote_mappings_) {
    if (remote.machine_id == machine_id) return remote.base;
  }

  const std::string name = ub_machine_region_name(machine_id);
  void *mapping = nullptr;
  const int result =
      ubsmem_shmem_map(nullptr, region_bytes_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, name.c_str(), 0, &mapping);
  if (result != UBSM_OK || mapping == nullptr || mapping == MAP_FAILED) {
    return nullptr;
  }
  try {
    validate_region(mapping, machine_id);
  } catch (...) {
    (void)ubsmem_shmem_unmap(mapping, region_bytes_);
    return nullptr;
  }
  remote_mappings_.push_back({machine_id, mapping});
  return mapping;
}

bool UBMachineContext::validate_endpoint(
    void *machine_base, uint64_t machine_id,
    const UBEndpointHandle &endpoint) const {
  if (machine_base == nullptr || !endpoint.valid()) return false;
  auto *metadata = static_cast<UBMachineRegionMetadata *>(machine_base);
  const bool inbox_in_range =
      endpoint.inbox_offset >= kUBMachineDataOffset &&
      endpoint.inbox_offset <= region_bytes_ &&
      sizeof(UBEndpointInbox) <= region_bytes_ - endpoint.inbox_offset;
  const bool arena_in_range =
      endpoint.arena_offset >= kUBMachineDataOffset &&
      endpoint.arena_offset <= region_bytes_ &&
      endpoint.arena_size >= sizeof(UBArenaHeader) &&
      endpoint.arena_size <= region_bytes_ - endpoint.arena_offset;
  if (metadata->header.machine_id != machine_id || !inbox_in_range ||
      !arena_in_range ||
      endpoint.inbox_offset % alignof(UBEndpointInbox) != 0 ||
      endpoint.arena_offset % alignof(UBArenaHeader) != 0 ||
      endpoint.arena_offset < endpoint.inbox_offset + sizeof(UBEndpointInbox)) {
    return false;
  }
  const UBEndpointRegistryEntry &entry = metadata->registry[endpoint.slot];
  const auto state = static_cast<UBEndpointState>(
      ub_atomic::load(&entry.state, ub_atomic::MemoryOrder::kAcquire));
  return state == UBEndpointState::kActive &&
         entry.generation == endpoint.generation &&
         entry.rpc_id == endpoint.rpc_id &&
         entry.inbox_offset == endpoint.inbox_offset &&
         entry.arena_offset == endpoint.arena_offset &&
         entry.arena_size == endpoint.arena_size;
}

}  // namespace erpc

#endif  // ERPC_UB
