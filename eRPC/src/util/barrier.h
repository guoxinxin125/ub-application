#pragma once

namespace erpc {

static void memory_barrier() { asm volatile("" ::: "memory"); }

#if defined(__x86_64__) || defined(__i386__)

static void lfence() { asm volatile("lfence" ::: "memory"); }

static void sfence() { asm volatile("sfence" ::: "memory"); }

static void mfence() { asm volatile("mfence" ::: "memory"); }

static void pause() { asm volatile("pause"); }

static void clflush(volatile void* p) { asm volatile("clflush (%0)" ::"r"(p)); }

static void cpuid(unsigned int* eax, unsigned int* ebx, unsigned int* ecx,
                  unsigned int* edx) {
  asm volatile("cpuid"
               : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
               : "0"(*eax), "2"(*ecx));
}

#elif defined(__aarch64__)

static void lfence() { asm volatile("dmb ishld" ::: "memory"); }

static void sfence() { asm volatile("dmb ishst" ::: "memory"); }

static void mfence() { asm volatile("dmb ish" ::: "memory"); }

static void pause() { asm volatile("yield"); }

static void clflush(volatile void* p) {
  // Clean and invalidate the addressed data-cache line to the point of
  // coherency. Linux must permit EL0 cache maintenance for this instruction.
  asm volatile("dc civac, %0" : : "r"(p) : "memory");
  asm volatile("dsb ish" ::: "memory");
}

static void cpuid(unsigned int* eax, unsigned int* ebx, unsigned int* ecx,
                  unsigned int* edx) {
  // AArch64 has no CPUID instruction. Return no optional x86 features to keep
  // callers conservative if this legacy helper is used by common code.
  *eax = 0;
  *ebx = 0;
  *ecx = 0;
  *edx = 0;
}

#else
#error "eRPC barriers are unsupported on this architecture"
#endif

}  // namespace erpc
