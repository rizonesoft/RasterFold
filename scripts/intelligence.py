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
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))
import campaign_guard  # noqa: E402

STATES = {"open", "deferred", "applied", "rejected"}
FAMILIES = ("claude", "codex")
SENTINELS = (
    "claude-campaign-guard.json",
    "claude-campaign-state.json",
    "claude-campaign-pending-cancel.json",
)


def _validate_history(row: dict, bucket_name: str) -> None:
    history = row.get("history")
    if not isinstance(history, list):
        raise ValueError("missing disposition history")
    state = "open"
    archived = False
    for idx, entry in enumerate(history):
        if not isinstance(entry, dict):
            raise ValueError("malformed disposition history entry")
        if archived:
            raise ValueError("no history transitions allowed after archive")
        action = entry.get("action")
        if action == "recurrence":
            if (
                idx != 0
                or set(entry) != {"action", "previous_id", "at"}
                or type(entry["previous_id"]) is not int
                or not 0 < entry["previous_id"] < row["id"]
                or not isinstance(entry["at"], str)
                or not entry["at"].strip()
            ):
                raise ValueError("malformed recurrence history entry")
            continue
        if action in ("apply", "reject", "defer", "archive"):
            if (
                set(entry) != {"action", "at", "reason", "commit"}
                or not isinstance(entry["at"], str)
                or not entry["at"].strip()
                or not isinstance(entry["reason"], str)
                or not entry["reason"].strip()
                or not isinstance(entry["commit"], str)
            ):
                raise ValueError("malformed disposition history entry")
            if action == "apply":
                if not entry["commit"].strip():
                    raise ValueError("apply history entry requires a commit")
            elif entry["commit"] != "":
                raise ValueError(f"{action} history entry must not carry a commit")
            if action in ("apply", "reject", "defer"):
                if state not in ("open", "deferred"):
                    raise ValueError("closed entries are immutable except for archiving")
                state = {"apply": "applied", "reject": "rejected", "defer": "deferred"}[action]
            else:
                if bucket_name != "archive" or state not in ("applied", "rejected"):
                    raise ValueError("only resolved entries can be archived")
                archived = True
            continue
        raise ValueError("unknown history action")
    if bucket_name == "archive":
        if row["status"] not in ("applied", "rejected"):
            raise ValueError("only resolved entries can be archived")
        if not archived or state != row["status"]:
            raise ValueError("archived entries require a closing archive history record matching status")
    else:
        if state != row["status"]:
            raise ValueError("intelligence status does not match disposition history")


def _validate_register_data(data: dict, family: str) -> dict:
    if (
        not isinstance(data, dict)
        or set(data) != {"schema", "family", "next_id", "entries", "archive"}
        or data.get("schema") != 1
        or data.get("family") != family
        or type(data.get("next_id")) is not int
        or data["next_id"] < 1
        or not isinstance(data.get("entries"), list)
        or not isinstance(data.get("archive"), list)
    ):
        raise ValueError(f"invalid intelligence register header for {family}")
    ids = set()
    active_keys = set()
    for bucket_name, bucket in (("entries", data["entries"]), ("archive", data["archive"])):
        for row in bucket:
            if (
                not isinstance(row, dict)
                or set(row) != {"id", "key", "title", "evidence", "proposal", "owner", "status", "created", "history"}
                or row["status"] not in STATES
            ):
                raise ValueError(f"malformed intelligence entry in {family}")
            if type(row["id"]) is not int or not 0 < row["id"] < data["next_id"] or row["id"] in ids:
                raise ValueError(f"invalid or duplicate intelligence id in {family}")
            ids.add(row["id"])
            if any(
                not isinstance(row[field], str) or not row[field].strip()
                for field in ("key", "title", "evidence", "proposal", "owner", "created")
            ):
                raise ValueError("intelligence requires title, evidence, proposal and owner")
            expected_key = hashlib.sha256(
                (row["title"].strip() + "\n" + row["evidence"].strip()).encode("utf-8")
            ).hexdigest()
            if row["key"] != expected_key:
                raise ValueError(f"intelligence key mismatch in {family}")
            _validate_history(row, bucket_name)
            if bucket_name == "entries":
                if row["key"] in active_keys:
                    raise ValueError(f"duplicate active intelligence key in {family}")
                active_keys.add(row["key"])
    return data


def read_register(root: Path, family: str) -> dict:
    if family not in FAMILIES:
        raise ValueError(f"unknown intelligence family: {family}")
    path = Path(root) / ".intelligence" / family / "register.json"
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing intelligence register for {family}")
    data = json.loads(path.read_text(encoding="utf-8"))
    return _validate_register_data(data, family)


def save_register(root: Path, family: str, data: dict) -> None:
    _validate_register_data(data, family)
    path = Path(root) / ".intelligence" / family / "register.json"
    temp = path.with_name(path.name + f".{os.getpid()}.tmp")
    try:
        with temp.open("w", encoding="utf-8") as out:
            json.dump(data, out, indent=2)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        temp.replace(path)
    except BaseException:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def require_idle(root: Path) -> None:
    build_dir = Path(root) / "build"
    try:
        st = os.lstat(build_dir)
    except FileNotFoundError:
        return
    except OSError as exc:
        raise ValueError(f"cannot verify stopped boundary on build directory: {exc}") from exc
    if not os.path.isdir(build_dir) or os.path.islink(build_dir):
        raise ValueError("campaign build path is not a regular directory; cannot verify stopped boundary")
    try:
        os.listdir(build_dir)
    except OSError as exc:
        raise ValueError(f"campaign build directory is unreadable: {exc}") from exc
    for name in SENTINELS:
        sentinel = build_dir / name
        try:
            os.lstat(sentinel)
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise ValueError(f"cannot verify stopped boundary for {name}: {exc}") from exc
        raise ValueError(f"campaign guard is active ({name} exists); stop the run before disposing of intelligence")
    _ = st


def update(
    root: Path,
    family: str,
    action: str,
    *,
    title: str = "",
    evidence: str = "",
    proposal: str = "",
    owner: str = "",
    ident: int | None = None,
    reason: str = "",
    commit: str = "",
    lock_wait: float = 10.0,
    _after_idle_hook=None,
) -> dict:
    with campaign_guard._Lock(str(root), wait=lock_wait):
        if action != "capture":
            require_idle(root)
            if _after_idle_hook is not None:
                _after_idle_hook()
        data = read_register(root, family)
        now = datetime.now(timezone.utc).isoformat()
        if action == "capture":
            if ident is not None or reason.strip() or commit.strip():
                raise ValueError("capture does not accept --id, --reason or --commit")
            if not all(x.strip() for x in (title, evidence, proposal, owner)):
                raise ValueError("capture requires title, evidence, proposal and owner")
            key = hashlib.sha256((title.strip() + "\n" + evidence.strip()).encode("utf-8")).hexdigest()
            previous = next((e for e in data["entries"] if e["key"] == key), None)
            if previous:
                return previous
            row = dict(
                id=data["next_id"],
                key=key,
                title=title,
                evidence=evidence,
                proposal=proposal,
                owner=owner,
                status="open",
                created=now,
                history=[],
            )
            archived = next((e for e in reversed(data["archive"]) if e["key"] == key), None)
            if archived:
                row["history"].append(dict(action="recurrence", previous_id=archived["id"], at=now))
            data["next_id"] += 1
            data["entries"].append(row)
        else:
            if any(x.strip() for x in (title, evidence, proposal, owner)):
                raise ValueError(f"{action} does not accept --title, --evidence, --proposal or --owner")
            row = next((e for e in data["entries"] if e["id"] == ident), None)
            if row is None:
                raise ValueError("entry is not in the active register")
            if not reason.strip():
                raise ValueError("record a reason and its supporting evidence")
            if action == "archive":
                if commit.strip():
                    raise ValueError("archive does not accept --commit")
                if row["status"] not in ("applied", "rejected"):
                    raise ValueError("open or deferred intelligence cannot be archived")
                commit = ""
                data["entries"].remove(row)
                data["archive"].append(row)
            elif action in ("apply", "reject", "defer"):
                if row["status"] not in ("open", "deferred"):
                    raise ValueError("closed entries are immutable except for archiving")
                if action == "apply":
                    if not commit.strip():
                        raise ValueError("applied intelligence requires a reachable implementation commit")
                    try:
                        commit = subprocess.check_output(
                            ["git", "--no-replace-objects", "rev-parse", "--verify", commit.strip() + "^{commit}"],
                            cwd=root,
                            text=True,
                            stderr=subprocess.DEVNULL,
                        ).strip()
                        subprocess.run(
                            ["git", "--no-replace-objects", "merge-base", "--is-ancestor", commit, "HEAD"],
                            cwd=root,
                            check=True,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    except subprocess.SubprocessError as exc:
                        raise ValueError("applied intelligence requires a reachable implementation commit") from exc
                else:
                    if commit.strip():
                        raise ValueError(f"{action} does not accept --commit")
                    commit = ""
                row["status"] = {"apply": "applied", "reject": "rejected", "defer": "deferred"}[action]
            else:
                raise ValueError("unknown intelligence action")
            row["history"].append(dict(action=action, at=now, reason=reason, commit=commit))
        save_register(root, family, data)
        return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--family", choices=FAMILIES, default="claude")
    parser.add_argument("action", choices=("list", "validate", "capture", "apply", "reject", "defer", "archive"))
    parser.add_argument("--id", type=int)
    for key in ("title", "evidence", "proposal", "owner", "reason", "commit"):
        parser.add_argument("--" + key, default="")
    args = parser.parse_args(argv)
    try:
        if args.action == "validate":
            for family in FAMILIES:
                data = read_register(args.root, family)
                print(f"{family}: {len(data['entries'])} active records, {len(data['archive'])} archived; valid")
            return 0
        if args.action == "list":
            data = read_register(args.root, args.family)
            print(json.dumps(data, indent=2))
            return 0
        row = update(
            args.root,
            args.family,
            args.action,
            ident=args.id,
            **{k: getattr(args, k) for k in ("title", "evidence", "proposal", "owner", "reason", "commit")},
        )
        print(json.dumps(row, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        print(f"intelligence: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
