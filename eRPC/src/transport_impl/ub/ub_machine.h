#pragma once

#ifdef ERPC_UB

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "transport_impl/ub/ub_config.h"
#include "transport_impl/ub/ub_machine_layout.h"

namespace erpc {

void ub_sdk_initialize();
void ub_sdk_finalize_noexcept() noexcept;
uint64_t ub_local_machine_id();
std::string ub_machine_region_name(uint64_t machine_id);

class UBMachineRegionOwner {
 public:
  UBMachineRegionOwner(ub_config::ProcessMode mode, size_t numa_node);
  ~UBMachineRegionOwner();

  UBMachineRegionOwner(const UBMachineRegionOwner &) = delete;
  UBMachineRegionOwner &operator=(const UBMachineRegionOwner &) = delete;

  UBEndpointHandle register_endpoint(uint32_t process_id, uint16_t sm_udp_port,
                                     uint8_t rpc_id);
  bool unregister_endpoint(const UBEndpointHandle &endpoint,
                           uint32_t process_id);

  void *base() const { return mapping_; }
  size_t size() const { return region_bytes_; }
  uint64_t machine_id() const { return machine_id_; }
  const std::string &name() const { return name_; }

 private:
  ub_config::ProcessMode mode_;
  size_t numa_node_;
  size_t region_bytes_;
  uint64_t machine_id_;
  std::string name_;
  void *mapping_;
  UBMachineRegionMetadata *metadata_;
  bool allocated_;
  std::mutex registry_mutex_;
};

class UBMachineContext {
 public:
  static UBMachineContext *acquire(size_t numa_node);
  static void release(UBMachineContext *context) noexcept;

  UBEndpointHandle register_endpoint(uint16_t sm_udp_port, uint8_t rpc_id);
  void unregister_endpoint(const UBEndpointHandle &endpoint) noexcept;

  void *local_base() const { return local_base_; }
  uint64_t local_machine_id() const { return machine_id_; }
  size_t region_bytes() const { return region_bytes_; }

  void *acquire_remote_machine(uint64_t machine_id);
  void release_remote_machine(uint64_t machine_id) noexcept;
  bool validate_endpoint(void *machine_base, uint64_t machine_id,
                         const UBEndpointHandle &endpoint) const;

 private:
  struct RemoteMapping {
    uint64_t machine_id;
    void *base;
    // One reference per RemoteEndpoint that caches this mapping.
    size_t references;
  };

  explicit UBMachineContext(size_t numa_node);
  ~UBMachineContext();

  UBEndpointHandle manager_register(uint16_t sm_udp_port, uint8_t rpc_id);
  void manager_unregister(const UBEndpointHandle &endpoint) noexcept;
  void map_local_manager_region();
  void validate_region(void *base, uint64_t expected_machine_id) const;

  ub_config::ProcessMode mode_;
  size_t numa_node_;
  size_t region_bytes_;
  uint64_t machine_id_;
  void *local_base_;
  std::unique_ptr<UBMachineRegionOwner> single_owner_;
  std::vector<RemoteMapping> remote_mappings_;
  std::mutex mutex_;
};

}  // namespace erpc

#endif  // ERPC_UB
