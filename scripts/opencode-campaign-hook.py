#!/usr/bin/env python3
"""Adapt the shared campaign Stop guard to the OpenCode plugin's turn-end payload.

The plugin (`.opencode/plugins/rasterfold-campaign/index.js`) sends one JSON object on stdin when a session's turn
ends: `{"session_id": ..., "cwd": ..., "interrupted": true|false}`. An interrupted turn is the operator's Esc, so it
is passed on as `terminationReason: USER_CANCELED` and never continued. The guard's decision (or `{}`) is printed.
Each valid turn end also records its session and UTC time in `build/opencode-hook-seen.json` under the guard lock;
`--seen --session <id> --since <YYYY-MM-DDTHH:MM:SSZ>` verifies that a turn end at or after `<since>` reached this
adapter before a campaign is acquired.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys

MAX_PAYLOAD_CHARS = 2_000_000
SEEN_REL = Path("build/opencode-hook-seen.json")


def _parse_utc(text):
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def _load_seen(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, ValueError, RecursionError):
        return {}
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("sessions"), dict):
        return {}
    return {str(k): str(v) for k, v in data["sessions"].items() if isinstance(k, str) and isinstance(v, str)}


def _record_seen(campaign_guard, root, session_id):
    path = Path(root) / SEEN_REL
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with campaign_guard._Lock(root):
        sessions = _load_seen(path)
        sessions[session_id] = stamp
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump({"schema": 1, "sessions": sessions}, handle, indent=1)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)


def _check_seen(campaign_guard, root, session_id, since):
    try:
        since_dt = _parse_utc(since)
    except (TypeError, ValueError):
        print(f"invalid --since UTC timestamp: {since!r}", file=sys.stderr)
        return 2
    path = Path(root) / SEEN_REL
    with campaign_guard._Lock(root):
        sessions = _load_seen(path)
    stamp = sessions.get(session_id)
    if not stamp:
        print(f"unseen {session_id}", file=sys.stderr)
        return 1
    try:
        stamp_dt = _parse_utc(stamp)
    except (TypeError, ValueError):
        print(f"malformed seen timestamp for {session_id}: {stamp!r}", file=sys.stderr)
        return 1
    if stamp_dt < since_dt:
        print(f"stale {session_id} at {stamp} (before {since})", file=sys.stderr)
        return 1
    print(f"seen {session_id} at {stamp}")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv:
        parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
        parser.add_argument("--seen", action="store_true", required=True)
        parser.add_argument("--session", required=True)
        parser.add_argument("--since", required=True)
        args = parser.parse_args(argv)
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import campaign_guard
        root = str(Path(__file__).resolve().parents[1])
        return _check_seen(campaign_guard, root, args.session, args.since)
    campaign_guard = None
    root = str(Path(__file__).resolve().parents[1])
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import campaign_guard as cg
        campaign_guard = cg
        payload = sys.stdin.read(MAX_PAYLOAD_CHARS + 1)
        if len(payload) > MAX_PAYLOAD_CHARS:
            campaign_guard._record_hook_error(root, campaign_guard.read_guard(root), "OpenCode turn-end payload too large")
            print("{}")
            return 0
        try:
            event = json.loads(payload)
        except (ValueError, RecursionError):
            event = None
        if not isinstance(event, dict) or not isinstance(event.get("session_id"), str) or not event["session_id"]:
            campaign_guard._record_hook_error(root, campaign_guard.read_guard(root), "malformed OpenCode turn-end payload")
            print("{}")
            return 0
        try:
            _record_seen(campaign_guard, root, event["session_id"])
        except BaseException as seen_error:
            campaign_guard._record_hook_error(
                root, campaign_guard.read_guard(root), f"OpenCode hook seen-record failed: {seen_error}"
            )
        # Only these fields reach the guard: another harness's fields must not change its verdict format.
        forwarded = {"session_id": event["session_id"]}
        if event.get("interrupted") is True:
            forwarded["terminationReason"] = "USER_CANCELED"
        status, response = campaign_guard.stop_hook(json.dumps(forwarded), root)
        print(response or "{}")
        return status
    except BaseException as error:
        if campaign_guard is not None:
            try:
                campaign_guard._record_hook_error(
                    root, campaign_guard.read_guard(root), f"OpenCode turn-end hook failed: {error}"
                )
            except BaseException:
                pass
        print(f"opencode-campaign-hook: {error}", file=sys.stderr)
        print("{}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
