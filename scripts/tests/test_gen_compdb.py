"""scripts/gen-compdb.py --rebuild-stale: a stale per-target database is rebuilt, and a failed build is a failure.

A section that changes build flags and rebuilds one target leaves the other targets' databases stale, which failed
the unattended preflight on 2026-11-11 (linux-x64-portable, windows-x64-portable). The build is stubbed here, so
these cases compile nothing.
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("rf_gen_compdb", SCRIPTS / "gen-compdb.py")
gc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gc)


class RebuildStaleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "src").mkdir()
        self.patches = [mock.patch.object(gc, "ROOT", self.root),
                        mock.patch.object(gc, "OUT", self.root / "compile_commands.json")]
        for patch in self.patches:
            patch.start()
        self.builds = []

    def tearDown(self):
        for patch in self.patches:
            patch.stop()
        self.tmp.cleanup()

    def store(self, target, entries):
        path = self.root / "build" / target / "compile_commands.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(entries), encoding="utf-8")

    def stale(self, target):
        entries = gc.expected_entries(target)
        entries[0]["arguments"] = [*entries[0]["arguments"], "-DSTALE_FLAG"]
        self.store(target, entries)

    def run_main(self, *argv, build=None):
        def fake_build(target):
            self.builds.append(target)
            return build(target) if build else 0
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(gc, "run_build", fake_build), mock.patch("sys.argv", ["gen-compdb.py", *argv]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gc.main()
        return code, out.getvalue() + err.getvalue()

    def test_rebuilds_exactly_the_stale_target(self):
        self.store("linux-x64", gc.expected_entries("linux-x64"))
        self.stale("linux-x64-portable")
        code, text = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("linux-x64-portable does not match build.py", text)
        self.assertIn("python3 scripts/build.py --target linux-x64 --portable", text)

        def good(target):
            self.store(target, gc.expected_entries(target))
            return 0
        code, text = self.run_main("--rebuild-stale", build=good)
        self.assertEqual(code, 0, text)
        self.assertEqual(self.builds, ["linux-x64-portable"])
        self.assertTrue((self.root / "compile_commands.json").is_file())
        self.assertEqual(self.run_main("--check")[0], 0)

    def test_a_failed_build_fails_even_when_its_database_now_matches(self):
        self.stale("windows-x64")

        def writes_then_fails(target):
            self.store(target, gc.expected_entries(target))
            return 3
        code, text = self.run_main("--rebuild-stale", build=writes_then_fails)
        self.assertEqual(code, 1)
        self.assertIn("windows-x64 (build exit 3)", text)
        self.assertFalse((self.root / "compile_commands.json").exists(), "a failed repair publishes no root database")

    def test_a_build_that_leaves_the_database_stale_fails(self):
        self.stale("linux-x64")
        code, text = self.run_main("--rebuild-stale")
        self.assertEqual(code, 1)
        self.assertIn("linux-x64 (still stale after its build)", text)

    def test_nothing_stale_builds_nothing(self):
        self.store("linux-x64", gc.expected_entries("linux-x64"))
        code, _ = self.run_main("--rebuild-stale")
        self.assertEqual(code, 0)
        self.assertEqual(self.builds, [])

    def test_build_argv_carries_portable_only_for_a_portable_directory(self):
        self.assertEqual(gc.build_argv("windows-x64-portable")[2:], ["--target", "windows-x64", "--portable"])
        self.assertEqual(gc.build_argv("android-arm64")[2:], ["--target", "android-arm64"])


if __name__ == "__main__":
    unittest.main()
