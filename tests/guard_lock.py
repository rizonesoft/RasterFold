"""Single-owner lock for a RasterFold run-guard cycle.

The worker that owns the cycle rewrites guard/lock while it works. A later
fire may take the lock only when the file is missing, or when the last
refresh is older than STALE_SEC because that worker died. Refreshing keeps
the same owner for as long as the cycle runs.

  python tests/guard_lock.py acquire
      Exit 0 and print the owner token when this fire now owns the lock.
      Exit 1 when a live owner still holds it.
  python tests/guard_lock.py refresh TOKEN
      Rewrite the refresh time. Exit 1 when TOKEN is not the current owner.
  python tests/guard_lock.py release TOKEN
      Delete the lock only when TOKEN is the current owner.
  python tests/guard_lock.py check
      Exit 0 when a new cycle may start (missing or stale). Exit 1 when held.

GUARD_LOCK_FILE and GUARD_LOCK_STALE_SEC override the path and the stale
window for tests. The default window is 30 minutes. A cycle refreshes at
least every 10 minutes, which stays inside that window.
"""

from __future__ import annotations

import os
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STALE_SEC = 30 * 60


def lock_path() -> Path:
    override = os.environ.get("GUARD_LOCK_FILE")
    if override:
        return Path(override)
    return ROOT / "guard" / "lock"


def stale_sec() -> int:
    override = os.environ.get("GUARD_LOCK_STALE_SEC")
    if override:
        return int(override)
    return STALE_SEC


def load(path: Path) -> tuple[str | None, int] | None:
    if not path.exists():
        return None
    token = None
    refreshed = None
    text = path.read_text(encoding="ascii", errors="replace")
    for line in text.splitlines():
        if line.startswith("token "):
            token = line[6:].strip()
        elif line.startswith("refreshed "):
            try:
                refreshed = int(line.split()[1])
            except (IndexError, ValueError):
                refreshed = None
    if refreshed is None:
        return None, int(path.stat().st_mtime)
    return token, refreshed


def fresh(record: tuple[str | None, int] | None, now: int) -> bool:
    return record is not None and (now - record[1]) < stale_sec()


def write_atomic(path: Path, token: str, refreshed: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_text(f"token {token}\nrefreshed {refreshed}\n", encoding="ascii", newline="\n")
    os.replace(tmp, path)


def acquire() -> int:
    path = lock_path()
    now = int(time.time())
    if fresh(load(path), now):
        print("held", file=sys.stderr)
        return 1
    token = uuid.uuid4().hex
    write_atomic(path, token, now)
    record = load(path)
    if record is None or record[0] != token:
        print("held", file=sys.stderr)
        return 1
    print(token)
    return 0


def refresh(token: str) -> int:
    path = lock_path()
    record = load(path)
    if record is None or record[0] != token:
        print("not-owner", file=sys.stderr)
        return 1
    now = int(time.time())
    write_atomic(path, token, now)
    record = load(path)
    if record is None or record[0] != token:
        print("not-owner", file=sys.stderr)
        return 1
    print(token)
    return 0


def release(token: str) -> int:
    path = lock_path()
    record = load(path)
    if record is None:
        print("free")
        return 0
    if record[0] != token:
        print("not-owner", file=sys.stderr)
        return 1
    path.unlink()
    print("free")
    return 0


def check() -> int:
    record = load(lock_path())
    now = int(time.time())
    if fresh(record, now):
        age = now - record[1]
        print(f"held {age}")
        return 1
    print("free")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 1 and argv[0] == "acquire":
        return acquire()
    if len(argv) == 2 and argv[0] == "refresh":
        return refresh(argv[1])
    if len(argv) == 2 and argv[0] == "release":
        return release(argv[1])
    if len(argv) == 1 and argv[0] == "check":
        return check()
    print(
        "usage: guard_lock.py acquire | refresh TOKEN | release TOKEN | check",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
