#include "core/UBBPlusTree.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

constexpr uint64_t arena_size = 16ULL * 1024 * 1024;
alignas(64) unsigned char arena[arena_size];
std::atomic<uint64_t> arena_bump{64};

void require(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

struct TestKey {
        uint64_t value;
};

struct TestValue {
        uint64_t value;
        uint64_t checksum;
};

struct TestComparator {
        int operator()(const TestKey &a, const TestKey &b) const
        {
                return a.value < b.value ? -1 : (a.value > b.value ? 1 : 0);
        }
};

using TestTree = star::UBBPlusTree<TestKey, TestValue, TestComparator, 7, 7>;

} // namespace

// This test supplies only the address-space operations used by UBBPlusTree.
// The production UBMemory implementation is intentionally not linked here.
namespace star
{

UBMemory ub_memory;

UBMemory::~UBMemory() = default;

uint64_t UBMemory::align_up(uint64_t value, uint64_t alignment)
{
        return (value + alignment - 1) & ~(alignment - 1);
}

UBGlobalPtr UBMemory::allocate(uint32_t region_id, uint64_t size, uint64_t alignment)
{
        require(region_id == 0, "unexpected test region");
        uint64_t current = arena_bump.load(std::memory_order_relaxed);
        while (true) {
                const uint64_t aligned = align_up(current, alignment);
                const uint64_t next = aligned + size;
                require(next <= arena_size, "test arena exhausted");
                if (arena_bump.compare_exchange_weak(current, next,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
                        std::memset(arena + aligned, 0, size);
                        return UBGlobalPtr(0, 1, aligned);
                }
        }
}

void *UBMemory::resolve(const UBGlobalPtr &ptr, uint64_t length) const
{
        if (!ptr || ptr.region_id != 0 || ptr.generation != 1 ||
            ptr.offset > arena_size || length > arena_size - ptr.offset)
                return nullptr;
        return arena + ptr.offset;
}

UBGlobalPtr UBMemory::to_global(uint32_t region_id, const void *address) const
{
        require(region_id == 0, "unexpected test region");
        const auto *pointer = static_cast<const unsigned char *>(address);
        require(pointer >= arena && pointer < arena + arena_size,
                "address is outside the test arena");
        return UBGlobalPtr(0, 1, static_cast<uint64_t>(pointer - arena));
}

} // namespace star

int main()
{
        TestTree::Descriptor descriptor{};
        descriptor.ready.store(2, std::memory_order_relaxed);
        descriptor.abi = star::UBBTreeCatalog::abi_version;
        descriptor.table_id = 1;
        descriptor.partition_id = 0;
        descriptor.owner_region = 0;
        descriptor.region_generation = 1;
        descriptor.key_size = sizeof(TestKey);
        descriptor.value_size = sizeof(TestValue);
        descriptor.leaf_capacity = TestTree::leaf_capacity;
        descriptor.inner_capacity = TestTree::inner_capacity;
        descriptor.leaf_node_size = sizeof(TestTree::LeafNode);
        descriptor.inner_node_size = sizeof(TestTree::InnerNode);
        descriptor.root_offset.store(0, std::memory_order_relaxed);
        descriptor.end_gap_offset.store(0, std::memory_order_relaxed);
        descriptor.tree_version.store(0, std::memory_order_relaxed);
        descriptor.tuple_count.store(0, std::memory_order_relaxed);
        descriptor.leaf_count.store(0, std::memory_order_relaxed);
        descriptor.inner_count.store(0, std::memory_order_relaxed);
        descriptor.allocated_tuples.store(0, std::memory_order_relaxed);
        descriptor.reused_tuples.store(0, std::memory_order_relaxed);

        const star::UBGlobalPtr root_ptr = star::ub_memory.allocate(
                0, sizeof(TestTree::LeafNode), alignof(TestTree::LeafNode));
        new (star::ub_memory.resolve_as<TestTree::LeafNode>(root_ptr))
                TestTree::LeafNode();
        descriptor.root_offset.store(root_ptr.offset, std::memory_order_release);
        descriptor.leaf_count.store(1, std::memory_order_relaxed);
        const star::UBGlobalPtr end_gap_ptr = star::ub_memory.allocate(
                0, sizeof(TestTree::TupleNode), alignof(TestTree::TupleNode));
        new (star::ub_memory.resolve_as<TestTree::TupleNode>(end_gap_ptr))
                TestTree::TupleNode(TestKey{}, TestValue{}, 1);
        descriptor.end_gap_offset.store(end_gap_ptr.offset,
                                        std::memory_order_release);

        TestTree tree(&descriptor);
        constexpr uint64_t thread_count = 4;
        constexpr uint64_t keys_per_thread = 256;
        std::vector<std::thread> threads;
        for (uint64_t thread_id = 0; thread_id < thread_count; ++thread_id) {
                threads.emplace_back([&, thread_id]() {
                        for (uint64_t i = 0; i < keys_per_thread; ++i) {
                                const TestKey key{thread_id * keys_per_thread + i};
                                const TestValue value{key.value, key.value ^ 0x5a5a};
                                require(tree.insert(key, value),
                                        "concurrent insert failed");
                        }
                });
        }
        for (auto &thread : threads)
                thread.join();

        require(descriptor.tuple_count.load(std::memory_order_acquire) ==
                        thread_count * keys_per_thread,
                "tuple count mismatch");
        require(descriptor.leaf_count.load(std::memory_order_acquire) > 1,
                "leaf split was not exercised");
        require(descriptor.inner_count.load(std::memory_order_acquire) > 1,
                "inner/root split was not exercised");

        uint64_t count = 0;
        uint64_t previous = 0;
        tree.scan(TestKey{0}, [&](const TestTree::TupleRef &ref) {
                require(ref.value->value == ref.key->value,
                        "value/key mismatch");
                require(ref.value->checksum == (ref.key->value ^ 0x5a5a),
                        "value checksum mismatch");
                require(count == 0 || ref.key->value > previous,
                        "scan order is not strictly increasing");
                previous = ref.key->value;
                ++count;
                return false;
        });
        require(count == thread_count * keys_per_thread,
                "scan count mismatch");

        const TestKey reused_key{17};
        auto before = tree.search(reused_key);
        require(static_cast<bool>(before), "reuse key was not found");
        require(tree.erase(reused_key), "erase failed");
        require(!tree.search(reused_key), "erased tuple remains visible");
        const TestValue replacement{1700, 0xbeef};
        require(tree.insert(reused_key, replacement), "same-key reuse failed");
        auto after = tree.search(reused_key);
        require(after && after.node == before.node,
                "same-key tombstone changed tuple address");
        require(after.value->value == replacement.value,
                "replacement value mismatch");
        require(descriptor.reused_tuples.load(std::memory_order_acquire) == 1,
                "reuse counter mismatch");

        const uint64_t allocated_before_churn =
                descriptor.allocated_tuples.load(std::memory_order_acquire);
        constexpr uint64_t same_key_churn_iterations = 10000;
        for (uint64_t i = 0; i < same_key_churn_iterations; ++i) {
                require(tree.erase(reused_key), "same-key churn erase failed");
                require(tree.insert(reused_key, TestValue{i, i ^ 0x5a5a}, 1),
                        "same-key churn insert failed");
                require(tree.search(reused_key).node == before.node,
                        "same-key churn changed the stable tuple address");
        }
        require(descriptor.allocated_tuples.load(std::memory_order_acquire) ==
                        allocated_before_churn,
                "same-key churn unexpectedly consumed region space");
        require(descriptor.reused_tuples.load(std::memory_order_acquire) ==
                        same_key_churn_iterations + 1,
                "same-key churn reuse counter mismatch");

        const TestKey placeholder_key{2000};
        require(tree.insert(placeholder_key, TestValue{1, 2}, 2),
                "placeholder insert failed");
        require(!tree.search(placeholder_key), "placeholder became visible");
        require(tree.discard_placeholder(placeholder_key),
                "placeholder discard failed");
        require(!tree.search(placeholder_key),
                "discarded placeholder became visible");

        uint64_t range_rows = 0;
        uint64_t protection_key = 0;
        require(tree.scan_range(
                        TestKey{10}, TestKey{20}, 0,
                        [&](const TestTree::TupleRef &ref, bool protection) {
                                if (protection)
                                        protection_key = ref.key->value;
                                else {
                                        require(ref.key->value >= 10 &&
                                                        ref.key->value <= 20,
                                                "range returned an out-of-bound key");
                                        ++range_rows;
                                }
                                return true;
                        }),
                "bounded range scan failed");
        require(range_rows == 11 && protection_key == 21,
                "range successor selection mismatch");

        range_rows = 0;
        protection_key = 0;
        require(tree.scan_range(
                        TestKey{10}, TestKey{20}, 5,
                        [&](const TestTree::TupleRef &ref, bool protection) {
                                if (protection)
                                        protection_key = ref.key->value;
                                else
                                        ++range_rows;
                                return true;
                        }),
                "limited range scan failed");
        require(range_rows == 5 && protection_key == 15,
                "limited range successor mismatch");

        const TestKey phantom_key{15};
        require(tree.erase(phantom_key), "phantom test erase failed");
        std::vector<star::UBTupleHeader *> held_range_locks;
        require(tree.scan_range(
                        TestKey{10}, TestKey{20}, 0,
                        [&](const TestTree::TupleRef &ref, bool) {
                                if (!ref.header->lock.try_lock_shared())
                                        return false;
                                held_range_locks.push_back(ref.header);
                                return true;
                        }),
                "phantom-protected scan lock failed");
        bool next_lock_acquired = false;
        const TestValue phantom_value{15, 15 ^ 0x5a5a};
        require(!tree.insert_lock_next(
                        phantom_key, phantom_value,
                        [&](const TestTree::TupleRef &next) {
                                next_lock_acquired = next.header->lock.try_lock();
                                return next_lock_acquired;
                        }),
                "insert crossed a protected scan gap");
        require(!next_lock_acquired,
                "insert unexpectedly locked the protected successor");
        for (star::UBTupleHeader *header : held_range_locks)
                header->lock.unlock_shared();

        star::UBTupleHeader *locked_successor = nullptr;
        require(tree.insert_lock_next(
                        phantom_key, phantom_value,
                        [&](const TestTree::TupleRef &next) {
                                if (!next.header->lock.try_lock())
                                        return false;
                                locked_successor = next.header;
                                return true;
                        }),
                "insert after releasing the protected gap failed");
        require(locked_successor != nullptr,
                "successful insert did not lock its successor");
        locked_successor->lock.unlock();

        TestTree::TupleRef end_gap;
        require(tree.scan_range(
                        TestKey{1020}, TestKey{5000}, 0,
                        [&](const TestTree::TupleRef &ref, bool protection) {
                                if (protection)
                                        end_gap = ref;
                                return true;
                        }),
                "end-gap scan failed");
        require(end_gap &&
                        star::ub_memory.to_global(0, end_gap.node).offset ==
                                descriptor.end_gap_offset.load(
                                        std::memory_order_acquire),
                "scan did not return the stable infinity-gap tuple");
        return 0;
}
