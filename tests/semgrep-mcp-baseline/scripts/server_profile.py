"""Opt-in, request-local profiling helpers for the Semgrep MCP custom-rule tool.

Installed as semgrep.mcp.baseline_profile; no global monkey-patching of scans.
"""

from contextlib import contextmanager
from contextvars import ContextVar
from functools import wraps
import json
import os
from time import perf_counter_ns
from uuid import uuid4


_current = ContextVar("semgrep_baseline_profile", default=None)


@contextmanager
def phase(name):
    """Measure one non-overlapping server phase when profiling is active."""
    state = _current.get()
    if state is None:
        yield
        return
    start = perf_counter_ns()
    try:
        yield
    finally:
        state["phases_ns"][name] = state["phases_ns"].get(name, 0) + perf_counter_ns() - start


def call(name, function, *args, **kwargs):
    with phase(name):
        return function(*args, **kwargs)


async def await_call(name, function, *args, **kwargs):
    with phase(name):
        return await function(*args, **kwargs)


async def scan_output(function, *args, **kwargs):
    """Retain native --time output separately; it is nested, not additive."""
    state = _current.get()
    if state is not None and os.environ.get("SEMGREP_MCP_PROFILE_ENGINE_TIME") == "1":
        kwargs = dict(kwargs)
        kwargs["args"] = [*kwargs["args"], "--time"]
    with phase("scan_backend_ns"):
        output = await function(*args, **kwargs)
    if state is not None and os.environ.get("SEMGREP_MCP_PROFILE_ENGINE_TIME") == "1":
        with phase("engine_profile_decode_ns"):
            state["engine_time"] = json.loads(output).get("time")
    return output


def custom_rule(function):
    """Attach timing only after the function's finally/cleanup has completed."""
    @wraps(function)
    async def wrapped(*args, **kwargs):
        if os.environ.get("SEMGREP_MCP_PROFILE") != "1":
            return await function(*args, **kwargs)
        state = {"schema": 1, "request_id": uuid4().hex,
                 "backend": "cli-custom-rule", "phases_ns": {}}
        token = _current.set(state)
        start = perf_counter_ns()
        try:
            result = await function(*args, **kwargs)
            state["tool_total_ns"] = perf_counter_ns() - start
            state["phases_ns"]["other_tool_ns"] = (
                state["tool_total_ns"] - sum(state["phases_ns"].values())
            )
            result.mcp_scan_results = dict(result.mcp_scan_results)
            result.mcp_scan_results["baseline_profile"] = state
            return result
        finally:
            _current.reset(token)
    return wrapped
