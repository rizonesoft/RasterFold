#!/usr/bin/env python3
"""Check that every rf_fail message in src/*.c is catalogued in docs/messages.json.

Usage:
    python3 scripts/message_catalog.py           # check, exit 1 on any gap
    python3 scripts/message_catalog.py --write   # regenerate the catalogue
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "docs" / "messages.json"
PATTERN = re.compile(r'rf_fail\(err,\s*(RF_ERR_[A-Z]+),\s*"([^"]+)"\)')
CONDITIONAL = re.compile(r'rf_fail\(err,\s*(RF_ERR_[A-Z]+),\s*[^,]*\?\s*"([^"]+)"\s*:\s*"([^"]+)"\)')
CODES = {"RF_ERR_USAGE": 1, "RF_ERR_IO": 2, "RF_ERR_UNSUPPORTED": 3, "RF_ERR_FORMAT": 4, "RF_ERR_LIMIT": 5}


def emitted() -> dict[str, int]:
    found: dict[str, int] = {}
    for src in sorted((ROOT / "src").glob("*.c")):
        text = src.read_text(encoding="utf-8")
        pairs = list(PATTERN.findall(text))
        for macro, a, b in CONDITIONAL.findall(text):
            pairs += [(macro, a), (macro, b)]
        for macro, message in pairs:
            code = CODES[macro]
            if message in found and found[message] != code:
                raise SystemExit(f"{message!r} emitted with two codes: {found[message]} and {code}")
            found[message] = code
    return found


def main() -> int:
    want = emitted()
    if "--write" in sys.argv:
        data = {"messages": [{"text": text, "code": want[text]} for text in sorted(want)]}
        CATALOG.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(f"message_catalog: wrote {len(want)} messages")
        return 0
    have = {m["text"]: m["code"] for m in json.loads(CATALOG.read_text(encoding="utf-8"))["messages"]}
    missing = sorted(set(want) - set(have))
    stale = sorted(set(have) - set(want))
    wrong = sorted(m for m in set(want) & set(have) if want[m] != have[m])
    for name in missing:
        print(f"message_catalog: uncatalogued message: {name!r}")
    for name in stale:
        print(f"message_catalog: catalogued but not emitted: {name!r}")
    for name in wrong:
        print(f"message_catalog: wrong code for {name!r}: catalogued {have[name]}, emitted {want[name]}")
    if missing or stale or wrong:
        return 1
    print(f"message_catalog: {len(want)} messages catalogued")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
