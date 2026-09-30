#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace sn_post_copy {

enum class Mode { kMemcpy, kScalar };

inline Mode configured_mode() {
#ifdef ERPC_UB
  static const Mode mode = [] {
    const char *value = std::getenv("ERPC_SN_STORAGE_WRITE_COPY_MODE");
    if (value == nullptr || std::strcmp(value, "memcpy") == 0) {
      std::fprintf(stderr, "SN_UB_STORAGE_WRITE_COPY_MODE=memcpy\n");
      return Mode::kMemcpy;
    }
    if (std::strcmp(value, "scalar") == 0) {
      std::fprintf(stderr, "SN_UB_STORAGE_WRITE_COPY_MODE=scalar\n");
      return Mode::kScalar;
    }
    throw std::invalid_argument(
        "ERPC_SN_STORAGE_WRITE_COPY_MODE must be memcpy or scalar");
  }();
  return mode;
#else
  return Mode::kMemcpy;
#endif
}

// RPCMsgReq is packed, so its PostData payload need not start on an 8-byte
// boundary. Align the remote source before issuing individual word loads.
// The local destination may remain unaligned; memcpy of one word is defined
// for every alignment and normally lowers to one scalar store.
inline void scalar_copy(void *destination, const void *source, size_t bytes) {
  static_assert(sizeof(uint64_t) == 8, "scalar copy needs 8-byte words");
  auto *dst = static_cast<unsigned char *>(destination);
  const auto *src = static_cast<const unsigned char *>(source);
  const size_t alignment = reinterpret_cast<uintptr_t>(src) & 7;
  size_t offset = alignment == 0 ? 0 : 8 - alignment;
  if (offset > bytes) offset = bytes;

  for (size_t i = 0; i < offset; ++i) dst[i] = src[i];
  for (; bytes - offset >= sizeof(uint64_t); offset += sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, src + offset, sizeof(word));
#if defined(__GNUC__) || defined(__clang__)
    // Prevent the compiler from folding the loop back into a wide memcpy.
    __asm__ __volatile__("" : "+r"(word) : : "memory");
#endif
    std::memcpy(dst + offset, &word, sizeof(word));
  }
  for (; offset < bytes; ++offset) dst[offset] = src[offset];
}

inline void copy(void *destination, const void *source, size_t bytes,
                 Mode mode) {
  if (mode == Mode::kScalar)
    scalar_copy(destination, source, bytes);
  else
    std::memcpy(destination, source, bytes);
}

}  // namespace sn_post_copy
