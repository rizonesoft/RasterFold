#!/usr/bin/env python3
"""Converter gates. Today: --case contract (D10 T01 section 1).

Validates converter/codecs.json against the vision format matrix and the
owning sections in todo/10-converter/TODO-02-codecs.md, compiles the
public headers as C11 and C++, and builds and runs
tests/converter/test_contract.c against converter/src/registry.c.
Later sections add their cases here; an unknown case is a failure.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REQUIRED = ["rasterfold", "bmp", "png", "apng", "pnm", "pam", "webp",
            "jpeg-xl", "avif", "jpeg", "gif", "tiff", "heif", "heic",
            "jpeg-2000", "qoi", "openexr", "camera-raw", "dng"]
KNOWN_SAMPLES = {"gray8", "gray16", "rgb8", "rgba8", "rgb16", "rgba16",
                 "rgb-half", "rgba-half", "rgb-float", "rgba-float"}
SETTING_TYPES = {"enum", "int", "bool"}
EXPECTED_OWNER = {"rasterfold": 1, "bmp": 1, "png": 1, "apng": 1, "pnm": 1,
                  "pam": 1, "webp": 2, "jpeg-xl": 3, "avif": 4, "jpeg": 5,
                  "gif": 6, "tiff": 6, "heif": 7, "heic": 7, "jpeg-2000": 9,
                  "qoi": 8, "openexr": 10, "camera-raw": 11, "dng": 11}
IMPORT_ONLY = {"camera-raw", "dng"}  # docs/vision.md: explicit import-only adapter
ANIMATED = {"apng", "jpeg-xl", "gif", "tiff"}


def fail(msg):
    print(f"contract: FAIL: {msg}")
    return 1


def validate_registry():
    path = os.path.join(ROOT, "converter", "codecs.json")
    try:
        doc = json.load(open(path, encoding="utf-8"))
    except Exception as exc:
        return fail(f"converter/codecs.json is not valid JSON: {exc}")
    if doc.get("schema") != 1 or doc.get("abi_version") != 1:
        return fail("converter/codecs.json: schema/abi_version must be 1")
    codecs_md = open(os.path.join(
        ROOT, "todo", "10-converter", "TODO-02-codecs.md"), encoding="utf-8").read()
    sections = {int(n) for n in re.findall(r"^## (\d+)\.", codecs_md, re.M)}
    shipped = set()
    for line in codecs_md.splitlines():
        m = re.match(r"\|\s*\d+\s*\|\s*§(\d+)\s*\|.*\|\s*\[x\]\s*\|", line)
        if m:
            shipped.add(int(m.group(1)))
    seen = set()
    for row in doc.get("formats", []):
        name = row.get("format")
        where = f"converter/codecs.json row {name!r}"
        if name in seen:
            return fail(f"{where}: duplicate format")
        seen.add(name)
        if not row.get("provider"):
            return fail(f"{where}: missing provider")
        owner = row.get("owner", "")
        m = re.fullmatch(r"D10 T02 §(\d+)", owner)
        if not m or int(m.group(1)) not in sections:
            return fail(f"{where}: missing or unresolvable owner {owner!r}")
        modes = row.get("modes", {})
        if not modes.get("read") and not modes.get("write"):
            return fail(f"{where}: advertises neither read nor write")
        if name in IMPORT_ONLY and modes.get("write"):
            return fail(f"{where}: import-only format advertises write")
        if name in EXPECTED_OWNER and m and int(m.group(1)) != EXPECTED_OWNER[name]:
            return fail(f"{where}: owner §{m.group(1)} does not own this format "
                        f"(expected §{EXPECTED_OWNER[name]} in TODO-02-codecs.md)")
        if name in ANIMATED and row.get("extension_owner") != "D10 T02 §12":
            return fail(f"{where}: animation/multi-page extension owner must be D10 T02 §12")
        if name not in ANIMATED and row.get("extension_owner"):
            return fail(f"{where}: unexpected extension_owner")
        for rule in row.get("rules", []):
            if not rule.get("setting_a") or not rule.get("value_a") or not rule.get("reason"):
                return fail(f"{where}: combination rule is incomplete: {rule!r}")
        samples = row.get("native_samples", [])
        if not samples or any(s not in KNOWN_SAMPLES for s in samples):
            return fail(f"{where}: native_samples missing or unknown: {samples!r}")
        if row.get("availability") not in ("planned", "usable"):
            return fail(f"{where}: availability must be planned or usable")
        if row.get("availability") == "planned" and not row.get("unavailable_reason"):
            return fail(f"{where}: planned format needs an unavailable_reason")
        if row.get("availability") == "usable" and m and int(m.group(1)) not in shipped:
            return fail(f"{where}: marked usable but owning section §{m.group(1)} has not shipped")
        for setting in row.get("settings", []):
            if setting.get("type") not in SETTING_TYPES:
                return fail(f"{where}: setting {setting.get('id')!r} has unknown type")
            if setting.get("type") == "enum" and not setting.get("allowed"):
                return fail(f"{where}: enum setting {setting.get('id')!r} lists no allowed values")
            if setting.get("default") is None:
                return fail(f"{where}: setting {setting.get('id')!r} has no default")
    missing = [f for f in REQUIRED if f not in seen]
    if missing:
        return fail(f"converter/codecs.json: missing required formats: {missing}")
    print(f"contract: registry ok ({len(seen)} formats, owners resolve)")
    return 0


def run(cmd):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, cwd=ROOT).returncode


def case_contract():
    if validate_registry():
        return 1
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    inc = os.path.join(ROOT, "converter", "include")
    with tempfile.TemporaryDirectory() as tmp:
        tu = os.path.join(tmp, "tu.c")
        open(tu, "w").write('#include "rfc_codec.h"\nint main(void){return 0;}\n')
        if run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc, "-c", tu, "-o", os.path.join(tmp, "tu.o")]):
            return fail("headers do not compile as C11")
        tux = os.path.join(tmp, "tu.cpp")
        open(tux, "w").write('#include "rfc_codec.h"\nint main(){return 0;}\n')
        if run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", inc, "-c", tux, "-o", os.path.join(tmp, "tu_cpp.o")]):
            return fail("headers do not compile as C++")
        exe = os.path.join(tmp, "test_contract")
        if run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc,
                os.path.join(ROOT, "converter", "src", "registry.c"),
                os.path.join(ROOT, "tests", "converter", "test_contract.c"),
                "-o", exe]):
            return fail("contract test does not build")
        if run([exe]):
            return fail("contract test failed")
        dump = subprocess.run([exe, "dump"], cwd=ROOT, capture_output=True, text=True)
        if dump.returncode:
            return fail("registry dump failed")
        doc = json.load(open(os.path.join(ROOT, "converter", "codecs.json"), encoding="utf-8"))
        expected = []
        for row in doc["formats"]:
            settings = ""
            for s in row["settings"]:
                allowed = ",".join(s.get("allowed", [])) + ("," if s.get("allowed") else "")
                settings += f"{s['id']}:{s['type']}:{s['default']}:{allowed};"
            rules = ""
            for r in row.get("rules", []):
                rules += f"{r['setting_a']}={r['value_a']}&{r.get('setting_b') or '-'}={r.get('value_b') or '-'};"
            expected.append(
                f"{row['format']}|{row['provider']}|{row['owner']}|"
                f"{row.get('extension_owner') or '-'}|{row['availability']}|"
                f"{1 if row['modes']['read'] else 0}|{1 if row['modes']['write'] else 0}|"
                f"{','.join(row['native_samples'])},|{settings}|{rules}")
        actual = dump.stdout.strip().splitlines()
        if actual != expected:
            for a, e in zip(actual, expected):
                if a != e:
                    return fail(f"registry JSON/C mismatch:\n  json: {e}\n  c:    {a}")
            return fail(f"registry JSON/C row count differs: {len(actual)} vs {len(expected)}")
        print("contract: registry JSON and C registry agree on every field")
    print("contract: PASS")
    return 0


def main():
    args = sys.argv[1:]
    if args == ["--case", "contract"]:
        return case_contract()
    print("usage: check-converter.py --case contract  (other cases land with their sections)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
