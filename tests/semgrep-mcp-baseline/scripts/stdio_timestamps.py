"""Observe the existing MCP stdio transport, without changing its JSON protocol.

Linux same-host monotonic timestamps; IPC phases include wakeups and scheduling.
SDK internals are touched only during creation of this one owned subprocess.
"""

from contextlib import asynccontextmanager
import json
import os
from pathlib import Path
import time


def key(value):
    return json.dumps(value, separators=(",", ":"))


def clock_identity():
    def read(path):
        try:
            return Path(path).read_text().strip()
        except OSError:
            return None
    try:
        namespace = os.readlink("/proc/self/ns/time")
    except OSError:
        namespace = None
    return {"boot_id": read("/proc/sys/kernel/random/boot_id"), "time_namespace": namespace,
            "clock": time.get_clock_info("perf_counter").implementation}


def message_id(message):
    root = getattr(getattr(message, "message", None), "root", None)
    return root


class ReceiveObserver:
    def __init__(self, inner, records, stamp):
        self.inner, self.records, self.stamp = inner, records, stamp

    def __getattr__(self, name):
        return getattr(self.inner, name)

    async def __aenter__(self):
        await self.inner.__aenter__()
        return self

    async def __aexit__(self, *args):
        return await self.inner.__aexit__(*args)

    def __aiter__(self):
        return self

    async def __anext__(self):
        import anyio
        try:
            return await self.receive()
        except anyio.EndOfStream:
            raise StopAsyncIteration

    async def receive(self):
        message = await self.inner.receive()
        stamp = time.perf_counter_ns()
        root = message_id(message)
        if root is not None and hasattr(root, "id") and key(root.id) in self.records:
            if self.stamp == "b_decoded" and getattr(root, "method", None) == "tools/call":
                self.records[key(root.id)][self.stamp] = stamp
            elif self.stamp == "a_decoded" and getattr(root, "method", None) is None:
                self.records[key(root.id)][self.stamp] = stamp
        return message


class SendObserver(ReceiveObserver):
    async def send(self, message):
        root = message_id(message)
        if root is not None and hasattr(root, "id"):
            if self.stamp == "a_queued" and getattr(root, "method", None) == "tools/call":
                self.records[key(root.id)] = {"rpc_key": key(root.id), "a_queued": time.perf_counter_ns()}
            elif self.stamp == "b_queued" and getattr(root, "method", None) is None and key(root.id) in self.records:
                self.records[key(root.id)]["b_queued"] = time.perf_counter_ns()
        await self.inner.send(message)


class ClientStdioTrace:
    def __init__(self):
        self.records = {}

    def observe_process(self, process):
        """Keep the original Process type and SDK shutdown behavior intact."""
        original_send, original_receive = process.stdin.send, process.stdout.receive
        pending = bytearray()

        async def send(data):
            parsed = json.loads(data)
            record = self.records.get(key(parsed.get("id"))) if parsed.get("method") == "tools/call" else None
            if record is not None:
                record.update(a_write_start=time.perf_counter_ns(), request_frame_bytes=len(data))
            await original_send(data)
            if record is not None:
                record["a_write_done"] = time.perf_counter_ns()

        async def receive(*args, **kwargs):
            data = await original_receive(*args, **kwargs)
            stamp = time.perf_counter_ns()
            pending.extend(data)
            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending[:] = rest
                try:
                    parsed = json.loads(line)
                except ValueError:
                    continue
                if isinstance(parsed, dict) and ("result" in parsed or "error" in parsed):
                    record = self.records.get(key(parsed.get("id")))
                    if record is not None:
                        record.update(a_read_done=stamp, response_frame_bytes=len(line)+1)
            return data

        process.stdin.send = send
        process.stdout.receive = receive
        return process

    def attach(self, samples, log_path):
        events = [json.loads(line) for line in Path(log_path).read_text(encoding="utf-8").splitlines()]
        if not events or events[0].get("clock_identity") != clock_identity():
            raise ValueError("stdio clocks/boot/time namespace do not match")
        server = {}
        for event in events[1:]:
            if event["rpc_key"] in server:
                raise ValueError("duplicate stdio response trace")
            server[event["rpc_key"]] = event
        for sample in samples:
            rpc_key = sample["stdio_rpc_key"]
            stamps = {**self.records[rpc_key], **server.get(rpc_key, {}),
                      "a_call_start": sample.pop("_stdio_call_start"),
                      "a_call_end": sample.pop("_stdio_call_end")}
            sample["stdio_timestamps"] = stamps
            sample["stdio_boundary"] = partition(stamps)


@asynccontextmanager
async def traced_stdio_client(parameters, trace):
    import importlib
    import importlib.metadata
    if importlib.metadata.version("mcp") != "1.29.0":
        raise RuntimeError("stdio instrumentation currently requires MCP SDK 1.29.0")
    sdk = importlib.import_module("mcp.client.stdio")
    original = sdk._create_platform_compatible_process
    async def create(*args, **kwargs):
        return trace.observe_process(await original(*args, **kwargs))
    sdk._create_platform_compatible_process = create
    try:
        async with sdk.stdio_client(parameters) as (reader, writer):
            sdk._create_platform_compatible_process = original
            yield ReceiveObserver(reader, trace.records, "a_decoded"), SendObserver(writer, trace.records, "a_queued")
    finally:
        sdk._create_platform_compatible_process = original


class ServerInput:
    def __init__(self, inner, records):
        self.inner, self.records = inner, records

    def __aiter__(self):
        return self

    async def __anext__(self):
        line = await self.inner.readline()
        stamp = time.perf_counter_ns()
        if not line:
            raise StopAsyncIteration
        try:
            parsed = json.loads(line)
            if parsed.get("method") == "tools/call":
                rpc_key = key(parsed["id"])
                self.records[rpc_key] = {"rpc_key": rpc_key, "b_read_done": stamp}
        except (ValueError, AttributeError):
            pass
        return line


class ServerOutput:
    def __init__(self, inner, records, trace_log):
        self.inner, self.records, self.log = inner, records, trace_log
        self.active = None

    async def write(self, text):
        parsed = json.loads(text)
        self.active = self.records.get(key(parsed.get("id"))) if "result" in parsed or "error" in parsed else None
        if self.active is not None:
            self.active["b_write_start"] = time.perf_counter_ns()
        return await self.inner.write(text)

    async def flush(self):
        await self.inner.flush()
        if self.active is not None:
            self.active["b_write_done"] = time.perf_counter_ns()
            # Side channel only: never put trace lines on the MCP stdout pipe.
            self.log.write(json.dumps(self.active) + "\n")
            self.log.flush()
            self.active = None


def partition(stamps):
    names = ["a_call_start", "a_queued", "a_write_start", "b_read_done", "b_decoded",
             "b_queued", "b_write_start", "a_read_done", "a_decoded", "a_call_end"]
    phases = ["client_before_queue_ns", "client_encode_queue_ns", "request_ipc_ns",
              "server_decode_queue_ns", "server_processing_ns", "server_encode_queue_ns",
              "response_ipc_ns", "client_decode_queue_ns", "client_dispatch_ns"]
    if any(name not in stamps for name in names):
        raise ValueError(f"incomplete stdio trace: missing {set(names)-set(stamps)}")
    values = {phase: stamps[end]-stamps[start] for start, end, phase in zip(names, names[1:], phases)}
    if any(value < 0 for value in values.values()):
        raise ValueError("stdio timestamp order invalid: clocks/namespaces or trace association; do not clamp")
    return {"phases_ns": values, "end_to_end_ns": stamps["a_call_end"]-stamps["a_call_start"],
            "scope": "same-host SDK/pipe boundaries; queue/decode phases include observation overhead; IPC includes scheduling"}
