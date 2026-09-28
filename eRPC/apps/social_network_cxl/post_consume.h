#pragma once
#include <cstdio>
#include <cstdlib>

#include "post_checksum.h"
#include "post_data.h"
#include "ub_breakdown.h"

namespace sn_consume {

// Mirrors the contiguous metadata prefix. One logical metadata read provides
// all lengths and counts; payload locations remain compile-time constants.
struct PostHeader {
  uint32_t version, size;
  int64_t post_id, req_id, timestamp, creator_id;
  int32_t type;
  uint16_t username_length, text_length;
  uint16_t media_count, mentions_count, urls_count;
  uint16_t media_type_lengths[SN_MAX_MEDIA];
  uint16_t mention_name_lengths[SN_MAX_MENTIONS];
  uint16_t shortened_url_lengths[SN_MAX_URLS];
  uint16_t expanded_url_lengths[SN_MAX_URLS];
};
static_assert(sizeof(PostHeader) == offsetof(PostData, creator_username),
              "consumer metadata layout mismatch");
static_assert(sizeof(PostHeader) % sizeof(uint64_t) == 0,
              "contiguous scalar reads require an 8-byte header boundary");
static_assert(alignof(PostData) >= alignof(uint64_t),
              "contiguous scalar reads require an 8-byte PostData base");

template <class T>
inline T native_load(const unsigned char *p) {
  T value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline void validate_header(const PostHeader &h) {
  if (h.version != SN_POST_LAYOUT_VERSION || h.size != sizeof(PostData) ||
      h.username_length >= SN_USERNAME_LEN || h.text_length >= SN_TEXT_LEN ||
      h.media_count > SN_MAX_MEDIA || h.mentions_count > SN_MAX_MENTIONS ||
      h.urls_count > SN_MAX_URLS)
    throw std::invalid_argument("PostData: invalid shared metadata");
}

inline void validate_active_lengths(const PostHeader &h) {
  for (size_t i = 0; i < h.media_count; ++i)
    if (h.media_type_lengths[i] >= SN_MEDIA_TYPE_LEN)
      throw std::invalid_argument("PostData: invalid shared media type length");
  for (size_t i = 0; i < h.mentions_count; ++i)
    if (h.mention_name_lengths[i] >= SN_USERNAME_LEN)
      throw std::invalid_argument(
          "PostData: invalid shared mention username length");
  for (size_t i = 0; i < h.urls_count; ++i)
    if (h.shortened_url_lengths[i] >= SN_SHORT_URL_LEN ||
        h.expanded_url_lengths[i] >= SN_EXPANDED_URL_LEN)
      throw std::invalid_argument("PostData: invalid shared URL length");
}

inline PostHeader load_header(const unsigned char *p) {
  const PostHeader header = native_load<PostHeader>(p);
#if defined(__GNUC__) || defined(__clang__)
  // Keep the complete 104-byte header materialized even when the compiler can
  // prove that a particular request does not use every length slot.
  __asm__ __volatile__("" : : "m"(header) : "memory");
#endif
  return header;
}

// Keep contiguous UB reads as individual 64-bit loads. Unlike the 8-byte
// memcpy used by the semantic fields path, these instructions cannot be
// combined into vector loads by an optimizing compiler. The imported PostData
// object and its 104-byte header boundary are both 8-byte aligned.
inline uint64_t contiguous_scalar_load(const unsigned char *p) {
#if defined(ERPC_UB) && defined(__aarch64__)
  uint64_t value;
  __asm__ __volatile__("ldr %0, [%1]" : "=r"(value) : "r"(p) : "memory");
  return value;
#elif defined(ERPC_UB) && defined(__x86_64__)
  uint64_t value;
  __asm__ __volatile__("movq (%1), %0" : "=r"(value) : "r"(p) : "memory");
  return value;
#else
  return load_le64(p);
#endif
}

inline uint64_t contiguous_scalar_checksum(const unsigned char *p,
                                           size_t size) {
  uint64_t lane0 = 0, lane1 = 0, lane2 = 0, lane3 = 0;
  uint64_t lane4 = 0, lane5 = 0, lane6 = 0, lane7 = 0;
  while (size >= 64) {
    lane0 += contiguous_scalar_load(p);
    lane1 += contiguous_scalar_load(p + 8);
    lane2 += contiguous_scalar_load(p + 16);
    lane3 += contiguous_scalar_load(p + 24);
    lane4 += contiguous_scalar_load(p + 32);
    lane5 += contiguous_scalar_load(p + 40);
    lane6 += contiguous_scalar_load(p + 48);
    lane7 += contiguous_scalar_load(p + 56);
    p += 64;
    size -= 64;
  }

  Checksum sum;
  sum.integer(lane0);
  sum.integer(lane1);
  sum.integer(lane2);
  sum.integer(lane3);
  sum.integer(lane4);
  sum.integer(lane5);
  sum.integer(lane6);
  sum.integer(lane7);
  while (size >= 8) {
    sum.integer(contiguous_scalar_load(p));
    p += 8;
    size -= 8;
  }
  uint64_t tail = 0;
  for (size_t i = 0; i < size; ++i)
    tail |= static_cast<uint64_t>(p[i]) << (8 * i);
  sum.integer(tail);
  return sum.value();
}

inline PostHeader checked_header(const void *data, size_t size) {
  if (data == nullptr || size != sizeof(PostData))
    throw std::invalid_argument("PostData: invalid shared object size");

  const auto *p = static_cast<const unsigned char *>(data);
  const uint64_t metadata_start = sn_profile::start();
  const PostHeader h = load_header(p);
  validate_header(h);
  sn_profile::record(sn_profile::Stage::kClientMetadata, metadata_start);
  return h;
}

inline uint64_t native_post_fields(const void *data, size_t size) {
  const auto *p = static_cast<const unsigned char *>(data);
  const PostHeader h = checked_header(data, size);

  const uint64_t fields_start = sn_profile::start();
  Checksum sum;
  uint64_t phase_start = sn_profile::start();
  sum.integer(static_cast<uint64_t>(h.post_id));
  sum.integer(static_cast<uint64_t>(h.req_id));
  sum.integer(static_cast<uint64_t>(h.timestamp));
  sum.integer(static_cast<uint64_t>(static_cast<int64_t>(h.type)));
  sum.integer(static_cast<uint64_t>(h.creator_id));
  sn_profile::record(sn_profile::Stage::kClientFieldsScalars, phase_start);

  phase_start = sn_profile::start();
  sum.bytes(p + offsetof(PostData, creator_username), h.username_length);
  sn_profile::record(sn_profile::Stage::kClientFieldsUsername, phase_start);

  phase_start = sn_profile::start();
  sum.bytes(p + offsetof(PostData, text), h.text_length);
  sn_profile::record(sn_profile::Stage::kClientFieldsText, phase_start);

  phase_start = sn_profile::start();
  sum.integer(h.media_count);
  for (size_t i = 0; i < h.media_count; ++i) {
    const uint16_t length = h.media_type_lengths[i];
    if (length >= SN_MEDIA_TYPE_LEN)
      throw std::invalid_argument("PostData: invalid shared media type length");
    sum.integer(static_cast<uint64_t>(native_load<int64_t>(
        p + offsetof(PostData, media_ids) + i * sizeof(int64_t))));
    sum.bytes(p + offsetof(PostData, media_types) + i * SN_MEDIA_TYPE_LEN,
              length);
  }
  sn_profile::record(sn_profile::Stage::kClientFieldsMedia, phase_start);

  phase_start = sn_profile::start();
  sum.integer(h.mentions_count);
  for (size_t i = 0; i < h.mentions_count; ++i) {
    const uint16_t length = h.mention_name_lengths[i];
    if (length >= SN_USERNAME_LEN)
      throw std::invalid_argument(
          "PostData: invalid shared mention username length");
    sum.integer(static_cast<uint64_t>(native_load<int64_t>(
        p + offsetof(PostData, mentions_ids) + i * sizeof(int64_t))));
    sum.bytes(p + offsetof(PostData, mentions_usernames) + i * SN_USERNAME_LEN,
              length);
  }
  sn_profile::record(sn_profile::Stage::kClientFieldsMentions, phase_start);

  phase_start = sn_profile::start();
  sum.integer(h.urls_count);
  for (size_t i = 0; i < h.urls_count; ++i) {
    if (h.shortened_url_lengths[i] >= SN_SHORT_URL_LEN ||
        h.expanded_url_lengths[i] >= SN_EXPANDED_URL_LEN)
      throw std::invalid_argument("PostData: invalid shared URL length");
    sum.bytes(p + offsetof(PostData, shortened_urls) + i * SN_SHORT_URL_LEN,
              h.shortened_url_lengths[i]);
    sum.bytes(p + offsetof(PostData, expanded_urls) + i * SN_EXPANDED_URL_LEN,
              h.expanded_url_lengths[i]);
  }
  sn_profile::record(sn_profile::Stage::kClientFieldsUrls, phase_start);

  const uint64_t value = sum.value();
  sn_profile::record(sn_profile::Stage::kClientFields, fields_start);
  return value;
}

inline uint64_t native_post_contiguous(const void *data, size_t size) {
  const auto *p = static_cast<const unsigned char *>(data);
  const PostHeader h = checked_header(data, size);

  const uint64_t fields_start = sn_profile::start();
  validate_active_lengths(h);
  const uint64_t contiguous_start = sn_profile::start();
  const uint64_t remainder = contiguous_scalar_checksum(
      p + sizeof(PostHeader), size - sizeof(PostHeader));
  sn_profile::record(sn_profile::Stage::kClientFieldsContiguous,
                     contiguous_start);

  // Fold locally materialized header values into the sink. The header's full
  // byte representation was already forced by load_header().
  Checksum sum;
  sum.integer(static_cast<uint64_t>(h.post_id));
  sum.integer(static_cast<uint64_t>(h.req_id));
  sum.integer(static_cast<uint64_t>(h.timestamp));
  sum.integer(static_cast<uint64_t>(static_cast<int64_t>(h.type)));
  sum.integer(static_cast<uint64_t>(h.creator_id));
  sum.integer(remainder);
  const uint64_t value = sum.value();
  sn_profile::record(sn_profile::Stage::kClientFields, fields_start);
  return value;
}

enum class PostReadMode { kFields, kContiguous };

inline PostReadMode configured_post_read_mode() {
#ifdef ERPC_UB
  static const PostReadMode mode = [] {
    const char *value = std::getenv("ERPC_SN_POST_READ_MODE");
    if (value == nullptr || std::strcmp(value, "fields") == 0) {
      std::fprintf(stderr, "SN_UB_POST_READ_MODE=fields\n");
      return PostReadMode::kFields;
    }
    if (std::strcmp(value, "contiguous") == 0) {
      std::fprintf(stderr, "SN_UB_POST_READ_MODE=contiguous\n");
      return PostReadMode::kContiguous;
    }
    throw std::invalid_argument(
        "ERPC_SN_POST_READ_MODE must be fields or contiguous");
  }();
  return mode;
#else
  return PostReadMode::kFields;
#endif
}

inline uint64_t native_post(const void *data, size_t size) {
  if (configured_post_read_mode() == PostReadMode::kContiguous)
    return native_post_contiguous(data, size);
  return native_post_fields(data, size);
}
}  // namespace sn_consume
