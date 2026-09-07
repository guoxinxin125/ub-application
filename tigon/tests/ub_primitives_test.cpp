#include "common/UBGlobalPtr.h"
#include "common/UBTuple.h"

#include <cstdint>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{

void require(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

} // namespace

int main()
{
        using star::UBGlobalPtr;
    using star::UBSharedRWLock;
        using star::UBTupleHeader;

        static_assert(sizeof(UBGlobalPtr) == 16, "shared pointer ABI changed");
        static_assert(std::is_trivially_copyable<UBGlobalPtr>::value,
                      "shared pointer must remain trivially copyable");
        UBGlobalPtr null_pointer;
        require(null_pointer.is_null(), "default UBGlobalPtr is not null");
        UBGlobalPtr pointer(1, 7, 4096);
        require(pointer && pointer.region_id == 1 && pointer.generation == 7 &&
                        pointer.offset == 4096,
                "UBGlobalPtr fields changed");

        UBSharedRWLock lock;
        require(lock.try_lock_shared(), "first shared lock failed");
        require(lock.try_lock_shared(), "second shared lock failed");
        require(lock.reader_count() == 2, "shared reader count mismatch");
        require(!lock.try_lock(), "writer acquired a read-locked word");
        lock.unlock_shared();
        lock.unlock_shared();
        require(lock.try_lock(), "writer lock failed");
        require(lock.write_locked(), "writer bit was not published");
        require(!lock.try_lock_shared(), "reader acquired a write-locked word");
        lock.unlock();

        UBTupleHeader placeholder(8, 64, 2);
        require(placeholder.valid.load(std::memory_order_acquire) == 2,
                "placeholder state was not initialized");
        placeholder.valid.store(1, std::memory_order_release);
        require(placeholder.valid.load(std::memory_order_acquire) == 1,
                "placeholder publication failed");

        constexpr uint32_t thread_count = 8;
        constexpr uint32_t iterations = 10000;
        uint64_t protected_value = 0;
        std::vector<std::thread> threads;
        for (uint32_t thread_id = 0; thread_id < thread_count; ++thread_id) {
                threads.emplace_back([&]() {
                        for (uint32_t i = 0; i < iterations; ++i) {
                                while (!lock.try_lock())
                                        std::this_thread::yield();
                                ++protected_value;
                                lock.unlock();
                        }
                });
        }
        for (auto &thread : threads)
                thread.join();
        require(protected_value == static_cast<uint64_t>(thread_count) * iterations,
                "contended writer-lock result mismatch");
        return 0;
}
