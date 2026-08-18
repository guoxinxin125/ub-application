#pragma once

#ifdef ERPC_UB

#include <type_traits>

namespace erpc {
namespace ub_atomic {

enum class MemoryOrder : int {
  kRelaxed = __ATOMIC_RELAXED,
  kAcquire = __ATOMIC_ACQUIRE,
  kRelease = __ATOMIC_RELEASE,
  kAcqRel = __ATOMIC_ACQ_REL,
  kSeqCst = __ATOMIC_SEQ_CST,
};

template <typename T>
inline T load(const volatile T *address,
              MemoryOrder order = MemoryOrder::kAcquire) {
  static_assert(std::is_integral<T>::value || std::is_pointer<T>::value,
                "UB atomics require an integral or pointer type");
  return __atomic_load_n(address, static_cast<int>(order));
}

template <typename T>
inline void store(volatile T *address, T value,
                  MemoryOrder order = MemoryOrder::kRelease) {
  static_assert(std::is_integral<T>::value || std::is_pointer<T>::value,
                "UB atomics require an integral or pointer type");
  __atomic_store_n(address, value, static_cast<int>(order));
}

template <typename T>
inline T fetch_add(volatile T *address, T increment,
                   MemoryOrder order = MemoryOrder::kAcqRel) {
  static_assert(std::is_integral<T>::value,
                "UB fetch_add requires an integral type");
  return __atomic_fetch_add(address, increment, static_cast<int>(order));
}

template <typename T>
inline T fetch_sub(volatile T *address, T decrement,
                   MemoryOrder order = MemoryOrder::kAcqRel) {
  static_assert(std::is_integral<T>::value,
                "UB fetch_sub requires an integral type");
  return __atomic_fetch_sub(address, decrement, static_cast<int>(order));
}

template <typename T>
inline bool compare_exchange(volatile T *address, T *expected, T desired,
                             bool weak = true,
                             MemoryOrder success = MemoryOrder::kAcqRel,
                             MemoryOrder failure = MemoryOrder::kAcquire) {
  static_assert(std::is_integral<T>::value || std::is_pointer<T>::value,
                "UB compare_exchange requires an integral or pointer type");
  return __atomic_compare_exchange_n(address, expected, desired, weak,
                                     static_cast<int>(success),
                                     static_cast<int>(failure));
}

inline void fence(MemoryOrder order) {
  __atomic_thread_fence(static_cast<int>(order));
}

inline void compiler_fence() { __asm__ __volatile__("" ::: "memory"); }

}  // namespace ub_atomic
}  // namespace erpc

#endif  // ERPC_UB
