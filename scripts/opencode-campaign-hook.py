#!/usr/bin/env python3
"""Adapt the shared campaign Stop guard to the OpenCode plugin's turn-end payload.

The plugin (`.opencode/plugins/rasterfold-campaign/index.js`) sends one JSON object on stdin when a session's turn
ends: `{"session_id": ..., "cwd": ..., "interrupted": true|false}`. An interrupted turn is the operator's Esc, so it
is passed on as `terminationReason: USER_CANCELED` and never continued. The guard's decision (or `{}`) is printed.
"""
from pathlib import Path
import json
import sys

MAX_PAYLOAD_CHARS = 2_000_000


def main():
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import campaign_guard
        root = str(Path(__file__).resolve().parents[1])
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
        # Only these fields reach the guard: another harness's fields must not change its verdict format.
        forwarded = {"session_id": event["session_id"]}
        if event.get("interrupted") is True:
            forwarded["terminationReason"] = "USER_CANCELED"
        status, response = campaign_guard.stop_hook(json.dumps(forwarded), root)
        print(response or "{}")
        return status
    except BaseException as error:
        print(f"opencode-campaign-hook: {error}", file=sys.stderr)
        print("{}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
