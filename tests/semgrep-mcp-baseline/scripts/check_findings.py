#!/usr/bin/env python3
"""Compare CLI or saved MCP findings with the fixed expected manifest."""

import json
import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def unwrap(document):
    if "results" in document:
        return document
    response = document.get("response", document)
    structured = response.get("structuredContent") or response.get("structured_content")
    if structured:
        if isinstance(structured, dict) and "results" in structured:
            return structured
        if isinstance(structured, dict) and "result" in structured:
            inner = structured["result"]
            return json.loads(inner) if isinstance(inner, str) else inner
    for block in response.get("content", []):
        if block.get("type") == "text":
            payload = json.loads(block["text"])
            if isinstance(payload, dict) and "results" in payload:
                return payload
    raise ValueError("No Semgrep results found in CLI/MCP JSON")


def finding_key(item):
    return (item["check_id"], Path(item["path"]).name, item["start"]["line"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("result", type=Path)
    parser.add_argument("--expected", type=Path, default=ROOT / "expected.json")
    args = parser.parse_args()
    expected = json.loads(args.expected.read_text(encoding="utf-8"))
    actual = unwrap(json.loads(args.result.read_text(encoding="utf-8")))
    if actual.get("errors"):
        raise SystemExit(f"SCAN_ERRORS: {actual['errors']}")
    expected_keys = sorted((x["check_id"], x["path"], x["line"])
                           for x in expected["findings"])
    actual_keys = sorted(finding_key(x) for x in actual["results"])
    if actual_keys != expected_keys:
        raise SystemExit(f"FINDINGS_FAIL expected={expected_keys} actual={actual_keys}")
    print(f"FINDINGS_PASS count={len(actual_keys)} keys={actual_keys}")


if __name__ == "__main__":
    main()
