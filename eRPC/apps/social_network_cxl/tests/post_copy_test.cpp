#include "../post_copy.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../social_network_rpc_type.h"

int main() {
  static_assert(sizeof(PostData) == 2048, "PostData layout changed");
  std::array<unsigned char, sizeof(PostData) + 16> source{};
  std::array<unsigned char, sizeof(PostData) + 16> actual{};
  std::array<unsigned char, sizeof(PostData) + 16> expected{};
  for (size_t i = 0; i < source.size(); ++i)
    source[i] = static_cast<unsigned char>((i * 37 + 11) & 255);

  for (size_t src_offset = 0; src_offset < 8; ++src_offset) {
    for (size_t dst_offset = 0; dst_offset < 8; ++dst_offset) {
      for (size_t bytes : {size_t{0}, size_t{1}, size_t{7}, size_t{8},
                           size_t{9}, sizeof(PostData)}) {
        actual.fill(0xa5);
        expected.fill(0xa5);
        sn_post_copy::copy(actual.data() + dst_offset,
                           source.data() + src_offset, bytes,
                           sn_post_copy::Mode::kScalar);
        std::memcpy(expected.data() + dst_offset, source.data() + src_offset,
                    bytes);
        assert(actual == expected);
      }
    }
  }

  PostStorageWriteCXLReq wire_request{};
  std::memcpy(&wire_request.post, source.data(), sizeof(PostData));
  RPCMsgReq<PostStorageWriteCXLReq> packed_request(
      RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ, 1, wire_request);
  PostData copied_post{};
  sn_post_copy::scalar_copy(&copied_post, &packed_request.req_control.post,
                            sizeof(PostData));
  assert(std::memcmp(&copied_post, &wire_request.post, sizeof(PostData)) == 0);

#ifdef ERPC_UB
  const char *mode = std::getenv("ERPC_SN_STORAGE_WRITE_COPY_MODE");
  if (mode != nullptr && std::strcmp(mode, "scalar") == 0)
    assert(sn_post_copy::configured_mode() == sn_post_copy::Mode::kScalar);
  else
    assert(sn_post_copy::configured_mode() == sn_post_copy::Mode::kMemcpy);
#else
  assert(sn_post_copy::configured_mode() == sn_post_copy::Mode::kMemcpy);
#endif
  std::cout << "post_copy_test PASS\n";
}
