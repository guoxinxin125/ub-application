#include "rpc.h"

namespace erpc {

template <class TTr>
void Rpc<TTr>::process_comps_st() {
  assert(in_dispatch());
  const size_t num_pkts = transport_->rx_burst();
  if (num_pkts == 0) return;

  // Measure RX burst size
  dpath_stat_inc(dpath_stats_.rx_burst_calls_, 1);
  dpath_stat_inc(dpath_stats_.pkts_rx_, num_pkts);

  // ev_loop_tsc was taken just before calling the packet RX code
  const size_t &batch_rx_tsc = ev_loop_tsc_;

  for (size_t i = 0; i < num_pkts; i++) {
    auto *pkthdr = reinterpret_cast<pkthdr_t *>(rx_ring_[rx_ring_head_]);
    rx_ring_head_ = (rx_ring_head_ + 1) % Transport::kNumRxRingEntries;

#if defined(ERPC_CXL) || defined(ERPC_UB)
    auto release_shared_queued_ref = [&]() {
      if (!std::is_same<TTr, CTransport>::value || pkthdr->msg_size_ == 0) {
        return;
      }

      const uint8_t *payload = get_pkt_payload_ptr(pkthdr);
      if (payload == nullptr ||
          payload == reinterpret_cast<const uint8_t *>(pkthdr + 1)) {
        return;
      }

      CTransport *shared_transport = static_cast<CTransport *>(transport_);
      const uint8_t *buffer_start = pkthdr->is_zerocopy_ != 0
                                        ? payload - sizeof(pkthdr_t)
                                        : payload;
      shared_transport->free_shared_buffer(
          Buffer(const_cast<uint8_t *>(buffer_start),
                 pkthdr->msg_size_ +
                     (pkthdr->is_zerocopy_ != 0 ? sizeof(pkthdr_t) : 0),
                 0));
    };
#endif

    // XXX: This acts as a stopgap function to filter non-eRPC packets, like
    // broadcast/ARP packets.
    if (unlikely(!pkthdr->check_magic())) {
#if defined(ERPC_CXL) || defined(ERPC_UB)
      static size_t invalid_magic_prints = 0;
      if (invalid_magic_prints++ < 3) {
        printf("Shared-memory RX invalid magic: rpc_id=%u magic=%zu type=%zu req_type=%u "
               "dsn=%u reqn=%zu pktn=%zu msg_size=%u\n",
               rpc_id_, static_cast<size_t>(pkthdr->magic_),
               static_cast<size_t>(pkthdr->pkt_type_), pkthdr->req_type_,
               pkthdr->dest_session_num_, static_cast<size_t>(pkthdr->req_num_),
               static_cast<size_t>(pkthdr->pkt_num_), pkthdr->msg_size_);
        fflush(stdout);
      }
      release_shared_queued_ref();
#else
      ERPC_INFO(
          "Rpc %u: Received %s with invalid magic. Packet headroom = %s. "
          "Dropping.\n",
          rpc_id_, pkthdr->to_string().c_str(),
          pkthdr->headroom_string().c_str());
#endif
      continue;
    }

    assert(pkthdr->msg_size_ <= kMaxMsgSize);  // msg_size can be 0 here

    if (unlikely(pkthdr->dest_session_num_ >= session_vec_.size())) {
#if defined(ERPC_CXL) || defined(ERPC_UB)
      ERPC_WARN(
          "Rpc %u: Received %s for a session yet to be connected. Dropping.\n",
          rpc_id_, pkthdr->to_string().c_str());
      release_shared_queued_ref();
#endif
      continue;
    }

    Session *session = session_vec_[pkthdr->dest_session_num_];
    if (unlikely(session == nullptr)) {
      ERPC_WARN("Rpc %u: Received %s for buried session. Dropping.\n", rpc_id_,
                pkthdr->to_string().c_str());
#if defined(ERPC_CXL) || defined(ERPC_UB)
      release_shared_queued_ref();
#endif
      continue;
    }
    // printf("eRPC RX check:, dest_session_num=%u, curr_session=%p, is_connected=%d, pkt_type=%d, req_num=%lu\n", rpc_id_, pkthdr->dest_session_num_, session, session->is_connected(), pkthdr->pkt_type_, pkthdr->req_num_);
 
    if (unlikely(!session->is_connected())) {
#if !defined(ERPC_CXL) && !defined(ERPC_UB)
      ERPC_WARN(
          "Rpc %u: Received %s for unconnected session (state %s). Dropping.\n",
          rpc_id_, pkthdr->to_string().c_str(),
          session_state_str(session->state_).c_str());
#else
      release_shared_queued_ref();
#endif
      continue;
    }

    assert(pkthdr->msg_size_ <= kMaxMsgSize);  // msg_size can be 0 here

    const size_t sslot_i = pkthdr->req_num_ % kSessionReqWindow;  // Bit shift
    SSlot *sslot = &session->sslot_arr_[sslot_i];

    switch (pkthdr->pkt_type_) {
      case PktType::kReq: {
#if defined(ERPC_CXL) || defined(ERPC_UB)
        if (std::is_same<TTr, CTransport>::value) {
          process_small_req_st(sslot, pkthdr);
          break;
        }
#endif
        pkthdr->msg_size_ <= TTr::kMaxDataPerPkt
            ? process_small_req_st(sslot, pkthdr)
            : process_large_req_one_st(sslot, pkthdr);
        break;
      }
      case PktType::kResp: {
        size_t rx_tsc = kCcOptBatchTsc ? batch_rx_tsc : dpath_rdtsc();
        process_resp_one_st(sslot, pkthdr, rx_tsc);
        break;
      }
      case PktType::kRFR: {
        process_rfr_st(sslot, pkthdr);
        break;
      }
      case PktType::kExplCR: {
        size_t rx_tsc = kCcOptBatchTsc ? batch_rx_tsc : dpath_rdtsc();
        process_expl_cr_st(sslot, pkthdr, rx_tsc);
        break;
      }
    }
  }

  // Technically, these RECVs can be posted immediately after rx_burst(), or
  // even in the rx_burst() code.
  transport_->post_recvs(num_pkts);
}

template <class TTr>
void Rpc<TTr>::submit_bg_req_st(SSlot *sslot) {
  assert(in_dispatch());
  assert(nexus_->num_bg_threads_ > 0);

  const size_t bg_etid = fast_rand_.next_u32() % nexus_->num_bg_threads_;
  auto *req_queue = nexus_hook_.bg_req_queue_arr_[bg_etid];

  req_queue->unlocked_push(Nexus::BgWorkItem::make_req_item(context_, sslot));
}

template <class TTr>
void Rpc<TTr>::submit_bg_resp_st(erpc_cont_func_t cont_func, void *tag,
                                 size_t bg_etid) {
  assert(in_dispatch());
  assert(nexus_->num_bg_threads_ > 0);
  assert(bg_etid < nexus_->num_bg_threads_);

  auto *req_queue = nexus_hook_.bg_req_queue_arr_[bg_etid];
  req_queue->unlocked_push(
      Nexus::BgWorkItem::make_resp_item(context_, cont_func, tag));
}

FORCE_COMPILE_TRANSPORTS

}  // namespace erpc
