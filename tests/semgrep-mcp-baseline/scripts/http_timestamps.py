"""HTTP application-boundary timestamps, not NIC/wire timestamps.

Only selected tools/call POSTs are recorded. Wall clocks estimate directions;
monotonic intervals measure server/request round trips without clock sync.
"""

import json
import time
import uuid
import httpx


ID = "x-baseline-trace-id"


class HttpBoundaryMiddleware:
    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        headers = dict(scope.get("headers", []))
        trace_id = headers.get(ID.encode())
        if trace_id and (len(trace_id) != 32 or any(c not in b"0123456789abcdef" for c in trace_id)):
            trace_id = None
        if scope["type"] != "http" or scope.get("method") != "POST" or not trace_id:
            await self.app(scope, receive, send)
            return
        state = {"entry_wall": time.time_ns(), "entry_mono": time.perf_counter_ns()}

        async def received():
            message = await receive()
            if message["type"] == "http.request" and not message.get("more_body", False):
                state["body_wall"] = time.time_ns()
                state["body_mono"] = time.perf_counter_ns()
            return message

        async def sent(message):
            if message["type"] == "http.response.start":
                stamp_wall, stamp_mono = time.time_ns(), time.perf_counter_ns()
                values = {
                    ID: trace_id.decode("ascii"),
                    "x-baseline-b-entry-wall": state["entry_wall"],
                    "x-baseline-b-body-wall": state.get("body_wall", 0),
                    "x-baseline-b-send-wall": stamp_wall,
                    "x-baseline-b-server-ns": stamp_mono - state["entry_mono"],
                    "x-baseline-b-body-ns": (stamp_mono - state["body_mono"]) if "body_mono" in state else -1,
                }
                message = dict(message)
                message["headers"] = [*message.get("headers", []),
                    *[(key.encode("ascii"), str(value).encode("ascii")) for key, value in values.items()]]
            await send(message)

        await self.app(scope, received, sent)


class ResponseStream(httpx.AsyncByteStream):
    def __init__(self, inner, record):
        self.inner, self.record = inner, record

    async def __aiter__(self):
        count = 0
        async for chunk in self.inner:
            count += len(chunk)
            yield chunk
        self.record.update(a_body_wall=time.time_ns(), a_body_mono=time.perf_counter_ns(), response_body_bytes=count)

    async def aclose(self):
        await self.inner.aclose()


class ClientHttpTrace:
    def __init__(self):
        self.records = []

    async def request(self, request):
        if request.method != "POST":
            return
        try:
            body = json.loads(request.content)
        except (ValueError, httpx.RequestNotRead):
            return
        if not isinstance(body, dict) or body.get("method") != "tools/call":
            return
        record = {"trace_id": uuid.uuid4().hex, "rpc_id": body.get("id"),
                  "request_body_bytes": len(request.content)}
        request.headers[ID] = record["trace_id"]
        request.extensions["baseline_trace"] = record
        self.records.append(record)
        record.update(a_send_wall=time.time_ns(), a_send_mono=time.perf_counter_ns())

    async def response(self, response):
        record = response.request.extensions.get("baseline_trace")
        if record is None:
            return
        record.update(a_headers_wall=time.time_ns(), a_headers_mono=time.perf_counter_ns(),
                      status=response.status_code,
                      content_type=response.headers.get("content-type", ""))
        record["server_headers"] = {key: value for key, value in response.headers.items()
                                    if key.startswith("x-baseline-")}
        response.stream = ResponseStream(response.stream, record)

    def factory(self, **kwargs):
        kwargs["event_hooks"] = {"request": [self.request], "response": [self.response]}
        # Match SDK 1.29 defaults, including proxy environment and redirects.
        kwargs.setdefault("follow_redirects", True)
        if kwargs.get("timeout") is None:
            kwargs["timeout"] = httpx.Timeout(30, read=300)
        return httpx.AsyncClient(**kwargs)


def estimate(record, offset_ns=0, uncertainty_ns=None):
    """offset_ns is B minus A; uncertainty must come from external clock evidence."""
    headers = record.get("server_headers", {})
    if headers.get(ID) != record["trace_id"]:
        raise ValueError("HTTP server timestamps missing or trace ID mismatch")
    if "application/json" not in record.get("content_type", "") or "a_body_mono" not in record:
        raise ValueError("requires a complete JSON response, not SSE or an interrupted body")
    b_entry = int(headers["x-baseline-b-entry-wall"])
    b_body = int(headers["x-baseline-b-body-wall"])
    b_send = int(headers["x-baseline-b-send-wall"])
    if not b_body:
        raise ValueError("server did not record completed request body")
    upstream = b_body - offset_ns - record["a_send_wall"]
    downstream = record["a_body_wall"] - (b_send - offset_ns)
    server_body_ns = int(headers["x-baseline-b-body-ns"])
    rtt = record["a_body_mono"] - record["a_send_mono"]
    combined = rtt - server_body_ns
    wall_rtt = record["a_body_wall"] - record["a_send_wall"]
    server_wall = b_send - b_body
    skew_check = (wall_rtt - server_wall) - combined
    warnings = []
    if uncertainty_ns is None:
        warnings.append("clock_error_bound_unknown: one-way estimates are exploratory")
    if upstream < 0 or downstream < 0 or combined < 0:
        warnings.append("negative_interval: clock offset/jump or invalid boundary; do not clamp")
    if abs(skew_check) > max(1_000_000, uncertainty_ns or 0):
        warnings.append("wall_monotonic_disagreement: investigate clock adjustments or measurement error")
    return {"upstream_estimate_ns": upstream, "downstream_estimate_ns": downstream,
            "combined_http_boundary_ns": combined, "http_round_trip_ns": rtt,
            "server_body_to_headers_ns": server_body_ns,
            "server_request_body_read_ns": int(headers["x-baseline-b-server-ns"]) - server_body_ns,
            "client_response_body_read_ns": record["a_body_mono"] - record["a_headers_mono"],
            "wall_minus_monotonic_residual_ns": skew_check,
            "assumed_b_minus_a_ns": offset_ns, "clock_uncertainty_ns": uncertainty_ns,
            "upstream_range_ns": [upstream-uncertainty_ns, upstream+uncertainty_ns] if uncertainty_ns is not None else None,
            "downstream_range_ns": [downstream-uncertainty_ns, downstream+uncertainty_ns] if uncertainty_ns is not None else None,
            "warnings": warnings,
            "scope": "HTTP application boundaries; includes stacks, scheduling, pooling and body handling, NOT pure wire"}
