#pragma once

#include <thread>

namespace star
{

inline void ub_cpu_relax()
{
#if defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#else
        std::this_thread::yield();
#endif
}

} // namespace star
