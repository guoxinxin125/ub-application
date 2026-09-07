#include "common/UBMPSCRingBuffer.h"
#include "common/UBMemory.h"
#include "core/Context.h"
#include "core/UBBPlusTree.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

struct TestKey {
        uint64_t value;
};

struct TestValue {
        uint64_t counter;
        uint64_t checksum;
};

struct TestKeyComparator {
        int operator()(const TestKey &a, const TestKey &b) const
        {
                return a.value < b.value ? -1 : (a.value > b.value ? 1 : 0);
        }
};

using TestTree = star::UBBPlusTree<TestKey, TestValue, TestKeyComparator>;

struct TestMessage {
        uint64_t source;
        uint64_t sequence;
        uint64_t checksum;
};

class TcpBarrier {
    public:
        TcpBarrier(uint32_t id, const std::string &sync_ip, uint16_t port)
        {
                if (id == 0)
                        accept_peer(sync_ip, port);
                else
                        connect_to_peer(sync_ip, port);
        }

        ~TcpBarrier()
        {
                if (fd_ >= 0)
                        close(fd_);
        }

        void sync()
        {
                const char outgoing = 'B';
                char incoming = 0;
                write_all(&outgoing, 1);
                read_all(&incoming, 1);
                if (incoming != outgoing)
                        throw std::runtime_error("invalid TCP barrier byte");
        }

    private:
        void accept_peer(const std::string &sync_ip, uint16_t port)
        {
                const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (listen_fd < 0)
                        throw std::runtime_error("socket() failed");
                int reuse = 1;
                setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                           sizeof(reuse));
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(port);
                if (inet_pton(AF_INET, sync_ip.c_str(), &address.sin_addr) != 1 ||
                    bind(listen_fd, reinterpret_cast<sockaddr *>(&address),
                         sizeof(address)) != 0 ||
                    listen(listen_fd, 1) != 0) {
                        close(listen_fd);
                        throw std::runtime_error("TCP barrier listen failed");
                }
                fd_ = accept(listen_fd, nullptr, nullptr);
                close(listen_fd);
                if (fd_ < 0)
                        throw std::runtime_error("TCP barrier accept failed");
        }

        void connect_to_peer(const std::string &sync_ip, uint16_t port)
        {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(120);
                while (std::chrono::steady_clock::now() < deadline) {
                        const int candidate = socket(AF_INET, SOCK_STREAM, 0);
                        if (candidate < 0)
                                throw std::runtime_error("socket() failed");
                        sockaddr_in address{};
                        address.sin_family = AF_INET;
                        address.sin_port = htons(port);
                        if (inet_pton(AF_INET, sync_ip.c_str(),
                                      &address.sin_addr) != 1) {
                                close(candidate);
                                throw std::runtime_error("invalid sync IP");
                        }
                        if (connect(candidate,
                                    reinterpret_cast<sockaddr *>(&address),
                                    sizeof(address)) == 0) {
                                fd_ = candidate;
                                return;
                        }
                        close(candidate);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                throw std::runtime_error("TCP barrier connect timed out");
        }

        void write_all(const void *data, std::size_t size)
        {
                const char *cursor = static_cast<const char *>(data);
                while (size != 0) {
                        const ssize_t written = send(fd_, cursor, size, 0);
                        if (written <= 0)
                                throw std::runtime_error("TCP barrier send failed");
                        cursor += written;
                        size -= static_cast<std::size_t>(written);
                }
        }

        void read_all(void *data, std::size_t size)
        {
                char *cursor = static_cast<char *>(data);
                while (size != 0) {
                        const ssize_t received = recv(fd_, cursor, size, 0);
                        if (received <= 0)
                                throw std::runtime_error("TCP barrier receive failed");
                        cursor += received;
                        size -= static_cast<std::size_t>(received);
                }
        }

        int fd_ = -1;
};

std::string argument(int argc, char **argv, const std::string &name,
                     const std::string &fallback = "")
{
        const std::string prefix = "--" + name + "=";
        for (int i = 1; i < argc; ++i) {
                const std::string current(argv[i]);
                if (current.compare(0, prefix.size(), prefix) == 0)
                        return current.substr(prefix.size());
        }
        return fallback;
}

void require(bool condition, const std::string &message)
{
        if (!condition)
                throw std::runtime_error(message);
}

uint64_t message_checksum(uint64_t source, uint64_t sequence)
{
        return 0x9e3779b97f4a7c15ULL ^ (source << 32) ^ sequence;
}

} // namespace

int main(int argc, char **argv)
{
        try {
                const uint32_t id =
                        static_cast<uint32_t>(std::stoul(argument(argc, argv, "id")));
                require(id < 2, "--id must be 0 or 1");
                const std::string sync_ip = argument(argc, argv, "sync-ip");
                require(!sync_ip.empty(), "--sync-ip is required");
                const uint16_t port = static_cast<uint16_t>(std::stoul(
                        argument(argc, argv, "port", "18951")));
                const uint64_t iterations = std::stoull(
                        argument(argc, argv, "iterations", "10000"));

                star::Context context;
                context.coordinator_id = id;
                context.coordinator_num = 2;
                context.shared_memory_backend = "ub";
                context.ub_memory_mode = argument(argc, argv, "mode", "one-sided");
                context.ub_region_prefix = argument(argc, argv, "prefix");
                require(!context.ub_region_prefix.empty(), "--prefix is required");
                context.ub_region_size = std::stoull(
                        argument(argc, argv, "region-mb", "64")) * 1024ULL * 1024ULL;
                context.ub_provider_host = argument(argc, argv, "provider-host");
                const std::string provider_numa =
                        argument(argc, argv, "provider-numa");
                if (!provider_numa.empty()) {
                        const unsigned long parsed = std::stoul(provider_numa);
                        require(parsed <= std::numeric_limits<uint32_t>::max(),
                                "--provider-numa is out of range");
                        context.ub_provider_numa = static_cast<uint32_t>(parsed);
                }
                context.ub_map_timeout_seconds = 120;

                star::ub_memory.initialize(context);
                star::UBBTreeCatalog::initialize(id);
                TcpBarrier barrier(id, sync_ip, port);
                barrier.sync();

                constexpr uint32_t table_id = 900;
                constexpr uint32_t partition_id = 0;
                TestTree::Descriptor *descriptor = nullptr;
                if (id == 0) {
                        descriptor = TestTree::create(0, table_id, partition_id);
                }
                barrier.sync();
                if (descriptor == nullptr)
                        descriptor = TestTree::find(0, table_id, partition_id);
                require(descriptor != nullptr, "shared UB table was not published");
                TestTree table(descriptor);

                const TestKey counter_key{ 1 };
                if (id == 0) {
                        const TestValue initial{ 100, 0x1111 };
                        require(table.insert(counter_key, initial, 1),
                                "initial insert failed");
                }
                barrier.sync();

                TestTree::TupleRef counter = table.search(counter_key);
                require(static_cast<bool>(counter), "counter tuple not found");
                for (uint64_t i = 0; i < iterations; ++i) {
                        while (!counter.header->lock.try_lock())
                                std::this_thread::yield();
                        require(counter.header->valid.load(std::memory_order_acquire) == 1,
                                "counter tuple became invalid");
                        ++counter.value->counter;
                        counter.header->version.fetch_add(1, std::memory_order_relaxed);
                        counter.header->lock.unlock();
                }
                barrier.sync();
                while (!counter.header->lock.try_lock_shared())
                        std::this_thread::yield();
                require(counter.value->counter ==
                                100 + iterations * 2,
                        "cross-node writer-lock result mismatch");
                counter.header->lock.unlock_shared();

                const TestKey lifecycle_key{ 2 };
                const TestValue lifecycle_value{ 7, 0x2222 };
                TestTree::TupleNode *lifecycle_node = nullptr;
                if (id == 0) {
                        require(table.insert(lifecycle_key, lifecycle_value, 2),
                                "placeholder insert failed");
                        require(!table.search(lifecycle_key),
                                "placeholder became reader-visible");
                        lifecycle_node = table.search_including_invalid(
                                lifecycle_key).node;
                }
                barrier.sync();
                require(!table.search(lifecycle_key),
                        "remote requester observed an uncommitted placeholder");
                if (id == 1)
                        require(!table.insert(lifecycle_key, lifecycle_value, 2),
                                "conflicting placeholder unexpectedly succeeded");
                barrier.sync();
                if (id == 0) {
                        auto placeholder = table.search_including_invalid(
                                lifecycle_key);
                        placeholder.header->valid.store(1, std::memory_order_release);
                        placeholder.header->version.fetch_add(1,
                                                              std::memory_order_relaxed);
                }
                barrier.sync();
                require(static_cast<bool>(table.search(lifecycle_key)),
                        "committed placeholder is not visible");

                if (id == 1) {
                        auto victim = table.search(lifecycle_key);
                        while (!victim.header->lock.try_lock())
                                std::this_thread::yield();
                        victim.header->valid.store(0, std::memory_order_release);
                        victim.header->version.fetch_add(1,
                                                         std::memory_order_relaxed);
                        victim.header->lock.unlock();
                }
                barrier.sync();
                require(!table.search(lifecycle_key),
                        "deleted tuple remains visible");
                if (id == 0) {
                        require(table.insert(lifecycle_key, lifecycle_value, 1),
                                "same-key tombstone reuse failed");
                        auto resurrected = table.search(lifecycle_key);
                        require(resurrected.node == lifecycle_node,
                                "same-key tombstone was not reused");
                }
                barrier.sync();

                if (id == 0) {
                        const TestKey aborted_key{ 3 };
                        const TestKey replacement_key{ 4 };
                        require(table.insert(aborted_key, lifecycle_value, 2),
                                "abort placeholder insert failed");
                        require(table.discard_placeholder(aborted_key),
                                "placeholder discard failed");
                        require(table.insert(replacement_key, lifecycle_value, 1),
                                "replacement insert failed");
                        require(!table.search(aborted_key),
                                "discarded placeholder became visible");
                }
                barrier.sync();

                // Both requesters insert into the owner tree concurrently. This
                // forces leaf, inner and root splits with the default fanout.
                constexpr uint64_t split_keys_per_node = 48;
                for (uint64_t i = 0; i < split_keys_per_node; ++i) {
                        const TestKey key{100 + id * split_keys_per_node + i};
                        const TestValue value{key.value, key.value ^ 0x55aa};
                        require(table.insert(key, value, 1),
                                "concurrent split insert failed");
                }
                barrier.sync();
                uint64_t previous = 0;
                uint64_t scanned = 0;
                table.scan(TestKey{100}, [&](const TestTree::TupleRef &ref) {
                        require(scanned == 0 || ref.key->value > previous,
                                "B+ Tree scan order is not strictly increasing");
                        previous = ref.key->value;
                        ++scanned;
                        return false;
                });
                require(scanned == split_keys_per_node * 2,
                        "B+ Tree split scan count mismatch");
                require(descriptor->leaf_count.load(std::memory_order_acquire) > 1,
                        "B+ Tree leaf split was not exercised");
                require(descriptor->inner_count.load(std::memory_order_acquire) > 0,
                        "B+ Tree root split was not exercised");
                barrier.sync();

                // A range reader protects the first key after the range (or
                // the stable +infinity gap). A concurrent requester-side
                // insert must fail while that protection lock is held, then
                // succeed after it is released.
                TestTree::TupleRef protected_gap;
                if (id == 0) {
                        require(table.scan_range(
                                        TestKey{190}, TestKey{195}, 0,
                                        [&](const TestTree::TupleRef &ref,
                                            bool protection_row) {
                                                if (!protection_row)
                                                        return true;
                                                require(ref.header->lock.try_lock_shared(),
                                                        "gap shared lock failed");
                                                protected_gap = ref;
                                                return true;
                                        }),
                                "range scan with gap protection failed");
                }
                barrier.sync();
                const TestKey phantom_key{196};
                const TestValue phantom_value{196, 196 ^ 0x55aa};
                if (id == 1) {
                        require(!table.insert_lock_next(
                                        phantom_key, phantom_value,
                                        [](const TestTree::TupleRef &next) {
                                                return next.header->lock.try_lock();
                                        },
                                        1),
                                "phantom insert bypassed the protected end gap");
                }
                barrier.sync();
                if (id == 0)
                        protected_gap.header->lock.unlock_shared();
                barrier.sync();
                if (id == 1) {
                        TestTree::TupleRef locked_successor;
                        require(table.insert_lock_next(
                                        phantom_key, phantom_value,
                                        [&](const TestTree::TupleRef &next) {
                                                if (!next.header->lock.try_lock())
                                                        return false;
                                                locked_successor = next;
                                                return true;
                                        },
                                        1),
                                "phantom insert did not succeed after gap release");
                        locked_successor.header->lock.unlock();
                }
                barrier.sync();
                require(static_cast<bool>(table.search(phantom_key)),
                        "phantom-test insert is not visible on both nodes");
                barrier.sync();

                const star::UBGlobalPtr inbox_root = star::UBMPSCRingBuffer::create(
                        id, 64, sizeof(TestMessage));
                star::ub_memory.publish_root(
                        id, star::UBMPSCRingBuffer::inbox_root_slot, inbox_root);
                star::UBMPSCRingBuffer local_inbox(inbox_root);
                for (uint64_t i = 0; i < 64; ++i) {
                        const TestMessage message{ id, i, message_checksum(id, i) };
                        require(local_inbox.try_enqueue(&message, sizeof(message)),
                                "local queue filled too early");
                }
                const TestMessage overflow{ id, 64, message_checksum(id, 64) };
                require(!local_inbox.try_enqueue(&overflow, sizeof(overflow)),
                        "full queue did not apply backpressure");
                for (uint64_t i = 0; i < 64; ++i) {
                        TestMessage message{};
                        require(local_inbox.try_dequeue(&message, sizeof(message)) ==
                                        sizeof(message),
                                "local queue dequeue failed");
                        require(message.sequence == i &&
                                        message.checksum == message_checksum(id, i),
                                "local queue payload mismatch");
                }
                barrier.sync();

                const uint32_t peer = 1 - id;
                star::UBMPSCRingBuffer remote_inbox(star::ub_memory.read_root(
                        peer, star::UBMPSCRingBuffer::inbox_root_slot));
                for (uint64_t i = 0; i < iterations; ++i) {
                        const TestMessage outgoing{ id, i, message_checksum(id, i) };
                        remote_inbox.enqueue(&outgoing, sizeof(outgoing));
                        TestMessage incoming{};
                        while (local_inbox.try_dequeue(&incoming, sizeof(incoming)) == 0)
                                std::this_thread::yield();
                        require(incoming.source == peer && incoming.sequence == i &&
                                        incoming.checksum == message_checksum(peer, i),
                                "cross-node queue payload mismatch");
                }

                barrier.sync();
                star::ub_memory.unmap_imported_regions();
                barrier.sync();
                star::ub_memory.shutdown(true);
                std::cout << "PASS id=" << id << " mode="
                          << context.ub_memory_mode << " iterations=" << iterations
                          << std::endl;
                return 0;
        } catch (const std::exception &error) {
                std::cerr << "FAIL: " << error.what() << std::endl;
                return 1;
        }
}
