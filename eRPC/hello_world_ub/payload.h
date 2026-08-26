#pragma once

#include <cstddef>
#include <cstdint>

static constexpr size_t kUBHelloPayloadHeaderSize = 2 * sizeof(uint64_t);
static constexpr uint8_t kUBHelloResponseXor = 0xa5;

struct UBHelloPayloadIdentity {
  uint64_t request_id;
  uint64_t client_id;
};

inline uint8_t ub_hello_u64_byte(uint64_t value, size_t byte_index) {
  return static_cast<uint8_t>((value >> (byte_index * 8)) & 0xff);
}

inline uint8_t ub_hello_request_byte(uint64_t client_id, uint64_t request_id,
                                     size_t offset) {
  if (offset < sizeof(uint64_t)) {
    return ub_hello_u64_byte(request_id, offset);
  }
  if (offset < kUBHelloPayloadHeaderSize) {
    return ub_hello_u64_byte(client_id, offset - sizeof(uint64_t));
  }

  const uint64_t mixed = request_id * 131 + client_id * 17 + offset * 29;
  return static_cast<uint8_t>((mixed ^ (mixed >> 11) ^ (mixed >> 23)) & 0xff);
}

inline void ub_hello_fill_request(uint8_t *payload, size_t size,
                                  uint64_t client_id, uint64_t request_id) {
  for (size_t offset = 0; offset < size; ++offset) {
    payload[offset] = ub_hello_request_byte(client_id, request_id, offset);
  }
}

inline uint64_t ub_hello_load_u64(const uint8_t *payload) {
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    value |= static_cast<uint64_t>(payload[i]) << (i * 8);
  }
  return value;
}

inline UBHelloPayloadIdentity ub_hello_payload_identity(
    const uint8_t *payload) {
  return {ub_hello_load_u64(payload),
          ub_hello_load_u64(payload + sizeof(uint64_t))};
}

inline bool ub_hello_make_response(const uint8_t *request, uint8_t *response,
                                   size_t size,
                                   UBHelloPayloadIdentity *identity) {
  *identity = ub_hello_payload_identity(request);
  bool valid = true;
  for (size_t offset = 0; offset < size; ++offset) {
    const uint8_t value = request[offset];
    if (value != ub_hello_request_byte(identity->client_id,
                                       identity->request_id, offset)) {
      valid = false;
    }
    response[offset] = static_cast<uint8_t>(value ^ kUBHelloResponseXor);
  }
  return valid;
}

inline bool ub_hello_validate_response(const uint8_t *response, size_t size,
                                       uint64_t client_id,
                                       uint64_t request_id) {
  for (size_t offset = 0; offset < size; ++offset) {
    const uint8_t expected = static_cast<uint8_t>(
        ub_hello_request_byte(client_id, request_id, offset) ^
        kUBHelloResponseXor);
    if (response[offset] != expected) return false;
  }
  return true;
}
