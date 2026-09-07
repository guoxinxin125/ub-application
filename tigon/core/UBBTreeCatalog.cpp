#include "core/UBBTreeCatalog.h"

#include "common/UBMemory.h"

#include <new>
#include <stdexcept>

namespace star
{

void UBBTreeCatalog::initialize(uint32_t owner_region)
{
        if (owner_region != ub_memory.local_region_id())
                throw std::invalid_argument("only the owner can initialize a UB B+ Tree catalog");
        if (find_catalog(owner_region) != nullptr)
                return;

        const UBGlobalPtr ptr = ub_memory.allocate(owner_region, sizeof(Catalog), alignof(Catalog));
        Catalog *catalog = ub_memory.resolve_as<Catalog>(ptr);
        catalog->abi = abi_version;
        catalog->owner_region = owner_region;
        catalog->region_generation = ptr.generation;
        catalog->reserved = 0;
        new (&catalog->table_count) std::atomic<uint32_t>(0);
        for (uint32_t i = 0; i < max_tables_per_region; ++i)
                new (&catalog->tables[i].ready) std::atomic<uint32_t>(0);
        ub_memory.publish_root(owner_region, catalog_root_slot, ptr);
}

UBBTreeCatalog::Catalog *UBBTreeCatalog::find_catalog(uint32_t owner_region)
{
        const UBGlobalPtr ptr = ub_memory.read_root(owner_region, catalog_root_slot);
        if (!ptr)
                return nullptr;
        Catalog *catalog = ub_memory.resolve_as<Catalog>(ptr);
        if (catalog == nullptr || catalog->abi != abi_version ||
            catalog->owner_region != owner_region ||
            catalog->region_generation != ptr.generation)
                throw std::runtime_error("incompatible UB B+ Tree catalog ABI");
        return catalog;
}

UBBTreeCatalog::TableDescriptor *UBBTreeCatalog::find(
        uint32_t owner_region, uint32_t table_id, uint32_t partition_id)
{
        Catalog *catalog = find_catalog(owner_region);
        if (catalog == nullptr)
                return nullptr;
        for (uint32_t i = 0; i < max_tables_per_region; ++i) {
                TableDescriptor &descriptor = catalog->tables[i];
                if (descriptor.ready.load(std::memory_order_acquire) == 2 &&
                    descriptor.table_id == table_id &&
                    descriptor.partition_id == partition_id)
                        return &descriptor;
        }
        return nullptr;
}

UBBTreeCatalog::TableDescriptor *UBBTreeCatalog::claim(
        uint32_t owner_region, uint32_t table_id, uint32_t partition_id)
{
        if (owner_region != ub_memory.local_region_id())
                throw std::invalid_argument("only the owner can create a UB B+ Tree");
        initialize(owner_region);
        if (TableDescriptor *existing = find(owner_region, table_id, partition_id))
                return existing;

        Catalog *catalog = find_catalog(owner_region);
        for (uint32_t i = 0; i < max_tables_per_region; ++i) {
                TableDescriptor &descriptor = catalog->tables[i];
                uint32_t free = 0;
                if (descriptor.ready.compare_exchange_strong(
                            free, 1, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        catalog->table_count.fetch_add(1, std::memory_order_relaxed);
                        return &descriptor;
                }
        }
        throw std::runtime_error("UB B+ Tree catalog is full");
}

} // namespace star
