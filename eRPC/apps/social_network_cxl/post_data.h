#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

// Fixed-position, fixed-capacity 2 KiB Post layout.
// String lengths exclude the local trailing NUL.
constexpr size_t SN_USERNAME_LEN = 16;
constexpr size_t SN_TEXT_LEN = 1168;
constexpr size_t SN_MEDIA_TYPE_LEN = 4;
constexpr size_t SN_MAX_MEDIA = 10;
constexpr size_t SN_MAX_MENTIONS = 5;
constexpr size_t SN_MAX_URLS = 5;
constexpr size_t SN_SHORT_URL_LEN = 32;
constexpr size_t SN_EXPANDED_URL_LEN = 72;
constexpr uint32_t SN_POST_LAYOUT_VERSION = 4;

// Helper-service response representation; stored PostData uses fixed arrays.
struct PostUrlData {
  uint32_t shortened_length;
  uint32_t expanded_length;
  char shortened[SN_SHORT_URL_LEN];
  char expanded[SN_EXPANDED_URL_LEN];
};

struct PostData {
  uint32_t layout_version;
  uint32_t object_size;
  int64_t post_id;
  int64_t req_id;
  int64_t timestamp;
  int64_t creator_user_id;
  int32_t post_type;
  uint16_t creator_username_length;
  uint16_t text_length;
  uint16_t media_count;
  uint16_t mentions_count;
  uint16_t urls_count;
  uint16_t media_type_lengths[SN_MAX_MEDIA];
  uint16_t mentions_username_lengths[SN_MAX_MENTIONS];
  uint16_t shortened_url_lengths[SN_MAX_URLS];
  uint16_t expanded_url_lengths[SN_MAX_URLS];

  char creator_username[SN_USERNAME_LEN];
  char text[SN_TEXT_LEN];
  int64_t media_ids[SN_MAX_MEDIA];
  char media_types[SN_MAX_MEDIA][SN_MEDIA_TYPE_LEN];
  int64_t mentions_ids[SN_MAX_MENTIONS];
  char mentions_usernames[SN_MAX_MENTIONS][SN_USERNAME_LEN];
  char shortened_urls[SN_MAX_URLS][SN_SHORT_URL_LEN];
  char expanded_urls[SN_MAX_URLS][SN_EXPANDED_URL_LEN];

  void init() {
    std::memset(this, 0, sizeof(*this));
    layout_version = SN_POST_LAYOUT_VERSION;
    object_size = sizeof(*this);
  }
};

template <size_t N>
inline uint16_t set_post_string(char (&target)[N], const std::string &source) {
  if (source.size() >= N)
    throw std::length_error("PostData: string exceeds fixed field capacity");
  std::memcpy(target, source.data(), source.size());
  target[source.size()] = '\0';
  return static_cast<uint16_t>(source.size());
}

template <size_t N>
inline std::string local_post_string(const char (&source)[N], uint16_t length) {
  if (length >= N)
    throw std::invalid_argument("PostData: invalid string length");
  return std::string(source, length);
}

inline void validate_post_data(const PostData &p) {
  if (p.layout_version != SN_POST_LAYOUT_VERSION ||
      p.object_size != sizeof(PostData) ||
      p.creator_username_length >= SN_USERNAME_LEN ||
      p.text_length >= SN_TEXT_LEN || p.media_count > SN_MAX_MEDIA ||
      p.mentions_count > SN_MAX_MENTIONS || p.urls_count > SN_MAX_URLS)
    throw std::invalid_argument("PostData: invalid layout, length or count");
  for (size_t i = 0; i < p.media_count; ++i)
    if (p.media_type_lengths[i] >= SN_MEDIA_TYPE_LEN)
      throw std::invalid_argument("PostData: invalid media type length");
  for (size_t i = 0; i < p.mentions_count; ++i)
    if (p.mentions_username_lengths[i] >= SN_USERNAME_LEN)
      throw std::invalid_argument("PostData: invalid mention username length");
  for (size_t i = 0; i < p.urls_count; ++i)
    if (p.shortened_url_lengths[i] >= SN_SHORT_URL_LEN ||
        p.expanded_url_lengths[i] >= SN_EXPANDED_URL_LEN)
      throw std::invalid_argument("PostData: invalid URL length");
}

static_assert(offsetof(PostData, creator_username) == 104,
              "metadata header ABI changed");
static_assert(offsetof(PostData, text) == 120, "text offset ABI changed");
static_assert(offsetof(PostData, media_ids) == 1288,
              "media ID offset ABI changed");
static_assert(sizeof(PostData) == 2048, "PostData must be exactly 2 KiB");
static_assert(std::is_trivially_copyable<PostData>::value &&
                  std::is_standard_layout<PostData>::value,
              "PostData must remain a native shared-memory layout");
