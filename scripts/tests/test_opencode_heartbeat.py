"""The OpenCode heartbeat job store: cron slots, one firing per slot, no backlog, expiry, refusals, fail-safe polling."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("campaign_guard_fixture", ROOT / "scripts/campaign_guard.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
CRON = "3-59/5 * * * *"


class OpenCodeHeartbeat(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(guard._workspace(self.tmp.name))
        self.script = self.root / "scripts/opencode-heartbeat.py"
        shutil.copyfile(ROOT / "scripts/opencode-heartbeat.py", self.script)
        self.prompt = self.root / "build/prompt.txt"
        self.prompt.write_text("OpenCode run-guard heartbeat for RasterFold Phase 3. Check, then act.\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *args, expect=0):
        result = subprocess.run([sys.executable, str(self.script), *args], capture_output=True, text=True,
                                cwd=self.root, timeout=60)
        self.assertEqual(result.returncode, expect, result.stderr)
        return result

    def create(self, now="2026-10-29T03:00:30Z", cron=CRON, session="ses_owner"):
        return self.run_cli("create", "--session", session, "--cron", cron, "--prompt-file", str(self.prompt),
                            "--now", now).stdout.strip()

    def due(self, now):
        return json.loads(self.run_cli("due", "--now", now).stdout)

    def test_the_3_59_5_minutes_fire_and_no_others(self):
        job = self.create()
        for minute, fires in (("00", False), ("03", True), ("04", False), ("08", True), ("53", True),
                              ("58", True), ("59", False)):
            with self.subTest(minute=minute):
                # Each poll follows a firing one minute earlier, so only this minute's own slot can match.
                self.run_cli("delete", "--job", job)
                job = self.create(now=f"2026-10-29T04:{int(minute) - 1:02d}:30Z" if minute != "00"
                                  else "2026-10-29T03:59:30Z")
                fired = self.due(f"2026-10-29T04:{minute}:10Z")
                if fires:
                    self.assertEqual([(entry["id"], entry["slot"]) for entry in fired],
                                     [(job, f"2026-10-29T04:{minute}:00Z")])
                else:
                    self.assertEqual(fired, [])

    def test_one_firing_per_slot_across_repeated_polls(self):
        job = self.create()
        self.assertEqual([entry["id"] for entry in self.due("2026-10-29T03:03:05Z")], [job])
        self.assertEqual(self.due("2026-10-29T03:03:35Z"), [])
        self.assertEqual(self.due("2026-10-29T03:07:59Z"), [])
        self.assertEqual([entry["id"] for entry in self.due("2026-10-29T03:08:01Z")], [job])

    def test_a_slot_before_creation_never_fires(self):
        self.create(now="2026-10-29T03:03:30Z")
        self.assertEqual(self.due("2026-10-29T03:03:50Z"), [])

    def test_a_long_gap_fires_once_at_the_latest_slot(self):
        self.create()
        fired = self.due("2026-10-29T09:51:00Z")
        self.assertEqual([entry["slot"] for entry in fired], ["2026-10-29T09:48:00Z"])
        self.assertEqual(self.due("2026-10-29T09:52:00Z"), [])

    def test_delete_is_idempotent(self):
        job = self.create()
        self.assertEqual(self.run_cli("delete", "--job", job).stdout.strip(), f"deleted {job}")
        self.assertEqual(self.run_cli("delete", "--job", job).stdout.strip(), f"{job} already gone")
        self.assertEqual(self.due("2026-10-29T03:08:00Z"), [])
        self.assertEqual(json.loads(self.run_cli("list", "--json").stdout), [])

    def test_deleting_a_job_confirms_the_pending_cancellation_that_names_it(self):
        job, other = self.create(), self.create()
        pending = self.root / "build/claude-campaign-pending-cancel.json"
        pending.write_text(json.dumps([{"cron_id": job, "reason": "operator-stop", "at": "2026-10-29T03:01:00Z"},
                                       {"cron_id": other, "reason": "park", "at": "2026-10-29T03:01:00Z"}]),
                           encoding="utf-8")
        out = self.run_cli("delete", "--job", job).stdout
        self.assertIn(f"deleted {job}", out)
        self.assertIn(f"cancel-confirmed: job {job} is gone; still pending: {other}", out)
        # Already gone from the store: the delete still confirms the record that names it.
        self.run_cli("delete", "--job", other)
        self.assertFalse(pending.exists())
        pending.write_text("not json", encoding="utf-8")
        result = self.run_cli("delete", "--job", "ohb-000000000000")
        self.assertIn("pending-cancellation record is unreadable", result.stderr)

    def test_a_job_expires_after_seven_days(self):
        job = self.create(now="2026-10-29T03:00:30Z")
        self.assertEqual([entry["id"] for entry in self.due("2026-11-05T03:00:29Z")], [job])
        self.assertEqual(self.due("2026-11-05T03:03:00Z"), [])
        self.assertEqual(json.loads(self.run_cli("list", "--json").stdout), [])

    def test_unsupported_cron_forms_are_refused(self):
        for cron in ("3/5 * * * *", "* * * *", "61 * * * *", "*/0 * * * *", "5-3 * * * *", "MON * * * *",
                     "1,2 * * * *", "* * * * 7"):
            with self.subTest(cron=cron):
                result = self.run_cli("create", "--session", "ses_owner", "--cron", cron, "--prompt-file",
                                      str(self.prompt), expect=2)
                self.assertIn("cron", result.stderr)
        self.assertFalse((self.root / "build/opencode-heartbeat-jobs.json").exists())

    def test_list_keeps_the_prompt_and_id_after_the_prompt_file_is_gone(self):
        job = self.create()
        self.prompt.unlink()
        listed = json.loads(self.run_cli("list", "--json").stdout)
        self.assertEqual([(entry["id"], entry["session"], entry["cron"]) for entry in listed],
                         [(job, "ses_owner", CRON)])
        self.assertTrue(listed[0]["prompt"].startswith("OpenCode run-guard heartbeat for RasterFold"))
        text = self.run_cli("list").stdout
        self.assertIn(job, text)
        self.assertIn("OpenCode run-guard heartbeat for RasterFold", text)
        self.assertEqual(self.due("2026-10-29T03:03:00Z")[0]["prompt"], listed[0]["prompt"])

    def test_a_malformed_store_fails_safe_for_due_and_refuses_the_rest(self):
        store = self.root / "build/opencode-heartbeat-jobs.json"
        for body in ("not json", "[]", json.dumps({"schema": 2, "jobs": []}),
                     json.dumps({"schema": 1, "jobs": [{"id": "x"}]})):
            with self.subTest(body=body):
                store.write_text(body, encoding="utf-8")
                self.assertEqual(self.due("2026-10-29T03:03:00Z"), [])
                self.assertIn("malformed" if body != "not json" else "unreadable",
                              self.run_cli("list", expect=2).stderr)
                self.run_cli("delete", "--job", "ohb-000000000000", expect=2)
                self.run_cli("create", "--session", "ses_owner", "--cron", CRON, "--prompt-file",
                             str(self.prompt), expect=2)
                self.assertEqual(store.read_text(encoding="utf-8"), body)


if __name__ == "__main__":
    unittest.main()
