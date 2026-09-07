#include "common/UBMemory.h"
#include "core/Context.h"

#include <glog/logging.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

#ifdef TIGON_ENABLE_UB
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ubs_mem.h>
#include <ubs_mem_def.h>
#endif

namespace star
{

UBMemory ub_memory;

namespace
{

std::runtime_error ub_error(const std::string &operation, int result)
{
        return std::runtime_error(operation + " failed, UBS Memory error=" +
                                  std::to_string(result));
}

} // namespace

UBMemory::~UBMemory()
{
        if (!initialized_ && !sdk_initialized_)
                return;
        try {
                shutdown(true);
        } catch (const std::exception &e) {
                LOG(ERROR) << "UB cleanup in destructor failed: " << e.what();
        }
}

uint64_t UBMemory::align_up(uint64_t value, uint64_t alignment)
{
        if (alignment == 0 || (alignment & (alignment - 1)) != 0)
                throw std::invalid_argument("UB allocation alignment must be a power of two");
        if (value > std::numeric_limits<uint64_t>::max() - (alignment - 1))
                throw std::overflow_error("UB allocation alignment overflow");
        return (value + alignment - 1) & ~(alignment - 1);
}

void UBMemory::validate_context(const Context &context) const
{
        if (context.coordinator_num == 0 || context.coordinator_id >= context.coordinator_num)
                throw std::invalid_argument("invalid UB coordinator id/count");
        if (context.ub_region_size < 4ULL * 1024 * 1024 ||
            context.ub_region_size % (4ULL * 1024 * 1024) != 0)
                throw std::invalid_argument("UB region size must be a multiple of 4 MiB");
        if (context.ub_region_prefix.empty())
                throw std::invalid_argument("UB region prefix must not be empty");
        // suffix "_<10 digit id>" and NUL must fit UBSM's 48-byte ABI.
        if (context.ub_region_prefix.size() + 12 >= 48)
                throw std::invalid_argument("UB region prefix is too long");
        if (context.ub_map_timeout_seconds <= 0)
                throw std::invalid_argument("UB map timeout must be positive");
        if (context.ub_memory_mode != "one-sided" && context.ub_memory_mode != "nocache")
                throw std::invalid_argument("UB memory mode must be one-sided or nocache");
}

void UBMemory::initialize_sdk()
{
#ifdef TIGON_ENABLE_UB
        int result = ubsmem_set_logger_level(3);
        if (result != UBSM_OK)
                throw ub_error("ubsmem_set_logger_level", result);
        ubsmem_options_t options{};
        result = ubsmem_init_attributes(&options);
        if (result != UBSM_OK)
                throw ub_error("ubsmem_init_attributes", result);
        result = ubsmem_initialize(&options);
        if (result != UBSM_OK)
                throw ub_error("ubsmem_initialize", result);
        sdk_initialized_ = true;
#else
        throw std::runtime_error("Tigon was built without TIGON_ENABLE_UB");
#endif
}

void UBMemory::initialize(const Context &context)
{
        if (initialized_)
                throw std::logic_error("UBMemory is already initialized");
        validate_context(context);
        local_region_id_ = static_cast<uint32_t>(context.coordinator_id);
        memory_mode_ = context.ub_memory_mode == "nocache" ? MemoryMode::NOCACHE
                                                            : MemoryMode::ONE_SIDED;
        regions_.resize(context.coordinator_num);
        for (uint32_t i = 0; i < regions_.size(); ++i) {
                regions_[i].name = context.ub_region_prefix + "_" + std::to_string(i);
                regions_[i].size = context.ub_region_size;
                regions_[i].owner = i == local_region_id_;
        }

        try {
                initialize_sdk();
                allocate_local_region(context);
                map_region_with_retry(
                        local_region_id_,
                        std::chrono::steady_clock::now() +
                                std::chrono::seconds(
                                        context.ub_map_timeout_seconds));
                initialize_local_header(static_cast<uint32_t>(context.coordinator_num));

                // Every process resolves any global pointer locally, so every process
                // maps every coordinator-owned region exactly once.
                for (uint32_t i = 0; i < regions_.size(); ++i) {
                        if (i == local_region_id_)
                                continue;
                        const MapDeadline deadline =
                                std::chrono::steady_clock::now() +
                                std::chrono::seconds(
                                        context.ub_map_timeout_seconds);
                        map_region_with_retry(i, deadline);
                        validate_remote_header(i, deadline);
                }
                initialized_ = true;
        } catch (...) {
                shutdown(true);
                throw;
        }

        LOG(INFO) << "UB memory initialized: local_region=" << local_region_id_
                  << " regions=" << regions_.size()
                  << " mode=" << context.ub_memory_mode
                  << " bytes_per_region=" << context.ub_region_size;
}

void UBMemory::allocate_local_region(const Context &context)
{
#ifdef TIGON_ENABLE_UB
        RegionMapping &region = regions_.at(local_region_id_);
        const uint64_t flags = (memory_mode_ == MemoryMode::ONE_SIDED
                                        ? UBSM_FLAG_ONLY_IMPORT_NONCACHE
                                        : UBSM_FLAG_NONCACHE) |
                               UBSM_FLAG_WR_DELAY_COMP;
        int result;
        if (context.ub_provider_host.empty()) {
                result = ubsmem_shmem_allocate("default", region.name.c_str(), region.size,
                                               S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
                                               flags);
        } else {
                if (context.ub_provider_host.size() >= MAX_HOST_NAME_DESC_LENGTH)
                        throw std::invalid_argument("UB provider hostname is too long");
                ubs_mem_provider_t provider{};
                std::memcpy(provider.host_name, context.ub_provider_host.c_str(),
                            context.ub_provider_host.size() + 1);
                provider.socket_id = context.ub_provider_socket;
                provider.numa_id = context.ub_provider_numa;
                provider.port_id = context.ub_provider_port;
                result = ubsmem_shmem_allocate_with_provider(
                        &provider, region.name.c_str(), region.size,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, flags);
        }
        if (result != UBSM_OK)
                throw ub_error("allocate owner region " + region.name, result);
        region.allocated = true;
#else
        (void)context;
#endif
}

void UBMemory::map_region_with_retry(uint32_t region_id, MapDeadline deadline)
{
#ifdef TIGON_ENABLE_UB
        RegionMapping &region = regions_.at(region_id);
        int last_result = UBSM_ERR_NOT_FOUND;
        while (std::chrono::steady_clock::now() < deadline) {
                void *base = nullptr;
                last_result = ubsmem_shmem_map(nullptr, region.size,
                                               PROT_READ | PROT_WRITE, MAP_SHARED,
                                               region.name.c_str(), 0, &base);
                if (last_result == UBSM_OK && base != nullptr && base != MAP_FAILED) {
                        region.base = base;
                        region.header = static_cast<RegionHeader *>(base);
                        return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        throw ub_error("map region " + region.name, last_result);
#else
        (void)region_id;
        (void)deadline;
#endif
}

void UBMemory::initialize_local_header(uint32_t coordinator_count)
{
        RegionMapping &region = regions_.at(local_region_id_);
        RegionHeader *header = region.header;
        // A unique run prefix is required. Refuse to silently reuse a live or
        // stale Tigon ABI header because stale global pointers would look valid.
        if (header->magic == abi_magic)
                throw std::runtime_error("owner UB region already contains a Tigon header: " +
                                         region.name);

        std::memset(header, 0, sizeof(*header));
        header->magic = abi_magic;
        header->abi = abi_version;
        header->region_id = local_region_id_;
        header->generation = static_cast<uint32_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^
                local_region_id_;
#ifdef TIGON_ENABLE_UB
        header->generation ^= static_cast<uint32_t>(::getpid());
#endif
        if (header->generation == 0)
                header->generation = 1;
        header->coordinator_count = coordinator_count;
        header->region_size = region.size;
        new (&header->bump_offset) std::atomic<uint64_t>(
                align_up(sizeof(RegionHeader), allocation_alignment));
        new (&header->ready) std::atomic<uint32_t>(0);
        for (uint32_t i = 0; i < root_slot_count; ++i) {
                new (&header->roots[i].sequence) std::atomic<uint64_t>(0);
                new (&header->roots[i].region_generation)
                        std::atomic<uint64_t>(
                                (static_cast<uint64_t>(UBGlobalPtr::null_region) << 32));
                new (&header->roots[i].offset) std::atomic<uint64_t>(0);
        }
        std::atomic_thread_fence(std::memory_order_release);
        header->ready.store(1, std::memory_order_release);
}

void UBMemory::validate_remote_header(uint32_t region_id,
                                      MapDeadline deadline) const
{
        const RegionMapping &region = regions_.at(region_id);
        RegionHeader *header = region.header;
        while (header->ready.load(std::memory_order_acquire) != 1) {
                if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error(
                                "timed out waiting for UB region header: " +
                                region.name);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (header->magic != abi_magic || header->abi != abi_version ||
            header->region_id != region_id || header->region_size != region.size ||
            header->coordinator_count != regions_.size())
                throw std::runtime_error("incompatible UB region header: " + region.name);
}

UBGlobalPtr UBMemory::allocate(uint32_t region_id, uint64_t size, uint64_t alignment)
{
        if (!initialized_ || size == 0)
                throw std::invalid_argument("invalid UB allocation request");
        RegionMapping &region = regions_.at(region_id);
        RegionHeader *header = region.header;
        uint64_t current = header->bump_offset.load(std::memory_order_acquire);
        while (true) {
                const uint64_t aligned = align_up(current, alignment);
                if (aligned > region.size || size > region.size - aligned)
                        throw std::bad_alloc();
                const uint64_t next = aligned + size;
                if (header->bump_offset.compare_exchange_weak(
                            current, next, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        std::memset(static_cast<char *>(region.base) + aligned, 0, size);
                        return UBGlobalPtr(region_id, header->generation, aligned);
                }
        }
}

void *UBMemory::resolve(const UBGlobalPtr &ptr, uint64_t length) const
{
        if (!initialized_ || ptr.is_null() || ptr.region_id >= regions_.size())
                return nullptr;
        const RegionMapping &region = regions_[ptr.region_id];
        if (region.header == nullptr || ptr.generation != region.header->generation)
                return nullptr;
        if (ptr.offset > region.size || length > region.size - ptr.offset)
                return nullptr;
        return static_cast<char *>(region.base) + ptr.offset;
}

UBGlobalPtr UBMemory::to_global(uint32_t region_id, const void *address) const
{
        const RegionMapping &region = regions_.at(region_id);
        const char *base = static_cast<const char *>(region.base);
        const char *pointer = static_cast<const char *>(address);
        if (pointer < base || static_cast<uint64_t>(pointer - base) >= region.size)
                throw std::out_of_range("address does not belong to the requested UB region");
        return UBGlobalPtr(region_id, region.header->generation,
                           static_cast<uint64_t>(pointer - base));
}

bool UBMemory::contains_address(const void *address, uint64_t length) const
{
        if (!initialized_ || address == nullptr)
                return false;
        const char *pointer = static_cast<const char *>(address);
        for (const RegionMapping &region : regions_) {
                const char *base = static_cast<const char *>(region.base);
                if (base == nullptr || pointer < base)
                        continue;
                const uint64_t offset = static_cast<uint64_t>(pointer - base);
                if (offset <= region.size && length <= region.size - offset)
                        return true;
        }
        return false;
}

void UBMemory::publish_root(uint32_t region_id, uint32_t slot, UBGlobalPtr value)
{
        if (slot >= root_slot_count || value.region_id != region_id ||
            region_id != local_region_id_)
                throw std::invalid_argument("invalid UB root publication");
        RootSlot &root = regions_.at(region_id).header->roots[slot];
        const uint64_t sequence = root.sequence.load(std::memory_order_acquire);
        root.sequence.store(sequence + 1, std::memory_order_release);
        root.region_generation.store((static_cast<uint64_t>(value.region_id) << 32) |
                                             value.generation,
                                     std::memory_order_relaxed);
        root.offset.store(value.offset, std::memory_order_relaxed);
        root.sequence.store(sequence + 2, std::memory_order_release);
}

UBGlobalPtr UBMemory::read_root(uint32_t region_id, uint32_t slot) const
{
        if (slot >= root_slot_count)
                throw std::out_of_range("invalid UB root slot");
        const RootSlot &root = regions_.at(region_id).header->roots[slot];
        while (true) {
                const uint64_t before = root.sequence.load(std::memory_order_acquire);
                if ((before & 1) != 0) {
                        std::this_thread::yield();
                        continue;
                }
                const uint64_t packed = root.region_generation.load(std::memory_order_relaxed);
                const uint64_t offset = root.offset.load(std::memory_order_relaxed);
                const uint64_t after = root.sequence.load(std::memory_order_acquire);
                if (before == after)
                        return UBGlobalPtr(static_cast<uint32_t>(packed >> 32),
                                           static_cast<uint32_t>(packed), offset);
        }
}

uint64_t UBMemory::region_size(uint32_t region_id) const
{
        return regions_.at(region_id).size;
}

const std::string &UBMemory::region_name(uint32_t region_id) const
{
        return regions_.at(region_id).name;
}

void UBMemory::shutdown(bool deallocate_owner)
{
#ifdef TIGON_ENABLE_UB
        unmap_imported_regions();
        if (local_region_id_ < regions_.size()) {
                RegionMapping &local = regions_[local_region_id_];
                if (local.base != nullptr) {
                        const int result = ubsmem_shmem_unmap(local.base, local.size);
                        if (result != UBSM_OK)
                                LOG(ERROR) << "unmap owner region " << local.name
                                           << " failed, UBS Memory error=" << result;
                        else
                                local.base = nullptr;
                }
                if (deallocate_owner && local.allocated) {
                        const int result = ubsmem_shmem_deallocate(local.name.c_str());
                        if (result != UBSM_OK)
                                LOG(ERROR) << "deallocate owner region " << local.name
                                           << " failed, UBS Memory error=" << result;
                        else
                                local.allocated = false;
                }
        }
        if (sdk_initialized_) {
                const int result = ubsmem_finalize();
                if (result != UBSM_OK)
                        LOG(ERROR) << "ubsmem_finalize failed, UBS Memory error=" << result;
        }
#endif
        regions_.clear();
        sdk_initialized_ = false;
        initialized_ = false;
        local_region_id_ = UBGlobalPtr::null_region;
}

void UBMemory::unmap_imported_regions()
{
#ifdef TIGON_ENABLE_UB
        for (uint32_t i = 0; i < regions_.size(); ++i) {
                if (i == local_region_id_ || regions_[i].base == nullptr)
                        continue;
                const int result = ubsmem_shmem_unmap(regions_[i].base, regions_[i].size);
                if (result != UBSM_OK)
                        LOG(ERROR) << "unmap imported region " << regions_[i].name
                                   << " failed, UBS Memory error=" << result;
                else
                        regions_[i].base = nullptr;
        }
#endif
}

} // namespace star
