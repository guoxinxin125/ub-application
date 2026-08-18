#pragma once

#include "common.h"
#include "pkthdr.h"
#include "util/buffer.h"
#include "util/math_utils.h"

namespace erpc {

// Forward declarations for friendship
class Session;

template <typename T>
class Rpc;

/**
 * @brief Applications store request and response messages in hugepage-backed
 * buffers called message buffers. These buffers are registered with the NIC,
 * allowing fast zero-copy transmission.
 *
 * A message buffer is allocated using Rpc::alloc_msg_buffer. Only its maximum
 * size is specified during allocation. Later, users can resize the message
 * buffer with Rpc::resize_msg_buffer to fit smaller requests or responses.
 * Rpc::free_msg_buffer frees a message buffer.
 *
 * A message buffer is invalid if its #buf pointer is null.
 */
class MsgBuffer {
  friend class CTransport;
  friend class Rpc<CTransport>;
  friend class Session;

 private:
  /// Return a pointer to the pre-appended packet header of this MsgBuffer
  inline pkthdr_t *get_pkthdr_0() const {
#if defined(ERPC_CXL) || defined(ERPC_UB)
    if (pkthdr_0_ != nullptr) return pkthdr_0_;
#endif
    return reinterpret_cast<pkthdr_t *>(buf_ - sizeof(pkthdr_t));
  }

  /// Return a pointer to the nth packet header of this MsgBuffer.
  /// get_pkthdr_0() is more efficient for retrieving the zeroth header.
  inline pkthdr_t *get_pkthdr_n(size_t n) const {
    if (unlikely(n == 0)) return get_pkthdr_0();
    return reinterpret_cast<pkthdr_t *>(
        buf_ + round_up<sizeof(size_t)>(max_data_size_) +
        (n - 1) * sizeof(pkthdr_t));
  }

  std::string get_pkthdr_str(size_t pkt_idx) const {
    return get_pkthdr_n(pkt_idx)->to_string();
  }

  /// Return true iff this MsgBuffer uses a dynamically-allocated MsgBuffer.
  /// This function does not sanity-check other fields.
  inline bool is_dynamic() const { return buffer_.buf_ != nullptr; }

  /// Check if this MsgBuffer is buried
  inline bool is_buried() const {
    return (buf_ == nullptr && buffer_.buf_ == nullptr);
  }

  /// Get the packet size (i.e., including packet header) of a packet
  template <size_t kMaxDataPerPkt>
  inline size_t get_pkt_size(size_t pkt_idx) const {
    if (num_pkts_ == 1) return sizeof(pkthdr_t) + data_size_;
    size_t offset = pkt_idx * kMaxDataPerPkt;
    return sizeof(pkthdr_t) + (std::min)(kMaxDataPerPkt, data_size_ - offset);
  }

  /// Return a string representation of this MsgBuffer
  std::string to_string() const {
    if (buf_ == nullptr) return "[Invalid]";

    std::ostringstream ret;
    ret << "[buf " << static_cast<void *>(buf_) << ", "
        << "buffer " << buffer_.to_string() << ", "
        << "data_size " << data_size_ << "(" << max_data_size_ << "), "
        << "pkts " << num_pkts_ << "(" << max_num_pkts_ << ")]";
    return ret.str();
  }

  /// Construct a MsgBuffer with a dynamic Buffer allocated by eRPC.
  /// The zeroth packet header is stored at \p buffer.buf. \p buffer must have
  /// space for at least \p max_data_bytes, and \p max_num_pkts packet headers.
  MsgBuffer(Buffer buffer, size_t max_data_size, size_t max_num_pkts)
      : buffer_(buffer),
        max_data_size_(max_data_size),
        data_size_(max_data_size),
        max_num_pkts_(max_num_pkts),
        num_pkts_(max_num_pkts),
#if defined(ERPC_CXL) || defined(ERPC_UB)
        pkthdr_0_(nullptr),
#endif
        buf_(buffer.buf_ + sizeof(pkthdr_t)) {
    assert(buffer.buf_ != nullptr);  // buffer must be valid
    // data_size can be 0
    assert(max_num_pkts >= 1);
    assert(buffer.class_size_ >=
           max_data_size + max_num_pkts * sizeof(pkthdr_t));

    pkthdr_t *pkthdr_0 = get_pkthdr_0();
    pkthdr_0->magic_ = kPktHdrMagic;

    // UDP checksum for raw Ethernet. Useless for other transports.
    static_assert(sizeof(pkthdr_t::headroom_) == kHeadroom + 2, "");
    pkthdr_0->headroom_[kHeadroom] = 0;
    pkthdr_0->headroom_[kHeadroom + 1] = 0;
  }

  /// Construct a single-packet "fake" MsgBuffer using a received packet,
  /// setting \p buffer to invalid so that we know not to free it.
  /// \p pkt must have space for \p max_data_bytes and one packet header.
  MsgBuffer(pkthdr_t *pkthdr, size_t max_data_size)
      : max_data_size_(max_data_size),
        data_size_(max_data_size),
        max_num_pkts_(1),
        num_pkts_(1),
#if defined(ERPC_CXL) || defined(ERPC_UB)
        pkthdr_0_(nullptr),
#endif
        buf_(reinterpret_cast<uint8_t *>(pkthdr) + sizeof(pkthdr_t)) {
    // max_data_size can be zero for control packets, so can't assert

    buffer_.buf_ = nullptr;  // Mark as a non-dynamic ("fake") MsgBuffer
  }

#if defined(ERPC_CXL) || defined(ERPC_UB)
  /// Construct a single-packet fake MsgBuffer whose eRPC header and payload are
  /// stored separately. This is used by CXL RX: the queue entry carries the
  /// per-hop header while the immutable payload remains in shared memory.
  MsgBuffer(pkthdr_t *pkthdr, uint8_t *payload, size_t max_data_size)
      : max_data_size_(max_data_size),
        data_size_(max_data_size),
        max_num_pkts_(1),
        num_pkts_(1),
        pkthdr_0_(pkthdr),
        buf_(payload) {
    buffer_.buf_ = nullptr;
  }
#endif

  /// Resize this MsgBuffer to any size smaller than its maximum allocation
  inline void resize(size_t new_data_size, size_t new_num_pkts) {
    assert(new_data_size <= max_data_size_);
    assert(new_num_pkts <= max_num_pkts_);
    data_size_ = new_data_size;
    num_pkts_ = new_num_pkts;
  }

 public:
  // The real constructors are private
  MsgBuffer()
      : buffer_(),
        max_data_size_(0),
        data_size_(0),
        max_num_pkts_(0),
        num_pkts_(0),
#if defined(ERPC_CXL) || defined(ERPC_UB)
        pkthdr_0_(nullptr),
        owned_pkthdr_(),
        owns_pkthdr_(false),
#endif
        buf_(nullptr) {}
  ~MsgBuffer() {}

#if defined(ERPC_CXL) || defined(ERPC_UB)
  inline void set_detached_pkthdr(pkthdr_t *pkthdr) {
    pkthdr_0_ = pkthdr;
    owns_pkthdr_ = false;
  }

  static MsgBuffer make_detached(pkthdr_t *pkthdr, uint8_t *payload,
                                 size_t max_data_size) {
    return MsgBuffer(pkthdr, payload, max_data_size);
  }

  MsgBuffer(const MsgBuffer &other) { *this = other; }

  MsgBuffer &operator=(const MsgBuffer &other) {
    buffer_ = other.buffer_;
    max_data_size_ = other.max_data_size_;
    data_size_ = other.data_size_;
    max_num_pkts_ = other.max_num_pkts_;
    num_pkts_ = other.num_pkts_;
#if defined(ERPC_CXL) || defined(ERPC_UB)
    owned_pkthdr_ = other.owned_pkthdr_;
    owns_pkthdr_ = other.owns_pkthdr_;
    pkthdr_0_ = owns_pkthdr_ ? &owned_pkthdr_ : other.pkthdr_0_;
#endif
    buf_ = other.buf_;
    return *this;
  }

  inline void own_pkthdr_copy() {
    if (buf_ == nullptr) return;
    owned_pkthdr_ = *get_pkthdr_0();
    pkthdr_0_ = &owned_pkthdr_;
    owns_pkthdr_ = true;
  }

  inline uint8_t get_hdr_req_type() const { return get_pkthdr_0()->req_type_; }

  inline size_t get_hdr_req_num() const { return get_pkthdr_0()->req_num_; }

  inline void set_hdr_req_type(uint8_t req_type) {
    get_pkthdr_0()->req_type_ = req_type;
  }

  inline void set_hdr_req_num(size_t req_num) {
    get_pkthdr_0()->req_num_ = req_num;
  }
#endif

  /**
   * Return the current amount of data in this message buffer. This can be
   * smaller than it's maximum data capacity due to resizing.
   */
  inline size_t get_data_size() const { return data_size_; }

 private:
  /// The optional backing hugepage buffer. buffer.buf points to the zeroth
  /// packet header, i.e., not application data.
  Buffer buffer_;

  // Size info
  size_t max_data_size_;  ///< Max data bytes in the MsgBuffer
  size_t data_size_;      ///< Current data bytes in the MsgBuffer
  size_t max_num_pkts_;   ///< Max number of packets in this MsgBuffer
  size_t num_pkts_;       ///< Current number of packets in this MsgBuffer
#if defined(ERPC_CXL) || defined(ERPC_UB)
  pkthdr_t *pkthdr_0_ = nullptr;  ///< Optional detached zeroth packet header
  pkthdr_t owned_pkthdr_{};
  bool owns_pkthdr_ = false;
#endif

 public:
  /// Pointer to the first application data byte. The message buffer is invalid
  /// invalid if this is null.
  uint8_t *buf_;
};
}  // namespace erpc
