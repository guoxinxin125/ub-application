#!/usr/bin/env python3
"""Verify one shared-page campaign against B's observation from the same owner run."""

import argparse
import re
from pathlib import Path


def fields(line: str) -> dict[str, int]:
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}


def verify(a_path: Path, b_path: Path) -> str:
    a = a_path.read_text(encoding="utf-8", errors="replace").splitlines()
    b = b_path.read_text(encoding="utf-8", errors="replace").splitlines()
    if any(("UB_DOMAIN_SHARED_" in line and "_FAIL" in line) or
           "UB_DOMAIN_OWNER_FAIL" in line for line in a + b):
        raise ValueError("shared probe or owner log contains a failure")
    campaign = [fields(line) for line in a if line.startswith("UB_DOMAIN_SHARED_CAMPAIGN_PASS ")]
    read = [fields(line) for line in a if line.startswith("UB_DOMAIN_SHARED_READ_PASS ")]
    atomic = [fields(line) for line in a if line.startswith("UB_DOMAIN_SHARED_ATOMIC_PASS ")]
    if len(campaign) != 1 or len(read) != 1 or len(atomic) != 1:
        raise ValueError("one successful A campaign, read and atomic result required")
    result = campaign[0]
    required = ("workers", "rounds", "increments_per_worker", "generation",
                "final_cell", "final_counter")
    if any(key not in result for key in required):
        raise ValueError("incomplete A campaign result")
    if result["workers"] < 2 or result["rounds"] < 2 or result["increments_per_worker"] < 1:
        raise ValueError("invalid campaign counts")
    if result["final_counter"] != result["workers"] * result["increments_per_worker"]:
        raise ValueError("A counter does not match total increments")
    if (read[0].get("workers") != result["workers"] or
        read[0].get("rounds") != result["rounds"] or
        read[0].get("generation") != result["generation"] or
        read[0].get("final_cell") != result["final_cell"] or
        atomic[0].get("workers") != result["workers"] or
        atomic[0].get("increments_per_worker") != result["increments_per_worker"] or
        atomic[0].get("unique_old_values") != result["final_counter"] or
        atomic[0].get("final_counter") != result["final_counter"]):
        raise ValueError("A read/atomic summaries disagree with campaign")
    ready = [fields(line) for line in b if line.startswith("UB_DOMAIN_SHARED_READY ")]
    if len(ready) != 1 or ready[0].get("generation") != result["generation"]:
        raise ValueError("B did not announce this shared-page generation")
    observed: dict[int, int] = {}
    for line in b:
        if line.startswith("UB_DOMAIN_SHARED_OBSERVED "):
            event = fields(line)
            if event.get("generation") != result["generation"]:
                continue
            if event.get("index") not in (1, 2) or "value" not in event:
                raise ValueError("malformed B shared observation")
            observed[event["index"]] = event["value"]
    if observed.get(1) != result["final_cell"] or observed.get(2) != result["final_counter"]:
        raise ValueError("B final shared state mismatch")
    return ("UB_DOMAIN_SHARED_VISIBILITY_PASS "
            f"workers={result['workers']} rounds={result['rounds']} "
            f"final_cell={result['final_cell']} final_counter={result['final_counter']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a-log", required=True, type=Path)
    parser.add_argument("--b-log", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(verify(args.a_log, args.b_log))
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
