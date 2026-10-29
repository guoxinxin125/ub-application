"""Local tests only: synthetic durations are not hardware measurements."""

import ast
import asyncio
import importlib.util
import os
import json
import time
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch, AsyncMock, mock_open


ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


profiler = load("server_profile")
installer = load("install_profile")
summary = load("summarize_profile")


class ProfileTests(unittest.TestCase):
    def test_client_warmup_is_excluded_and_session_is_reused(self):
        tree = ast.parse((ROOT/"scripts/run_mcp.py").read_text(encoding="utf-8"))
        node = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == "calls")
        ns = {"time": time, "json": json}
        exec(compile(ast.Module(body=[node], type_ignores=[]), "<client-test>", "exec"), ns)
        raw = {"structuredContent": {"results": [], "errors": [], "mcp_scan_results": {
            "baseline_profile": {"tool_total_ns": 1, "phases_ns": {"other_tool_ns": 1}}}}}
        response = SimpleNamespace(isError=False, model_dump=lambda **kw: raw)
        session = SimpleNamespace(initialize=AsyncMock(),
                                  list_tools=AsyncMock(return_value=SimpleNamespace(tools=[SimpleNamespace(name="semgrep_scan_with_custom_rule")])),
                                  call_tool=AsyncMock(return_value=response))
        options = SimpleNamespace(warmup=2, samples=3, profile=True, engine_time=False)
        samples = asyncio.run(ns["calls"](session, "source", "rule", "sample.py", options))
        self.assertEqual(len(samples), 3)
        self.assertEqual(session.call_tool.await_count, 5)
        self.assertEqual(session.initialize.await_count, 1)
        self.assertEqual([x["sample"] for x in samples], [0, 1, 2])
        del raw["structuredContent"]["mcp_scan_results"]["baseline_profile"]
        with self.assertRaises(RuntimeError):
            asyncio.run(ns["calls"](session, "source", "rule", "sample.py", options))

    def test_cleanup_is_inside_total_and_default_is_off(self):
        @profiler.custom_rule
        async def tool():
            try:
                with profiler.phase("scan_backend_ns"):
                    await asyncio.sleep(0)
                return SimpleNamespace(mcp_scan_results={})
            finally:
                with profiler.phase("cleanup_ns"):
                    pass
        with patch.dict(os.environ, {"SEMGREP_MCP_PROFILE": "0"}):
            self.assertEqual(asyncio.run(tool()).mcp_scan_results, {})
        with patch.dict(os.environ, {"SEMGREP_MCP_PROFILE": "1"}):
            data = asyncio.run(tool()).mcp_scan_results["baseline_profile"]
        self.assertIn("cleanup_ns", data["phases_ns"])
        self.assertEqual(sum(data["phases_ns"].values()), data["tool_total_ns"])

    def test_concurrent_requests_are_isolated(self):
        @profiler.custom_rule
        async def tool():
            await profiler.await_call("scan_backend_ns", asyncio.sleep, 0)
            return SimpleNamespace(mcp_scan_results={})
        async def run():
            return await asyncio.gather(tool(), tool())
        with patch.dict(os.environ, {"SEMGREP_MCP_PROFILE": "1"}):
            a, b = asyncio.run(run())
        self.assertNotEqual(a.mcp_scan_results["baseline_profile"]["request_id"],
                            b.mcp_scan_results["baseline_profile"]["request_id"])

    def test_error_clears_context(self):
        @profiler.custom_rule
        async def tool():
            raise ValueError("expected")
        with patch.dict(os.environ, {"SEMGREP_MCP_PROFILE": "1"}):
            with self.assertRaises(ValueError):
                asyncio.run(tool())
        self.assertIsNone(profiler._current.get())

    def test_transform_preserves_other_functions_and_refuses_twice(self):
        path = ROOT.parents[1]/"semgrep/cli/src/semgrep/mcp/server.py"
        if not path.exists():
            self.skipTest("requires the local Semgrep source checkout")
        source = path.read_text(encoding="utf-8")
        result = installer.transform(source)
        original_functions = {x.name: ast.dump(x) for x in ast.parse(source).body
                              if isinstance(x, (ast.FunctionDef, ast.AsyncFunctionDef)) and x.name != installer.TARGET}
        after = {x.name: ast.dump(x) for x in ast.parse(result).body
                 if isinstance(x, (ast.FunctionDef, ast.AsyncFunctionDef)) and x.name != installer.TARGET}
        self.assertEqual(original_functions, after)
        self.assertIn("scan_output", result)
        with self.assertRaises(ValueError):
            installer.transform(result)

    def test_ratios_use_paired_sums(self):
        records = [{"call_elapsed_ns": 100, "server_profile": {
            "tool_total_ns": 60, "phases_ns": {"scan_backend_ns": 50, "other_tool_ns": 10}}},
            {"call_elapsed_ns": 300, "server_profile": {
            "tool_total_ns": 240, "phases_ns": {"scan_backend_ns": 200, "other_tool_ns": 40}}}]
        result = summary.summarize(records)
        self.assertEqual(result["phases"]["outside_tool_ns"]["share_percent"], 25)
        self.assertEqual(result["phases"]["scan_backend_ns"]["share_percent"], 62.5)
        records[0]["call_elapsed_ns"] = 10
        with self.assertRaises(ValueError):
            summary.summarize(records)

    def test_transformed_real_function_runs_and_includes_cleanup(self):
        path = ROOT.parents[1]/"semgrep/cli/src/semgrep/mcp/server.py"
        if not path.exists():
            self.skipTest("requires the local Semgrep source checkout")
        source = path.read_text(encoding="utf-8")
        transformed = ast.parse(installer.transform(source))
        node = next(x for x in transformed.body if isinstance(x, ast.AsyncFunctionDef) and x.name == installer.TARGET)
        model = SimpleNamespace(model_validate_json=lambda value: SimpleNamespace(mcp_scan_results={}))
        noop = lambda *a, **kw: None
        namespace = dict(_baseline_profile=profiler, with_tool_span=lambda: lambda f: f,
                         Context=object, CodeFile=object, REMOTE_CODE_FILES_FIELD=[], RULE_FIELD="",
                         SemgrepScanResult=model, get_workspace_dir=AsyncMock(return_value=None),
                         validate_remote_files=lambda files: files,
                         create_temp_files_from_code_content=lambda files: "/unused-test-path",
                         os=os, ENCODING="utf-8", ENCODING_ERRORS="strict",
                         get_semgrep_scan_args=lambda *a: ["scan"],
                         run_semgrep_output=AsyncMock(return_value='{}'),
                         findings_elicitation_enabled=lambda: False,
                         logger=SimpleNamespace(info=noop), attach_scan_metrics=noop,
                         get_current_span=noop, remove_temp_dir_from_results=noop,
                         shutil=SimpleNamespace(rmtree=noop), McpError=RuntimeError,
                         ValidationError=ValueError)
        exec(compile(ast.Module(body=[node], type_ignores=[]), "<instrumented-test>", "exec"), namespace)
        with patch.dict(os.environ, {"SEMGREP_MCP_PROFILE": "1"}), patch("builtins.open", mock_open()):
            result = asyncio.run(namespace[installer.TARGET](None, [], "rules: []"))
        phases = result.mcp_scan_results["baseline_profile"]["phases_ns"]
        self.assertTrue({"input_prepare_ns", "rule_write_ns", "scan_backend_ns", "result_decode_ns", "cleanup_ns"}.issubset(phases))

    def test_native_time_is_optional_and_nested(self):
        async def run():
            @profiler.custom_rule
            async def tool():
                async def backend(**kwargs):
                    self.assertIn("--time", kwargs["args"])
                    return '{"time": {"rules_parse_time": 0.001}}'
                await profiler.scan_output(backend, args=["scan"])
                return SimpleNamespace(mcp_scan_results={})
            return await tool()
        with patch.dict(os.environ, {"SEMGREP_MCP_PROFILE": "1", "SEMGREP_MCP_PROFILE_ENGINE_TIME": "1"}):
            result = asyncio.run(run())
        self.assertEqual(result.mcp_scan_results["baseline_profile"]["engine_time"]["rules_parse_time"], 0.001)


if __name__ == "__main__":
    unittest.main()
