#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Keep stateful fixtures native and block the retired source-extraction patterns."""
import ast
from pathlib import Path
import re
import unittest
import importlib.util
import tempfile

TESTS = Path(__file__).resolve().parents[1]
HOST_TESTS = {
    "test_scoped_persist.py", "test_native_process.py",
    "test_preview_timing.py", "test_fixture_structure.py",
}
SCAFFOLDING = {
    "engine_harness.py", "engine_scheduler.h", "conditions_harness.c",
    "selection_refresh_harness.c",
}
# These are test-owned pair-provider records, not replicas of production types.
TEST_OWNED_TYPES = {"kp_rgb_pair_config", "kp_rgb_pair_data"}
PLATFORM_TYPES = {
    "device", "k_work", "k_work_delayable", "zmk_behavior_binding",
    "zmk_behavior_binding_event",
}
SOURCE_SUFFIXES = {".py", ".c", ".h", ".inc"}
BUILD_DIRS = {"artifacts", "__pycache__", ".pytest_cache", "CMakeFiles", "zephyr"}


def sources():
    for path in TESTS.rglob("*"):
        relative = path.relative_to(TESTS)
        if any(part in BUILD_DIRS or part.startswith(("build", "out"))
               for part in relative.parts[:-1]):
            continue
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def _path_parts(node, bindings, seen=None):
    """Resolve string path components through simple assignments and `/`."""
    seen = set() if seen is None else seen
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [node.value]
    if isinstance(node, ast.Name) and node.id in bindings and node.id not in seen:
        return _path_parts(bindings[node.id], bindings, seen | {node.id})
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        return _path_parts(node.left, bindings, seen) + _path_parts(node.right, bindings, seen)
    if isinstance(node, ast.Attribute):
        return _path_parts(node.value, bindings, seen)
    if isinstance(node, ast.Subscript):
        return _path_parts(node.value, bindings, seen)
    if isinstance(node, ast.Call):
        if isinstance(node.func, ast.Name) and node.func.id == "Path" and node.args:
            return _path_parts(node.args[0], bindings, seen)
        if isinstance(node.func, ast.Attribute):
            parts = _path_parts(node.func.value, bindings, seen)
            if node.func.attr == "joinpath":
                for argument in node.args:
                    parts += _path_parts(argument, bindings, seen)
            return parts
    return []


def reads_production_source(tree):
    """Return line numbers of reads resolving to module src/include C or headers."""
    bindings = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    bindings[target.id] = node.value
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            bindings[node.target.id] = node.value
    reads = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        if isinstance(node.func, ast.Name) and node.func.id == "open" and node.args:
            parts = _path_parts(node.args[0], bindings)
        elif isinstance(node.func, ast.Attribute) and node.func.attr in {"read_text", "read_bytes", "open"}:
            parts = _path_parts(node.func.value, bindings)
        else:
            continue
        path = "/".join(parts).replace("\\", "/")
        if re.search(r"(?:^|/)zephyr/include/generated/", path):
            continue
        if re.search(r"(?:^|/)(?:src|include)/.*\.(?:c|h)(?:$|/)", path):
            reads.append(node.lineno)
    return reads


def imported_test_suites(tree):
    """Return (line, module) pairs for imports that cross into a test suite."""
    imports = []
    for node in ast.walk(tree):
        modules = ([node.module] if isinstance(node, ast.ImportFrom) else
                   [alias.name for alias in node.names] if isinstance(node, ast.Import) else [])
        for module in modules:
            if module and any(part.startswith("test_") for part in module.split(".")):
                imports.append((node.lineno, module))
    return imports


def fixture_violations(text, path):
    """Check C/H/INC fixture text for copied declarations and API substitutes."""
    failures = []
    # Definitions only: uses, includes, and designated initializers are fine.
    for match in re.finditer(r"\b(?:struct|enum)\s+([A-Za-z_]\w*)\s*\{", text):
        name = match.group(1)
        if name.startswith("kp_rgb_") and name not in TEST_OWNED_TYPES:
            failures.append(f"{path}: copied production type {name}")
        elif name in PLATFORM_TYPES:
            failures.append(f"{path}: hand-defined platform type {name}")

    # Only macro replacements are substitutes; ordinary DT/K_WORK API use is real.
    for match in re.finditer(
            r"(?m)^\s*#\s*define\s+(DT_(?!DRV_COMPAT\b)[A-Z0-9_]+|DEVICE_DT_[A-Z0-9_]+|K_WORK_[A-Za-z0-9_]+)\b",
            text):
        failures.append(f"{path}: substitute core macro {match.group(1)}")

    # Match function bodies, not calls or prototypes. Wrappers are observers and
    # intentionally retain their definitions while delegating to real APIs.
    function_definition = re.compile(
        r"(?m)^\s*(?:static\s+|inline\s+|extern\s+)*"
        r"[A-Za-z_]\w*(?:\s+|\s*\*\s*)(k_work_[A-Za-z0-9_]+|__wrap_k_work_[A-Za-z0-9_]+)\s*"
        r"\([^;{}]*\)\s*\{"
    )
    for match in function_definition.finditer(text):
        name = match.group(1)
        if not name.startswith("__wrap_"):
            failures.append(f"{path}: substitute core work function {name}")
    return failures


class FixtureStructureTests(unittest.TestCase):
    def test_stage14_kconfig_contract(self):
        module = TESTS.parent
        try:
            import kconfiglib as parser
        except ImportError:
            parser_path = module.parents[1] / "zmk/zephyr/scripts/kconfig/kconfiglib.py"
            if not parser_path.is_file():
                self.skipTest("Stage14 Kconfig checks need kconfiglib or an existing ZMK workspace")
            spec = importlib.util.spec_from_file_location("stage14_kconfiglib", parser_path)
            parser = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(parser)
        symbols = (
            "DT_HAS_ZMK_LED_STRIP_COMPOSITE_ENABLED", "DT_HAS_KEYPAW_RGB_MATRIX_ENABLED",
            "DT_HAS_KEYPAW_RGB_CONDITION_CAPS_LOCK_ENABLED", "DT_HAS_KEYPAW_RGB_CONDITION_BLE_ENABLED",
            "DT_HAS_KEYPAW_RGB_TRIGGER_TABLE_ENABLED", "ZMK_SPLIT", "ZMK_SPLIT_ROLE_CENTRAL",
            "LED_STRIP", "ZMK_HID_INDICATORS")
        advanced = {
            "KEYPAW_RGB_WORKQUEUE_PRIORITY": "10", "KEYPAW_RGB_MATRIX_HUE_STEP": "10",
            "KEYPAW_RGB_MATRIX_SAT_STEP": "10", "KEYPAW_RGB_MATRIX_BRT_STEP": "10",
            "KEYPAW_RGB_MATRIX_DURATION_STEP": "100", "KEYPAW_RGB_MATRIX_DURATION_MIN_MS": "100",
            "KEYPAW_RGB_MATRIX_DURATION_MAX_MS": "10000", "KEYPAW_RGB_SPLIT_SYNC_POLL_MS": "1000",
            "KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS": "1000", "KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS": "30000"}
        with tempfile.TemporaryDirectory(prefix="rgb-kconfig-") as directory:
            wrapper = Path(directory) / "Kconfig"
            for matrix, table, split, central in (
                    (True, False, False, False), (True, True, False, False),
                    (True, True, True, False), (True, True, True, True),
                    (True, False, True, True), (False, True, True, True),
                    (False, False, False, False)):
                enabled = {"DT_HAS_ZMK_LED_STRIP_COMPOSITE_ENABLED"}
                for present, name in ((matrix, "DT_HAS_KEYPAW_RGB_MATRIX_ENABLED"),
                                      (table, "DT_HAS_KEYPAW_RGB_TRIGGER_TABLE_ENABLED"),
                                      (split, "ZMK_SPLIT"), (central, "ZMK_SPLIT_ROLE_CENTRAL")):
                    if present:
                        enabled.add(name)
                wrapper.write_text("\n".join(
                    f"config {name}\n    bool\n    default {'y' if name in enabled else 'n'}"
                    for name in symbols) + f'\nsource "{module / "Kconfig"}"\n')
                config = parser.Kconfig(str(wrapper), warn_to_stderr=False)
                for name, expected in (("KEYPAW_RGB_TRIGGERS", matrix and table and (not split or central)),
                                       ("KEYPAW_RGB_SPLIT_SYNC", matrix and split and central)):
                    symbol = config.syms[name]
                    self.assertTrue(all(node.prompt is None for node in symbol.nodes), name)
                    self.assertEqual(symbol.str_value, "y" if expected else "n", name)
                    self.assertEqual(symbol.assignable, (), name)
                self.assertEqual(config.syms["LED_STRIP_COMPOSITE"].str_value, "y")
                self.assertEqual(config.syms["LED_STRIP_COMPOSITE_INIT_PRIORITY"].str_value, "91")
                config.syms["LED_STRIP_COMPOSITE_INIT_PRIORITY"].set_value("90")
                self.assertEqual(config.syms["LED_STRIP_COMPOSITE_INIT_PRIORITY"].str_value, "90")
                if not matrix:
                    continue
                for name, default in (("KEYPAW_RGB_MATRIX_TICK_MS", "32"),
                                      ("KEYPAW_RGB_WORKQUEUE_STACK_SIZE", "2048")):
                    symbol = config.syms[name]
                    self.assertEqual(symbol.str_value, default, name)
                    self.assertTrue(symbol.nodes[0].prompt, name)
                    self.assertEqual(symbol.nodes[0].parent.item.name, "KEYPAW_RGB_MATRIX", name)
                for name, default in advanced.items():
                    if "SPLIT_SYNC_" in name and not (split and central):
                        continue
                    symbol = config.syms[name]
                    self.assertEqual(symbol.str_value, default, name)
                    self.assertIn("Advanced RGB", symbol.nodes[0].parent.prompt[0], name)
                    symbol.set_value(str(int(default) + 1))
                    self.assertEqual(symbol.str_value, str(int(default) + 1), name)
                self.assertEqual(config.warnings, [])

    def test_retired_startup_controls_are_absent(self):
        module = TESTS.parent
        paths = set(sources())
        for directory in (module / "src", module / "include", module / "examples"):
            if directory.exists():
                paths.update(path for path in directory.rglob("*") if path.is_file())
        paths.update(module.glob("Kconfig*"))
        for path in (TESTS / "sim").rglob("*"):
            relative = path.relative_to(TESTS)
            if (path.is_file() and path.suffix in {".conf", ".overlay", ".keymap", ".yaml", ".yml"}
                    and not any(part in BUILD_DIRS or part.startswith(("build", "out"))
                                for part in relative.parts[:-1])):
                paths.add(path)
        retired = re.compile(r"\b(?:zmk_rgb_matrix_start|CONFIG_KEYPAW_RGB_CONTROL_AUTO_START|KEYPAW_RGB_CONTROL_AUTO_START)\b")
        failures = []
        for path in sorted(paths):
            if path == Path(__file__).resolve() or path.suffix == ".md":
                continue
            for line, text in enumerate(path.read_text().splitlines(), 1):
                if retired.search(text):
                    failures.append(f"{path.relative_to(module)}:{line}: {text.strip()}")
        self.assertEqual(failures, [], "Startup must use the mandatory automatic boundary:\n" + "\n".join(failures))

    def test_only_small_host_helpers_remain(self):
        actual = {p.name for p in (TESTS / "unit").glob("test_*.py")}
        self.assertEqual(actual, HOST_TESTS,
                         f"unexpected unit test suites: {sorted(actual - HOST_TESTS)}; missing: {sorted(HOST_TESTS - actual)}")
        present = sorted(str(p.relative_to(TESTS)) for p in sources() if p.name in SCAFFOLDING)
        self.assertEqual(present, [], "Retired extraction scaffolding remains checked in: " + ", ".join(present))

    def test_python_fixtures_do_not_extract_production_source(self):
        failures = []
        for path in sources():
            if path.suffix != ".py":
                continue
            tree = ast.parse(path.read_text(), filename=str(path))
            failures.extend(f"{path.relative_to(TESTS)}:{line}: reads production C/header source"
                            for line in reads_production_source(tree))
        self.assertEqual(failures, [], "Compile production sources and headers instead:\n" + "\n".join(failures))

    def test_python_fixtures_do_not_import_test_suites(self):
        failures = []
        for path in sources():
            if path.suffix != ".py":
                continue
            tree = ast.parse(path.read_text(), filename=str(path))
            failures.extend(f"{path.relative_to(TESTS)}:{line}: imports {module}"
                            for line, module in imported_test_suites(tree))
        self.assertEqual(failures, [], "Shared fixtures belong in a non-test helper module:\n" + "\n".join(failures))

    def test_fixture_text_uses_real_types_and_platform_apis(self):
        failures = []
        for path in sources():
            failures.extend(fixture_violations(path.read_text(), path.relative_to(TESTS)))
        self.assertEqual(failures, [], "Use production includes and Zephyr APIs:\n" + "\n".join(failures))

    def test_guard_recognizes_forbidden_and_permitted_examples(self):
        python_bad = "from tests.unit.test_suite import case\npath = ROOT / 'src/rgb_matrix.c'\ntext = path.read_text()\n"
        tree = ast.parse(python_bad)
        self.assertTrue(reads_production_source(tree))
        self.assertTrue(imported_test_suites(tree))
        for expression in ("open(ROOT / 'src/foo.c')", "Path('include/zmk/foo.h').read_text()",
                           "ROOT.joinpath('src', 'foo.c').read_bytes()"):
            self.assertTrue(reads_production_source(ast.parse(expression)), expression)
        generated = ast.parse("header = build / 'zephyr/include/generated/zephyr/devicetree_generated.h'\nheader.read_text()\n")
        self.assertEqual(reads_production_source(generated), [])

        c_bad = "struct kp_rgb_" + "hsb { int copied; };\nstruct " + "device { int copied; };\n"
        c_bad += "#define DT_PROP(node, prop) 0\n# define K_WORK_DEFINE(name, handler)\n"
        c_bad += "#define DT_INST_PROP(inst, prop) 0\n"
        c_bad += "int k_work_submit(struct k_work *work) { return 0; }\n"
        violations = fixture_violations(c_bad, "snippet.inc")
        for expected in ("kp_rgb_hsb", "device", "DT_PROP", "DT_INST_PROP", "K_WORK_DEFINE", "k_work_submit"):
            self.assertTrue(any(expected in violation for violation in violations), expected)

        legal = """#include <zmk/rgb_matrix.h>
int real_work(void) { return k_work_submit(&work); }
#define DT_DRV_COMPAT test_native_provider
#if DT_PROP(node, value)
#endif
K_WORK_DEFINE(work, handler);
int __wrap_k_work_submit(struct k_work *work) { return k_work_submit(work); }
"""
        legal_tree = ast.parse("subprocess.run(['cc', 'fixture.c'], timeout=10)\n")
        dts_compile = ast.parse("subprocess.run(['west', 'build', '-DDTC_OVERLAY_FILE=fixture.overlay'], timeout=10)\n")
        self.assertEqual(fixture_violations(legal, "fixture.c"), [])
        self.assertEqual(reads_production_source(legal_tree), [])
        self.assertEqual(reads_production_source(dts_compile), [])
        self.assertTrue(any(path.suffix == ".inc" for path in sources()), "native .inc inventory must be scanned")


if __name__ == "__main__":
    unittest.main()
