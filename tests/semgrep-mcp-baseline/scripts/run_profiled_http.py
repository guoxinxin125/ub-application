#!/usr/bin/env python3
"""Run the installed modified Semgrep MCP with optional ASGI boundary headers."""

import argparse
import os


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--no-auth", action="store_true")
    args = parser.parse_args()
    os.environ["SEMGREP_MCP"] = "true"
    from semgrep.commands.mcp import setup_mcp_server
    from semgrep.mcp.server import register, deregister_tools
    from http_timestamps import HttpBoundaryMiddleware
    import uvicorn

    server = setup_mcp_server(args.host, args.port, no_auth=args.no_auth)
    register(server)
    deregister_tools(server, "streamable-http")
    app = server.streamable_http_app()
    app.add_middleware(HttpBoundaryMiddleware)
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
