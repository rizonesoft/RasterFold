#!/usr/bin/env python3
"""Evidence-led runner improvements. Capture while running; dispose when stopped."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
STATES = {"open", "deferred", "applied", "rejected"}
FAMILIES = ("claude", "codex")


def read_register(root: Path, family: str) -> dict:
    path = Path(root) / ".intelligence" / family / "register.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or data.get("family") != family or type(data.get("next_id")) is not int:
        raise ValueError(f"invalid intelligence register header for {family}")
    ids = set()
    for row in data["entries"] + data["archive"]:
        if set(row) != {"id", "key", "title", "evidence", "proposal", "owner", "status", "created", "history"} or row["status"] not in STATES:
            raise ValueError(f"malformed intelligence entry in {family}")
        if type(row["id"]) is not int or not 0 < row["id"] < data["next_id"] or row["id"] in ids:
            raise ValueError(f"invalid or duplicate intelligence id in {family}")
        ids.add(row["id"])
        if any(not isinstance(row[field], str) or not row[field].strip() for field in ("key", "title", "evidence", "proposal", "owner", "created")):
            raise ValueError("intelligence requires title, evidence, proposal and owner")
        if not isinstance(row["history"], list):
            raise ValueError("missing disposition history")
        if row in data["archive"] and row["status"] not in ("applied", "rejected"):
            raise ValueError("only resolved entries can be archived")
    return data


def save_register(root: Path, family: str, data: dict) -> None:
    path = Path(root) / ".intelligence" / family / "register.json"
    temp = path.with_name(path.name + f".{os.getpid()}.tmp")
    with temp.open("w", encoding="utf-8") as out:
        json.dump(data, out, indent=2)
        out.write("\n")
        out.flush()
        os.fsync(out.fileno())
    temp.replace(path)


def require_idle(root: Path) -> None:
    guard = Path(root) / "build" / "claude-campaign-guard.json"
    if guard.exists():
        raise ValueError("campaign guard is active; stop the run before disposing of intelligence")


def update(root: Path, family: str, action: str, *, title="", evidence="", proposal="", owner="", ident=None, reason="", commit="") -> dict:
    data = read_register(root, family)
    now = datetime.now(timezone.utc).isoformat()
    if action == "capture":
        if not all(x.strip() for x in (title, evidence, proposal, owner)):
            raise ValueError("capture requires title, evidence, proposal and owner")
        key = hashlib.sha256((title.strip() + "\n" + evidence.strip()).encode("utf-8")).hexdigest()
        previous = next((e for e in data["entries"] if e["key"] == key), None)
        if previous:
            return previous
        row = dict(id=data["next_id"], key=key, title=title, evidence=evidence, proposal=proposal,
                   owner=owner, status="open", created=now, history=[])
        archived = next((e for e in reversed(data["archive"]) if e["key"] == key), None)
        if archived:
            row["history"].append(dict(action="recurrence", previous_id=archived["id"], at=now))
        data["next_id"] += 1
        data["entries"].append(row)
    else:
        require_idle(root)
        row = next((e for e in data["entries"] if e["id"] == ident), None)
        if row is None:
            raise ValueError("entry is not in the active register")
        if not reason.strip():
            raise ValueError("record a reason and its supporting evidence")
        if action == "archive":
            if row["status"] not in ("applied", "rejected"):
                raise ValueError("open or deferred intelligence cannot be archived")
            data["entries"].remove(row)
            data["archive"].append(row)
        elif action in ("apply", "reject", "defer"):
            if row["status"] not in ("open", "deferred"):
                raise ValueError("closed entries are immutable except for archiving")
            if action == "apply":
                if not commit:
                    raise ValueError("applied intelligence requires a reachable implementation commit")
                commit = subprocess.check_output(["git", "--no-replace-objects", "rev-parse", "--verify", commit + "^{commit}"], cwd=root, text=True).strip()
                subprocess.run(["git", "--no-replace-objects", "merge-base", "--is-ancestor", commit, "HEAD"], cwd=root, check=True)
            row["status"] = {"apply": "applied", "reject": "rejected", "defer": "deferred"}[action]
        else:
            raise ValueError("unknown intelligence action")
        row["history"].append(dict(action=action, at=now, reason=reason, commit=commit))
    save_register(root, family, data)
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--family", choices=FAMILIES, default="claude")
    parser.add_argument("action", choices=("list", "validate", "capture", "apply", "reject", "defer", "archive"))
    parser.add_argument("--id", type=int)
    for key in ("title", "evidence", "proposal", "owner", "reason", "commit"):
        parser.add_argument("--" + key, default="")
    args = parser.parse_args()
    try:
        if args.action == "validate":
            for family in FAMILIES:
                reg_path = Path(args.root) / ".intelligence" / family / "register.json"
                if reg_path.exists():
                    data = read_register(args.root, family)
                    print(f"{family}: {len(data['entries'])} active records, {len(data['archive'])} archived; valid")
            return 0
        if args.action == "list":
            data = read_register(args.root, args.family)
            print(json.dumps(data, indent=2))
            return 0
        row = update(args.root, args.family, args.action, ident=args.id,
                     **{k: getattr(args, k) for k in ("title", "evidence", "proposal", "owner", "reason", "commit")})
        print(json.dumps(row, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        print(f"intelligence: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
