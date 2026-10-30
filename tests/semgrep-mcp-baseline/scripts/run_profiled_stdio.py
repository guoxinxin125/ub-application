#!/usr/bin/env python3
"""Owned Semgrep stdio child with transport observation and a private trace log."""

import argparse
import json
import os
import sys


async def serve(log_path):
    import anyio
    from mcp.server.stdio import stdio_server
    from semgrep.commands.mcp import setup_mcp_server
    from semgrep.mcp.server import register, deregister_tools
    from stdio_timestamps import (clock_identity, ServerInput, ServerOutput,
                                  ReceiveObserver, SendObserver)

    records = {}
    sys.stdin.reconfigure(encoding="utf-8", errors="replace")
    sys.stdout.reconfigure(encoding="utf-8")
    server = setup_mcp_server("localhost", 8000, no_auth=True)
    register(server)
    deregister_tools(server, "stdio")
    with open(log_path, "x", encoding="utf-8") as log:
        log.write(json.dumps({"clock_identity": clock_identity()}) + "\n")
        log.flush()
        stdin = ServerInput(anyio.wrap_file(sys.stdin), records)
        stdout = ServerOutput(anyio.wrap_file(sys.stdout), records, log)
        async with stdio_server(stdin=stdin, stdout=stdout) as (reader, writer):
            await server._mcp_server.run(ReceiveObserver(reader, records, "b_decoded"),
                                        SendObserver(writer, records, "b_queued"),
                                        server._mcp_server.create_initialization_options())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-log", required=True)
    args = parser.parse_args()
    os.environ["SEMGREP_MCP"] = "true"
    import anyio
    anyio.run(serve, args.trace_log)


if __name__ == "__main__":
    main()
