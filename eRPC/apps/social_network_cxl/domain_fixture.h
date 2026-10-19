#pragma once
#include "post_data.h"
#include <cstdio>

// Opt-in VM acceptance fixture, not an application policy or timed baseline.
// Even users have one deterministic post; odd users have an empty timeline.
namespace sn_fixture {
constexpr uint32_t kUsers = 962;
constexpr int64_t kPostBase = 100000;
inline uint32_t user(uint32_t request) {
  if (!request) throw std::invalid_argument("fixture request must be positive");
  return (request - 1) % kUsers;
}
inline bool verify(uint32_t request, uint32_t count, const PostData *post) {
  const uint32_t uid = user(request);
  if (uid & 1U) return count == 0 && post == nullptr;
  char expected[48];
  const int length = std::snprintf(expected, sizeof(expected), "social-vm-post-%u", uid);
  return count == 1 && post && post->layout_version == SN_POST_LAYOUT_VERSION &&
      post->object_size == sizeof(PostData) && post->post_id == kPostBase + uid &&
      post->creator_user_id == uid && length > 0 &&
      post->text_length == static_cast<uint16_t>(length) &&
      std::memcmp(post->text, expected, static_cast<size_t>(length)) == 0;
}
}  // namespace sn_fixture
