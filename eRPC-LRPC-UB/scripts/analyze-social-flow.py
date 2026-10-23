#!/usr/bin/env python3
"""Summarize opt-in SocialNetwork read-path events using same-host clocks only."""

import argparse
import re
from pathlib import Path


EVENT = re.compile(r"SN_FLOW_EVENT\s+role=(\w+)\s+thread=\d+\s+request=(\d+)\s+stage=(\w+)\s+at_ns=(\d+)")
MAP = re.compile(r"SN_FLOW_EVENT\s+role=client\s+thread=\d+\s+request=(\d+)\s+stage=wire_identity\s+at_ns=\d+\s+original=(\d+)")
CONTRACT = re.compile(r"SN_FLOW_CONTRACT\s+thread=\d+\s+events=(\d+)\s+dropped=(\d+)")
WARMUP = re.compile(r"SN_DOMAIN_MEASUREMENT\s+[^\n]*\bwarmup=(\d+)")
LANE = re.compile(r"SN_DOMAIN_MEASUREMENT\s+[^\n]*\bsource=(\d+)\s+lane=(\d+)")

# Each pair is measured entirely on one host. In particular, no A timestamp
# is ever subtracted from a B timestamp, even if the hosts appear synchronized.
A_PAIRS = (
    ("client_e2e", "client", "after_pop", "client", "consume_done"),
    ("client_prepare", "client", "after_pop", "client", "request_ready"),
    ("client_before_forward", "client", "after_pop", "client", "forward_begin"),
    ("forward_domain", "client", "forward_begin", "client", "forward_end"),
    ("client_after_forward", "client", "forward_end", "client", "request_ready"),
    ("client_send_setup", "client", "request_ready", "client", "enqueue_done"),
    ("client_flush_enqueue", "client", "send_begin", "client", "enqueue_done"),
    ("client_to_timeline_local", "client", "enqueue_done", "user_timeline", "request_arrival"),
    ("timeline_before_storage", "user_timeline", "request_arrival", "user_timeline", "storage_enqueue"),
    ("timeline_worker", "user_timeline", "worker_begin", "user_timeline", "worker_done"),
    ("storage_roundtrip", "user_timeline", "storage_enqueue", "user_timeline", "storage_callback"),
    ("timeline_after_storage", "user_timeline", "storage_callback", "user_timeline", "response_enqueue"),
    ("reverse_domain", "user_timeline", "reverse_begin", "user_timeline", "reverse_end"),
    ("timeline_to_client_local", "user_timeline", "response_enqueue", "client", "response_arrival"),
    ("client_consume", "client", "response_arrival", "client", "consume_done"),
)
B_PAIRS = (
    ("lb_forward_residence", "load_balance", "forward_receive", "load_balance", "forward_enqueue"),
    ("lb_to_nginx_local", "load_balance", "forward_enqueue", "nginx", "forward_receive"),
    ("nginx_forward_residence", "nginx", "forward_receive", "nginx", "forward_enqueue"),
    ("nginx_reverse_residence", "nginx", "reverse_receive", "nginx", "reverse_enqueue"),
    ("nginx_to_lb_local", "nginx", "reverse_enqueue", "load_balance", "reverse_receive"),
    ("lb_reverse_residence", "load_balance", "reverse_receive", "load_balance", "reverse_enqueue"),
    ("post_storage_residence", "post_storage", "request_arrival", "post_storage", "response_enqueue"),
)


def timeline_pairs(workload):
    role = f"{workload}_timeline"
    return tuple((label, role if first_role == "user_timeline" else first_role, first_stage,
                  role if last_role == "user_timeline" else last_role, last_stage)
                 for label, first_role, first_stage, last_role, last_stage in A_PAIRS)


def load_logs(directory, names, wire_map=None, require_map=False):
    events = {}
    contracts = 0
    warmup = None
    lanes = set()
    mapping = {} if wire_map is None else dict(wire_map)
    raw_events = []
    for name in names:
        path = directory / f"{name}.log"
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if name == "client":
            match = WARMUP.search(text)
            if match:
                warmup = int(match.group(1))
            lanes.update(LANE.findall(text))
        for match in CONTRACT.finditer(text):
            contracts += 1
            if int(match.group(2)):
                raise ValueError(f"{path}: flow events dropped; reduce sample count")
        for match in MAP.finditer(text):
            wire, original = map(int, match.groups())
            if wire in mapping and mapping[wire] != original:
                raise ValueError(f"{path}: conflicting wire request mapping for {wire}")
            mapping[wire] = original
        for match in EVENT.finditer(text):
            role, request, stage, at_ns = match.groups()
            raw_events.append((int(request), role, stage, int(at_ns)))
    if len(lanes) > 1:
        raise ValueError(f"{directory}: analysis requires one client source/lane; got {sorted(lanes)}")
    for request, role, stage, at_ns in raw_events:
        if require_map and role != "client":
            if request not in mapping:
                raise ValueError(f"{directory}: missing original request for wire={request}")
            request = mapping[request]
        if stage == "wire_identity":
            continue
        key = (request, role, stage)
        events.setdefault(key, []).append(at_ns)
    return events, contracts, warmup, mapping


def percentile(values, percentage):
    return values[(len(values) * percentage + 99) // 100 - 1]


def summarize(mode, host, events, pairs, warmup):
    for label, first_role, first_stage, last_role, last_stage in pairs:
        if mode == "rpc" and label in ("client_before_forward", "forward_domain", "client_after_forward", "reverse_domain",
                                        "client_to_timeline_local", "timeline_to_client_local"):
            continue
        if mode == "domain" and (label.startswith("lb_") or label.startswith("nginx_")):
            continue
        values = []
        ambiguous = 0
        requests = {key[0] for key in events if key[0] > warmup}
        for request in requests:
            begins = events.get((request, first_role, first_stage), ())
            ends = events.get((request, last_role, last_stage), ())
            if not begins or not ends:
                continue
            # A read can fan out to multiple storage requests. Never silently
            # choose one of several timestamps or turn an overlapping span into
            # an additive per-request stage.
            if len(begins) != 1 or len(ends) != 1 or ends[0] < begins[0]:
                ambiguous += 1
                continue
            values.append(ends[0] - begins[0])
        if not values:
            continue
        values.sort()
        print(f"SN_FLOW_SUMMARY mode={mode} host={host} stage={label} count={len(values)} "
              f"ambiguous={ambiguous} p50_ns={percentile(values, 50)} "
              f"p99_ns={percentile(values, 99)} avg_ns={sum(values)/len(values):.1f} "
              "clock=steady_ns scope=same-host-only")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload", choices=("user", "home"), default="user")
    parser.add_argument("--mode", choices=("domain", "rpc"), required=True)
    parser.add_argument("--a-logs", type=Path, required=True)
    parser.add_argument("--b-logs", type=Path, required=True)
    args = parser.parse_args()
    domain = args.mode == "domain"
    timeline = f"{args.workload}_timeline"
    a, a_contracts, warmup, mapping = load_logs(
        args.a_logs, ("client", timeline), require_map=domain)
    b, b_contracts, _, _ = load_logs(
        args.b_logs, ("load_balance", "nginx", "post_storage"),
        wire_map=mapping, require_map=domain)
    if not a_contracts or (args.mode == "rpc" and not b_contracts):
        parser.error("missing SN_FLOW_CONTRACT; enable both profile flags on A and B, then stop the sessions cleanly")
    if warmup is None:
        parser.error("missing client SN_DOMAIN_MEASUREMENT warmup")
    print(f"SN_FLOW_ANALYSIS workload={args.workload} mode={args.mode} warmup={warmup} "
          f"a_events={sum(map(len, a.values()))} b_events={sum(map(len, b.values()))} "
          "cross_host_subtraction=0")
    summarize(args.mode, "A", a, timeline_pairs(args.workload), warmup)
    summarize(args.mode, "B", b, B_PAIRS, warmup)


if __name__ == "__main__":
    main()
