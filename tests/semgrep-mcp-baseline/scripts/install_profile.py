#!/usr/bin/env python3
"""Instrument only custom-rule calls, preserving host/tmpfs/auth modifications.

Default is check-only. --apply targets the active Python environment. Run with
the same private venv as semgrep, never sudo. Restart the MCP server afterwards.
"""

import argparse
import ast
import hashlib
import importlib.util
from pathlib import Path
import shutil


MARKER = "from semgrep.mcp import baseline_profile as _baseline_profile"
TARGET = "semgrep_scan_with_custom_rule"
PHASES = {
    "get_workspace_dir": "workspace_ns",
    "validate_remote_files": "validation_ns",
    "create_temp_files_from_code_content": "input_prepare_ns",
    "get_semgrep_scan_args": "scan_args_ns",
    "SemgrepScanResult.model_validate_json": "result_decode_ns",
    "findings_elicitation": "elicitation_ns",
    "attach_findings_metrics": "findings_metrics_ns",
    "attach_scan_metrics": "scan_metrics_ns",
    "remove_temp_dir_from_results": "normalize_ns",
    "shutil.rmtree": "cleanup_ns",
}


class Instrument(ast.NodeTransformer):
    def __init__(self):
        self.seen = set()

    def visit_Await(self, node):
        if isinstance(node.value, ast.Call):
            name = ast.unparse(node.value.func)
            if name in PHASES:
                self.seen.add(name)
                node.value = self.wrap(node.value, "await_call", PHASES[name])
                return node
        return self.generic_visit(node)

    def wrap(self, node, helper, phase=None):
        args = ([ast.Constant(phase)] if phase else []) + [node.func, *node.args]
        return ast.Call(func=ast.Attribute(value=ast.Name(id="_baseline_profile", ctx=ast.Load()),
                                          attr=helper, ctx=ast.Load()), args=args, keywords=node.keywords)

    def visit_Call(self, node):
        name = ast.unparse(node.func)
        if name == "run_semgrep_output":
            self.seen.add(name)
            return self.wrap(node, "scan_output")
        if name in PHASES:
            self.seen.add(name)
            return self.wrap(node, "call", PHASES[name])
        return self.generic_visit(node)

    def visit_With(self, node):
        node = self.generic_visit(node)
        if any(isinstance(x.context_expr, ast.Call) and
               ast.unparse(x.context_expr.func) == "open" for x in node.items):
            self.seen.add("rule_write")
            context = ast.parse("with _baseline_profile.phase('rule_write_ns'):\n    pass").body[0]
            context.body = [node]
            return context
        return node


def transform(source):
    """Fail closed if key anchors have changed; never rewrite other functions."""
    if MARKER in source:
        raise ValueError("already instrumented; do not apply twice")
    tree = ast.parse(source)
    nodes = [n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == TARGET]
    if len(nodes) != 1:
        raise ValueError("custom-rule function not found uniquely")
    node = nodes[0]
    start = min([node.lineno, *[d.lineno for d in node.decorator_list]])
    end = node.end_lineno
    instrument = Instrument()
    node = instrument.visit(node)
    required = {"run_semgrep_output", "create_temp_files_from_code_content",
                "SemgrepScanResult.model_validate_json", "shutil.rmtree", "rule_write"}
    if not required.issubset(instrument.seen):
        raise ValueError(f"unsupported server source: missing {required - instrument.seen}")
    node.decorator_list.append(ast.parse("_baseline_profile.custom_rule").body[0].value)
    lines = source.splitlines(keepends=True)
    replacement = MARKER + "\n\n" + ast.unparse(ast.fix_missing_locations(node)) + "\n"
    result = "".join(lines[:start-1]) + replacement + "".join(lines[end:])
    ast.parse(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--server", type=Path, help="Explicit server.py (otherwise active environment)")
    args = parser.parse_args()
    if args.server:
        target = args.server.resolve()
    else:
        spec = importlib.util.find_spec("semgrep")
        if spec is None or spec.origin is None:
            parser.error("activate the Semgrep private venv first")
        target = Path(spec.origin).parent / "mcp/server.py"
    source = target.read_text(encoding="utf-8")
    result = transform(source)
    print(f"PROFILE_CHECK_OK target={target}")
    if not args.apply:
        return
    digest = hashlib.sha256(target.read_bytes()).hexdigest()[:12]
    backup = target.with_name(f"server.py.pre-profile-{digest}.bak")
    if backup.exists():
        parser.error(f"backup exists: {backup}; inspect before reinstalling")
    shutil.copy2(target, backup)
    helper = Path(__file__).with_name("server_profile.py")
    shutil.copy2(helper, target.with_name("baseline_profile.py"))
    target.write_text(result, encoding="utf-8", newline="\n")
    print(f"PROFILE_INSTALLED backup={backup}; restart your MCP server")


if __name__ == "__main__":
    main()
