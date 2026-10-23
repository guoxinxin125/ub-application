#!/usr/bin/env python3
"""Compare A's final write expectations with B's observed owner-side values."""

import argparse
import re
from pathlib import Path

FIELDS = re.compile(r"\b([a-z_]+)=([0-9]+)\b")


def rows(path: Path, marker: str):
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if marker in line:
            yield {key: int(value) for key, value in FIELDS.findall(line.split(marker, 1)[1])}


def verify(a_path: Path, b_path: Path) -> str:
    a_text = a_path.read_text(encoding="utf-8", errors="replace")
    b_text = b_path.read_text(encoding="utf-8", errors="replace")
    if any(marker in a_text for marker in ("UB_DOMAIN_WRITE_WORKER_FAIL",
                                           "UB_DOMAIN_WRITE_CLOSE_FAIL",
                                           "UB_DOMAIN_WRITE_CAMPAIGN_FAIL")):
        raise ValueError("A log contains a worker/campaign failure")
    if "UB_DOMAIN_OWNER_FAIL" in b_text:
        raise ValueError("B owner log contains a failure")
    campaigns = list(rows(a_path, "UB_DOMAIN_WRITE_CAMPAIGN_PASS"))
    if len(campaigns) != 1:
        raise ValueError("A log must contain exactly one successful campaign")
    campaign = campaigns[0]
    workers = campaign["workers"]
    calls = campaign["calls_per_worker"]
    expected = {}
    worker_slots = {}
    for row in rows(a_path, "UB_DOMAIN_WRITE_A_PASS"):
        key = (row["slot"], row["identity"], row["index"])
        if key in expected or row["value"] != row["ticket"]:
            raise ValueError(f"duplicate or invalid A result: {key}")
        worker = row["worker"]
        if worker in worker_slots and worker_slots[worker] != key[:2]:
            raise ValueError(f"worker {worker} changed slot/identity")
        worker_slots[worker] = key[:2]
        expected[key] = row["value"]
    if (not workers or not calls or len(expected) != workers * calls or
            campaign["final_cells"] != len(expected) or
            set(worker_slots) != set(range(workers)) or
            len(set(worker_slots.values())) != workers):
        raise ValueError("A result count or connection identity does not match campaign")
    for worker, slot_identity in worker_slots.items():
        if {key[2] for key in expected if key[:2] == slot_identity} != set(range(1, calls + 1)):
            raise ValueError(f"worker {worker} did not report all final cells")

    ready = {(r["slot"], r["identity"])
             for r in rows(b_path, "UB_DOMAIN_WRITE_SLOT_READY")}
    observed = {}
    for row in rows(b_path, "UB_DOMAIN_WRITE_OBSERVED"):
        key = (row["slot"], row["identity"], row["index"])
        observed[key] = row["value"]  # The final B observation matters after rewrites.
    for key, value in expected.items():
        if key[:2] not in ready:
            raise ValueError(f"B did not announce this connection: {key[:2]}")
        if observed.get(key) != value:
            raise ValueError(f"B final value mismatch: {key}, A={value}, B={observed.get(key)}")
    return (f"UB_DOMAIN_WRITE_VISIBILITY_PASS workers={workers} "
            f"final_cells={len(expected)} rewrites_per_worker={campaign['rewrites_per_worker']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a-log", type=Path, required=True)
    parser.add_argument("--b-log", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(verify(args.a_log, args.b_log))
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"UB_DOMAIN_WRITE_VISIBILITY_FAIL reason={error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
