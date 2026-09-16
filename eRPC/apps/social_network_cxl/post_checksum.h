#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Identical in both applications; a benchmark checksum, not an integrity hash.
namespace sn_consume {
inline uint64_t load_le64(const unsigned char *p) {
  uint64_t value;
  std::memcpy(&value, p, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  value = __builtin_bswap64(value);
#endif
  return value;
}
class Checksum {
 public:
  void integer(uint64_t value) {
    state_ = (state_ ^ value) * UINT64_C(0x9e3779b185ebca87);
    state_ = (state_ << 27) | (state_ >> 37);
  }
  void bytes(const void *data, size_t size) {
    integer(size);
    const auto *p = static_cast<const unsigned char *>(data);
    uint64_t a = 0, b = 0, c = 0, d = 0;
    // Logical 64-byte blocks, not a promise of one remote transaction.
    // Constant-size memcpy loads avoid unaligned/type-aliasing UB.
    while (size >= 64) {
      a += load_le64(p);
      b += load_le64(p + 8);
      c += load_le64(p + 16);
      d += load_le64(p + 24);
      a += load_le64(p + 32);
      b += load_le64(p + 40);
      c += load_le64(p + 48);
      d += load_le64(p + 56);
      p += 64;
      size -= 64;
    }
    integer(a);
    integer(b);
    integer(c);
    integer(d);
    while (size >= 8) {
      integer(load_le64(p));
      p += 8;
      size -= 8;
    }
    uint64_t tail = 0;
    for (size_t i = 0; i < size; ++i)
      tail |= static_cast<uint64_t>(p[i]) << (8 * i);
    integer(tail);
  }
  uint64_t value() const { return state_; }

 private:
  uint64_t state_ = UINT64_C(0x243f6a8885a308d3);
};
inline void retain(uint64_t value) {
  static thread_local volatile uint64_t sink = 0;
  sink = sink + value;
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("" : : "r"(value) : "memory");
#endif
}
// Canonical order and integer values; absent protobuf fields use getter
// defaults.
template <class PostT>
uint64_t protobuf_post(const PostT &post) {
  Checksum sum;
  sum.integer(static_cast<uint64_t>(post.post_id()));
  sum.integer(static_cast<uint64_t>(post.req_id()));
  sum.integer(static_cast<uint64_t>(post.timestamp()));
  sum.integer(static_cast<uint64_t>(static_cast<int64_t>(post.post_type())));
  sum.integer(static_cast<uint64_t>(post.creator().user_id()));
  sum.bytes(post.creator().username().data(), post.creator().username().size());
  sum.bytes(post.text().data(), post.text().size());
  sum.integer(post.media_size());
  for (const auto &m : post.media()) {
    sum.integer(static_cast<uint64_t>(m.media_id()));
    sum.bytes(m.media_type().data(), m.media_type().size());
  }
  sum.integer(post.user_mentions_size());
  for (const auto &m : post.user_mentions()) {
    sum.integer(static_cast<uint64_t>(m.user_id()));
    sum.bytes(m.username().data(), m.username().size());
  }
  sum.integer(post.urls_size());
  for (const auto &u : post.urls()) {
    sum.bytes(u.shortened_url().data(), u.shortened_url().size());
    sum.bytes(u.expanded_url().data(), u.expanded_url().size());
  }
  return sum.value();
}
}  // namespace sn_consume
