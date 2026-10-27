#!/usr/bin/env python3
"""Adapt the shared campaign Stop guard to Codex's JSON hook contract."""
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
            campaign_guard._record_hook_error(root, campaign_guard.read_guard(root), "Codex Stop payload too large")
            print("{}")
            return 0
        try:
            event = json.loads(payload)
            if isinstance(event, dict):
                # Codex's session_id is authoritative; Antigravity's field must not change its verdict format.
                event.pop("conversationId", None)
                payload = json.dumps(event)
        except (ValueError, RecursionError):
            pass  # The shared guard handles and records malformed input.
        status, response = campaign_guard.stop_hook(payload, root)
        print(response or "{}")
        return status
    except BaseException as error:
        print(f"codex-campaign-hook: {error}", file=sys.stderr)
        print("{}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
