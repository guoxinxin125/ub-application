#pragma once
#include <cstdint>
#include <cstring>

// POD representation of Post for zero-copy CXL transfer
// Keep sizes conservative; adjust if needed.
constexpr size_t SN_USERNAME_LEN = 64;
constexpr size_t SN_TEXT_LEN = 2048;
constexpr size_t SN_MEDIA_TYPE_LEN = 16;
constexpr size_t SN_MAX_MEDIA = 8;
constexpr size_t SN_MAX_MENTIONS = 8;

struct PostData {
    int64_t post_id;
    int64_t req_id;
    int64_t timestamp;
    int32_t post_type;

    // creator
    int64_t creator_user_id;
    char creator_username[SN_USERNAME_LEN];

    // text
    char text[SN_TEXT_LEN];

    // media
    uint32_t media_count;
    int64_t media_ids[SN_MAX_MEDIA];
    char media_types[SN_MAX_MEDIA][SN_MEDIA_TYPE_LEN];

    // mentions
    uint32_t mentions_count;
    int64_t mentions_ids[SN_MAX_MENTIONS];

    // zero-init convenience
    void init() {
        post_id = 0; req_id = 0; timestamp = 0; post_type = 0;
        creator_user_id = 0; creator_username[0] = '\0';
        text[0] = '\0'; media_count = 0; mentions_count = 0;
        memset(media_ids, 0, sizeof(media_ids));
        for (size_t i = 0; i < SN_MAX_MEDIA; i++) media_types[i][0] = '\0';
        memset(mentions_ids, 0, sizeof(mentions_ids));
    }
};
