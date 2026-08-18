/**
 * @file log_callbacks.h
 * SMR log record callbacks
 */

#pragma once
#include "smr.h"

static int smr_raft_send_snapshot_cb(raft_server_t *, void *, raft_node_t *) {
  // 伪 Snapshot 模式：不真正发送 Snapshot，只返回错误码
  // 这会导致落后太多的 Follower 无法追赶，但不会崩溃
  return -1;
}

// Raft callback for setting the log entry at \p entry_idx to \p *ety
static int smr_raft_log_offer_cb(raft_server_t *, void *udata,
                                 raft_entry_t *ety, raft_index_t entry_idx) {
  // We currently handle only application log entries
  assert(!raft_entry_is_cfg_change(ety));
  assert(ety->data.len == sizeof(client_req_t));

  if (kUsePmem) {
    auto *c = static_cast<AppContext *>(udata);

    // During failures, willemt/raft uses log_pop() to pop old entries instead
    // of directly overwriting them. This is also the behavior of LogCabin.
    erpc::rt_assert(static_cast<size_t>(entry_idx) ==
                    c->server.pmem_log->get_num_entries());

    c->server.pmem_log->append(pmem_ser_logentry_t(
        *ety, *reinterpret_cast<client_req_t *>(ety->data.buf)));
  }

  return 0;  // Unneeded for DRAM mode
}

// Raft callback for applying an entry to the FSM
static int smr_raft_applylog_cb(raft_server_t *, void *udata, raft_entry_t *ety,
                                raft_index_t idx) {
  assert(!raft_entry_is_cfg_change(ety));

  // We're applying an entry to the application's state machine, so we're sure
  // about its length. Other log callbacks can be invoked for non-application
  // log entries.
  assert(ety->data.len == sizeof(client_req_t));
  auto *client_req = reinterpret_cast<client_req_t *>(ety->data.buf);
  assert(client_req->key == client_req->value.v[0]);

  auto *c = static_cast<AppContext *>(udata);

  if (kAppVerbose) {
    printf("smr: Applying log entry %s received at Raft server %u [%s].\n",
           client_req->to_string().c_str(), ety->id,
           erpc::get_formatted_time().c_str());
  }

  c->server.table.insert(
      std::pair<size_t, value_t>(client_req->key, client_req->value));
    
  // 每 100,000 条日志触发一次 Snapshot，释放 CXL 内存
  if (idx > 0 && idx % 100000 == 0) {
    c->server.need_snapshot = true;
  }
  return 0;
}

// Raft callback for saving voted_for field to persistent storage.
static int smr_raft_persist_vote_cb(raft_server_t *, void *udata,
                                    raft_node_id_t voted_for) {
  if (kUsePmem) {
    auto *c = static_cast<AppContext *>(udata);
    c->server.pmem_log->persist_vote(voted_for);
  }

  return 0;  // Unneeded for DRAM mode
}

// Raft callback for saving term and voted_for field to persistent storage
static int smr_raft_persist_term_cb(raft_server_t *, void *udata,
                                    raft_term_t term,
                                    raft_node_id_t voted_for) {
  erpc::rt_assert(term < INT32_MAX, "Term too large for atomic pmem append");
  if (kUsePmem) {
    auto *c = static_cast<AppContext *>(udata);
    c->server.pmem_log->persist_term(term, voted_for);
  }
  return 0;  // Unneeded for DRAM mode
}

// Raft callback for removing the first entry from the log (log compaction)
static int smr_raft_log_poll_cb(raft_server_t *raft, void *udata, raft_entry_t *ety,
                                raft_index_t) {
  auto *c = static_cast<AppContext *>(udata);
  
  if (likely(ety->data.len == sizeof(client_req_t))) {
    if (ety->data.buf == nullptr) return 0;
    
    c->server.log_entry_appdata_pool.free(
        static_cast<client_req_t *>(ety->data.buf));
    ety->data.buf = nullptr;
  } else {
    if (ety->data.buf != nullptr) {
      free(ety->data.buf);
      ety->data.buf = nullptr;
    }
  }

  return 0;
}

// Raft callback for deleting the most recent entry from the log (rollback)
static int smr_raft_log_pop_cb(raft_server_t *raft, void *udata, raft_entry_t *ety,
                               raft_index_t) {
  auto *c = static_cast<AppContext *>(udata);
  if (kUsePmem) c->server.pmem_log->pop();

  if (likely(ety->data.len == sizeof(client_req_t))) {
    if (ety->data.buf == nullptr) return 0;
    
    c->server.log_entry_appdata_pool.free(
        static_cast<client_req_t *>(ety->data.buf));
    ety->data.buf = nullptr;
  } else {
    if (ety->data.buf != nullptr) {
      free(ety->data.buf);
      ety->data.buf = nullptr;
    }
  }

  return 0;
}

// Raft callback for determining which node this configuration log entry affects
static int smr_raft_log_get_node_id_cb(raft_server_t *, void *, raft_entry_t *,
                                       raft_index_t) {
  erpc::rt_assert(false, "Configuration change not supported");
  return -1;
}

// Non-voting node now has enough logs to be able to vote. Append a finalization
// cfg log entry.
static int smr_raft_node_has_sufficient_logs_cb(raft_server_t *, void *,
                                                raft_node_t *) {
  printf("smr: Ignoring smr_raft_node_has_sufficient_logs callback.\n");
  return 0;
}

// Callback for being notified of membership changes. Implementing this callback
// is optional.
static void smr_raft_notify_membership_event_cb(raft_server_t *, void *,
                                                raft_node_t *, raft_entry_t *,
                                                raft_membership_e) {
  printf("smr: Ignoring smr_raft_notify_membership_event callback.\n");
}
