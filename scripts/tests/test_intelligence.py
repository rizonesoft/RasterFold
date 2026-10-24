#!/usr/bin/env python3
"""Unit tests for scripts/intelligence.py and stopped-boundary enforcement."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))

import campaign_guard  # noqa: E402
import intelligence  # noqa: E402


def make_workspace(tmp: str) -> tuple[Path, str]:
    root = Path(tmp)
    for family in intelligence.FAMILIES:
        reg_dir = root / ".intelligence" / family
        reg_dir.mkdir(parents=True, exist_ok=True)
        (reg_dir / "register.json").write_text(
            json.dumps(
                {"schema": 1, "family": family, "next_id": 1, "entries": [], "archive": []},
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
    (root / "README.md").write_text("seed\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(root), "add", "README.md"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "-c",
            "user.name=Test",
            "-c",
            "user.email=test@example.com",
            "commit",
            "-q",
            "-m",
            "seed",
        ],
        check=True,
    )
    head = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
    ).strip()
    return root, head


class IntelligenceTests(unittest.TestCase):
    def test_repository_registers_and_docs_hold(self):
        self.assertEqual(intelligence.main(["validate"]), 0)
        readme = (ROOT / ".intelligence/README.md").read_text(encoding="utf-8")
        skill = (ROOT / ".agents/skills/process-intelligence/SKILL.md").read_text(
            encoding="utf-8"
        )
        for text in (readme, skill):
            self.assertIn("scripts/intelligence.py --family claude list", text)
            self.assertIn("scripts/intelligence.py --family codex list", text)
            self.assertIn("--family <claude|codex> capture", text)
            self.assertIn("--family <claude|codex> apply", text)
            self.assertIn("--family <claude|codex> reject", text)
            self.assertIn("--family <claude|codex> defer", text)
            self.assertIn("--family <claude|codex> archive", text)
            self.assertNotIn(".codex/runner/intelligence.py", text)
            self.assertNotIn(".claude/runner/intelligence.py", text)

    def test_family_isolation_with_identical_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, head = make_workspace(tmp)
            c_row = intelligence.update(
                root,
                "claude",
                "capture",
                title="Claude item",
                evidence="ev-claude",
                proposal="prop-claude",
                owner="D00 T04 §4",
            )
            x_row = intelligence.update(
                root,
                "codex",
                "capture",
                title="Codex item",
                evidence="ev-codex",
                proposal="prop-codex",
                owner="D00 T04 §4",
            )
            self.assertEqual(c_row["id"], 1)
            self.assertEqual(x_row["id"], 1)
            claude_path = root / ".intelligence/claude/register.json"
            claude_before = claude_path.read_bytes()

            intelligence.update(
                root,
                "codex",
                "apply",
                ident=1,
                commit=head,
                reason="applied to codex only",
            )
            intelligence.update(
                root,
                "codex",
                "archive",
                ident=1,
                reason="archived codex id 1",
            )
            self.assertEqual(claude_path.read_bytes(), claude_before)
            claude_reg = intelligence.read_register(root, "claude")
            codex_reg = intelligence.read_register(root, "codex")
            self.assertEqual(len(claude_reg["entries"]), 1)
            self.assertEqual(claude_reg["entries"][0]["status"], "open")
            self.assertEqual(len(codex_reg["entries"]), 0)
            self.assertEqual(len(codex_reg["archive"]), 1)
            self.assertEqual(codex_reg["archive"][0]["status"], "applied")

    def test_capture_deduplication_disposition_archive_and_recurrence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, head = make_workspace(tmp)
            first = intelligence.update(
                root,
                "claude",
                "capture",
                title="Hook timeout",
                evidence="log:1",
                proposal="bound read",
                owner="D00 T04 §4",
            )
            dup = intelligence.update(
                root,
                "claude",
                "capture",
                title="Hook timeout",
                evidence="log:1",
                proposal="different proposal",
                owner="D00 T04 §4",
            )
            self.assertEqual(first["id"], dup["id"])
            self.assertEqual(len(intelligence.read_register(root, "claude")["entries"]), 1)

            deferred = intelligence.update(
                root,
                "claude",
                "defer",
                ident=1,
                reason="waiting on D00 T04 §4",
            )
            self.assertEqual(deferred["status"], "deferred")

            reg_path = root / ".intelligence/claude/register.json"
            before_bad_archive = reg_path.read_bytes()
            with self.assertRaisesRegex(ValueError, "open or deferred intelligence cannot be archived"):
                intelligence.update(root, "claude", "archive", ident=1, reason="too early")
            self.assertEqual(reg_path.read_bytes(), before_bad_archive)

            before_bad_apply = reg_path.read_bytes()
            with self.assertRaises((ValueError, subprocess.SubprocessError)):
                intelligence.update(
                    root,
                    "claude",
                    "apply",
                    ident=1,
                    commit="deadbeef" * 5,
                    reason="unreachable commit",
                )
            for bad_action in ("defer", "reject"):
                with self.assertRaisesRegex(ValueError, f"{bad_action} does not accept --commit"):
                    intelligence.update(
                        root,
                        "claude",
                        bad_action,
                        ident=1,
                        commit=head,
                        reason="incompatible commit",
                    )
            with self.assertRaisesRegex(ValueError, "capture does not accept --id, --reason or --commit"):
                intelligence.update(
                    root,
                    "claude",
                    "capture",
                    title="T",
                    evidence="E",
                    proposal="P",
                    owner="D00 T04 §4",
                    commit=head,
                )
            with self.assertRaisesRegex(ValueError, "defer does not accept --title, --evidence, --proposal or --owner"):
                intelligence.update(
                    root,
                    "claude",
                    "defer",
                    ident=1,
                    reason="bad extra title",
                    title="Unexpected",
                )
            self.assertEqual(reg_path.read_bytes(), before_bad_apply)

            rejected = intelligence.update(
                root,
                "claude",
                "reject",
                ident=1,
                reason="reproduced and found false premise",
            )
            self.assertEqual(rejected["status"], "rejected")

            before_bad_archive_commit = reg_path.read_bytes()
            with self.assertRaisesRegex(ValueError, "archive does not accept --commit"):
                intelligence.update(
                    root,
                    "claude",
                    "archive",
                    ident=1,
                    commit=head,
                    reason="incompatible commit on archive",
                )
            self.assertEqual(reg_path.read_bytes(), before_bad_archive_commit)

            archived = intelligence.update(
                root,
                "claude",
                "archive",
                ident=1,
                reason="closed rejected entry",
            )
            self.assertEqual(archived["history"][-1]["action"], "archive")

            recur = intelligence.update(
                root,
                "claude",
                "capture",
                title="Hook timeout",
                evidence="log:1",
                proposal="bound read v2",
                owner="D00 T04 §4",
            )
            self.assertEqual(recur["id"], 2)
            self.assertEqual(len(recur["history"]), 1)
            self.assertEqual(recur["history"][0]["action"], "recurrence")
            self.assertEqual(recur["history"][0]["previous_id"], 1)
            self.assertEqual(intelligence.main(["--root", str(root), "validate"]), 0)

    def test_malformed_registers_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, _ = make_workspace(tmp)
            reg_path = root / ".intelligence/claude/register.json"
            good_key = hashlib.sha256(b"T\nE").hexdigest()
            good_entry = {
                "id": 1,
                "key": good_key,
                "title": "T",
                "evidence": "E",
                "proposal": "P",
                "owner": "D00 T04 §4",
                "status": "open",
                "created": "2026-10-24T00:00:00Z",
                "history": [],
            }
            bad_cases = [
                {"schema": 2, "family": "claude", "next_id": 1, "entries": [], "archive": []},
                {"schema": 1, "family": "codex", "next_id": 1, "entries": [], "archive": []},
                {"schema": 1, "family": "claude", "next_id": True, "entries": [], "archive": []},
                {"schema": 1, "family": "claude", "next_id": 2, "entries": [{**good_entry, "key": "0" * 64}], "archive": []},
                {"schema": 1, "family": "claude", "next_id": 2, "entries": [{**good_entry, "status": "applied"}], "archive": []},
                {
                    "schema": 1,
                    "family": "claude",
                    "next_id": 2,
                    "entries": [
                        {
                            **good_entry,
                            "status": "applied",
                            "history": [
                                {
                                    "action": "defer",
                                    "at": "2026-10-24T00:00:00Z",
                                    "reason": "only deferred",
                                    "commit": "",
                                }
                            ],
                        }
                    ],
                    "archive": [],
                },
                {
                    "schema": 1,
                    "family": "claude",
                    "next_id": 2,
                    "entries": [
                        {
                            **good_entry,
                            "status": "open",
                            "history": [
                                {
                                    "action": "defer",
                                    "at": "2026-10-24T00:00:00Z",
                                    "reason": "wrong status",
                                    "commit": "",
                                }
                            ],
                        }
                    ],
                    "archive": [],
                },
                {
                    "schema": 1,
                    "family": "claude",
                    "next_id": 2,
                    "entries": [
                        {
                            **good_entry,
                            "status": "open",
                            "history": [{"action": "recurrence", "at": "2026-10-24T00:00:00Z", "previous_id": 1}],
                        }
                    ],
                    "archive": [],
                },
                {"schema": 1, "family": "claude", "next_id": 2, "entries": [], "archive": [good_entry]},
                {
                    "schema": 1,
                    "family": "claude",
                    "next_id": 2,
                    "entries": [],
                    "archive": [
                        {
                            **good_entry,
                            "status": "applied",
                            "history": [{"action": "apply", "at": "2026-10-24T00:00:00Z", "reason": "ok", "commit": "abc"}],
                        }
                    ],
                },
            ]
            for bad in bad_cases:
                reg_path.write_text(json.dumps(bad), encoding="utf-8")
                with self.assertRaises(ValueError):
                    intelligence.read_register(root, "claude")
                self.assertEqual(intelligence.main(["--root", str(root), "validate"]), 2)

            reg_path.unlink()
            self.assertEqual(intelligence.main(["--root", str(root), "validate"]), 2)

    def test_stopped_boundary_and_continuous_lock_lifetime(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, _ = make_workspace(tmp)
            intelligence.update(
                root,
                "claude",
                "capture",
                title="Item 1",
                evidence="ev-1",
                proposal="prop-1",
                owner="D00 T04 §4",
            )
            reg_path = root / ".intelligence/claude/register.json"
            baseline = reg_path.read_bytes()
            build_dir = root / "build"
            build_dir.mkdir(parents=True, exist_ok=True)

            # Capture is permitted while guard is active, but disposition is refused for every sentinel.
            for sentinel in intelligence.SENTINELS:
                spath = build_dir / sentinel
                spath.write_text("{}", encoding="utf-8")
                try:
                    with self.assertRaisesRegex(ValueError, "campaign guard is active"):
                        intelligence.update(
                            root, "claude", "defer", ident=1, reason="blocked by " + sentinel
                        )
                    self.assertEqual(reg_path.read_bytes(), baseline)
                finally:
                    spath.unlink()

            # Unreadable or non-directory build path refuses disposition and leaves register untouched.
            with mock.patch("intelligence.os.listdir", side_effect=PermissionError("unreadable build dir")):
                with self.assertRaisesRegex(ValueError, "campaign build directory is unreadable"):
                    intelligence.update(
                        root, "claude", "defer", ident=1, reason="blocked by unreadable build"
                    )
            self.assertEqual(reg_path.read_bytes(), baseline)

            # _Lock.__enter__ releases the in-process thread lock if os.makedirs fails, so a subsequent
            # disposition in the same process still succeeds once the directory is creatable.
            with mock.patch("campaign_guard.os.makedirs", side_effect=OSError("makedirs failed")):
                with self.assertRaisesRegex(OSError, "makedirs failed"):
                    intelligence.update(
                        root, "claude", "defer", ident=1, reason="blocked by makedirs failure"
                    )
            self.assertFalse(campaign_guard._Lock(str(root)).thread_lock.locked())
            self.assertEqual(reg_path.read_bytes(), baseline)

            # Held lock refuses disposition and leaves register untouched.
            with campaign_guard._Lock(str(root), wait=1.0):
                with self.assertRaises(ValueError):
                    intelligence.update(
                        root, "claude", "defer", ident=1, reason="lock held", lock_wait=0.05
                    )
                self.assertEqual(reg_path.read_bytes(), baseline)

            # Deterministic interleaving probe: while update() is between require_idle() and save_register(),
            # a competing thread attempting to acquire the campaign guard lock must be refused because update()
            # holds one continuous _Lock across idle check, read, mutation, and save.
            interleave_errors: list[BaseException] = []

            def competing_acquire() -> None:
                def worker() -> None:
                    try:
                        with campaign_guard._Lock(str(root), wait=0.05):
                            pass
                    except BaseException as exc:
                        interleave_errors.append(exc)

                t = threading.Thread(target=worker)
                t.start()
                t.join(timeout=2.0)

            updated = intelligence.update(
                root,
                "claude",
                "defer",
                ident=1,
                reason="continuous lock held",
                _after_idle_hook=competing_acquire,
            )
            self.assertEqual(updated["status"], "deferred")
            self.assertEqual(len(interleave_errors), 1)
            self.assertIsInstance(interleave_errors[0], campaign_guard.GuardError)

            # Atomic write failure leaves register bytes unchanged and cleans up temporary file.
            after_defer = reg_path.read_bytes()
            with mock.patch("intelligence.os.fsync", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"):
                    intelligence.update(
                        root, "claude", "reject", ident=1, reason="should roll back"
                    )
            self.assertEqual(reg_path.read_bytes(), after_defer)
            self.assertEqual(list(reg_path.parent.glob("*.tmp")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
