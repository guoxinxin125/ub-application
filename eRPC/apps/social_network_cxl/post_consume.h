#pragma once
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

template <class T>
inline T native_load(const unsigned char *p) {
  T value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint64_t native_post(const void *data, size_t size) {
  if (data == nullptr || size != sizeof(PostData))
    throw std::invalid_argument("PostData: invalid shared object size");

  const auto *p = static_cast<const unsigned char *>(data);
  const uint64_t metadata_start = sn_profile::start();
  const PostHeader h = native_load<PostHeader>(p);
  if (h.version != SN_POST_LAYOUT_VERSION || h.size != sizeof(PostData) ||
      h.username_length >= SN_USERNAME_LEN || h.text_length >= SN_TEXT_LEN ||
      h.media_count > SN_MAX_MEDIA || h.mentions_count > SN_MAX_MENTIONS ||
      h.urls_count > SN_MAX_URLS)
    throw std::invalid_argument("PostData: invalid shared metadata");
  sn_profile::record(sn_profile::Stage::kClientMetadata, metadata_start);

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
}  // namespace sn_consume
