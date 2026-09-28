"""Lock ownership for the run guard. Uses a temp lock, never guard/lock."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tests" / "guard_lock.py"
passed = 0
failed = 0


class Fail(Exception):
    pass


def check(cond: bool, message: str) -> None:
    if not cond:
        raise Fail(message)


def run(lock: Path, args: list[str], stale: int = 30) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["GUARD_LOCK_FILE"] = str(lock)
    env["GUARD_LOCK_STALE_SEC"] = str(stale)
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def test_live_owner_blocks_a_second_cycle(lock: Path) -> None:
    first = run(lock, ["acquire"])
    check(first.returncode == 0, first.stderr)
    token = first.stdout.strip()
    check(len(token) == 32, token)
    second = run(lock, ["acquire"])
    check(second.returncode == 1, "second acquire took a live lock")
    held = run(lock, ["check"])
    check(held.returncode == 1 and held.stdout.startswith("held "), held.stdout)
    refreshed = run(lock, ["refresh", token])
    check(refreshed.returncode == 0, refreshed.stderr)
    stranger = run(lock, ["refresh", "nope"])
    check(stranger.returncode == 1, "stranger refreshed the lock")
    stolen = run(lock, ["release", "nope"])
    check(stolen.returncode == 1 and lock.exists(), "stranger released the lock")
    done = run(lock, ["release", token])
    check(done.returncode == 0 and not lock.exists(), "owner release left the lock")
    free = run(lock, ["check"])
    check(free.returncode == 0 and free.stdout.strip() == "free", free.stdout)


def test_stale_lock_can_be_taken(lock: Path) -> None:
    first = run(lock, ["acquire"], stale=30)
    token = first.stdout.strip()
    text = lock.read_text(encoding="ascii")
    old = int(time.time()) - 31
    lock.write_text(text.replace(text.splitlines()[1], f"refreshed {old}"), encoding="ascii")
    taken = run(lock, ["acquire"], stale=30)
    check(taken.returncode == 0, taken.stderr)
    new_token = taken.stdout.strip()
    check(new_token != token, "takeover reused the dead token")
    late = run(lock, ["refresh", token], stale=30)
    check(late.returncode == 1, "dead owner refreshed after takeover")
    check(run(lock, ["release", new_token], stale=30).returncode == 0, "new owner could not release")


def test_legacy_lock_uses_mtime(lock: Path) -> None:
    lock.write_text("cycle\n", encoding="ascii")
    os.utime(lock, None)
    check(run(lock, ["check"]).returncode == 1, "fresh legacy lock looked free")
    check(run(lock, ["acquire"]).returncode == 1, "acquire stole a fresh legacy lock")
    old = time.time() - 120
    os.utime(lock, (old, old))
    taken = run(lock, ["acquire"], stale=30)
    check(taken.returncode == 0, taken.stderr)
    check(lock.read_text(encoding="ascii").startswith("token "), lock.read_text(encoding="ascii"))


def main() -> int:
    global passed, failed
    work = ROOT / "tests" / "work" / "guard-lock"
    work.mkdir(parents=True, exist_ok=True)
    tests = [
        test_live_owner_blocks_a_second_cycle,
        test_stale_lock_can_be_taken,
        test_legacy_lock_uses_mtime,
    ]
    for test in tests:
        lock = work / f"{test.__name__}.lock"
        if lock.exists():
            lock.unlink()
        try:
            test(lock)
        except Fail as exc:
            failed += 1
            print(f"FAIL {test.__name__}: {exc}")
        else:
            passed += 1
            print(f"ok   {test.__name__}")
        finally:
            if lock.exists():
                lock.unlink()
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
