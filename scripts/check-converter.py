#!/usr/bin/env python3
"""Converter gates. Today: --case contract (D10 T01 section 1).

Validates converter/codecs.json against the vision format matrix and the
owning sections in todo/10-converter/TODO-02-codecs.md, compiles the
public headers as C11 and C++, and builds and runs
tests/converter/test_contract.c against converter/src/registry.c.
Later sections add their cases here; an unknown case is a failure.
"""
import importlib.util
import json
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load_reference():
    path = os.path.join(ROOT, "tests", "converter", "reference.py")
    spec = importlib.util.spec_from_file_location("rf_conv_ref", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_ref = _load_reference()

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
IMPORT_ONLY = {"camera-raw", "dng",   # docs/vision.md: explicit import-only adapter
               "heif", "heic"}        # docs/codec-distribution.md: system codec, read only
ANIMATED = {"apng", "jpeg-xl", "gif", "tiff"}
META_ITEMS = ("icc", "orientation", "frame-timing", "exif", "xmp")


def fail(msg):
    print(f"contract: FAIL: {msg}")
    return 1


def validate_registry(json_path=None):
    path = json_path if json_path else os.path.join(ROOT, "converter", "codecs.json")
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except Exception as exc:
        return fail(f"converter/codecs.json is not valid JSON: {exc}")
    if doc.get("schema") != 1 or doc.get("abi_version") != 1:
        return fail("converter/codecs.json: schema/abi_version must be 1")
    with open(os.path.join(ROOT, "todo", "10-converter", "TODO-02-codecs.md"),
              encoding="utf-8") as f:
        codecs_md = f.read()
    sections = {int(n) for n in re.findall(r"^## (\d+)\.", codecs_md, re.M)}
    shipped = set()
    for line in codecs_md.splitlines():
        m = re.match(r"\|\s*\d+\s*\|\s*§(\d+)\s*\|.*\|\s*\[x\]\s*\|", line)
        if m:
            shipped.add(int(m.group(1)))
    adapter_by_sec = {
        1: os.path.join("converter", "codecs", "basic.c"),
        2: os.path.join("converter", "codecs", "webp.c"),
        3: os.path.join("converter", "codecs", "jxl.c"),
        4: os.path.join("converter", "codecs", "avif.c"),
        5: os.path.join("converter", "codecs", "jpeg.c"),
        6: os.path.join("converter", "codecs", "tiff_gif.c"),
        7: os.path.join("converter", "codecs", "heif.c"),
        8: os.path.join("converter", "codecs", "qoi.c"),
        9: os.path.join("converter", "codecs", "jp2.c"),
        10: os.path.join("converter", "codecs", "openexr.c"),
        11: os.path.join("converter", "codecs", "raw_dng.c"),
        12: os.path.join("converter", "src", "frames.c"),
    }
    started_ready = set()
    for sec_num, rel_path in adapter_by_sec.items():
        blk = re.search(rf"^## {sec_num}\..*?(?=^## \d+\.|\Z)", codecs_md, re.M | re.S)
        if blk and "**Started:**" in blk.group(0) and os.path.isfile(os.path.join(ROOT, rel_path)):
            started_ready.add(sec_num)
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
        by_id = {s.get("id"): s for s in row.get("settings", [])}
        for setting in row.get("settings", []):
            if not setting.get("refusal_reason"):
                return fail(f"{where}: setting {setting.get('id')!r} has no refusal_reason")
        for rule in row.get("rules", []):
            for key in ("setting_a", "setting_b"):
                sid = rule.get(key)
                if sid is not None and sid not in by_id:
                    return fail(f"{where}: rule references unknown setting {sid!r}")
            if (rule.get("setting_b") is None) != (rule.get("value_b") is None):
                return fail(f"{where}: rule pairs setting_b with value_b or neither: {rule!r}")
            for key, vkey in (("setting_a", "value_a"), ("setting_b", "value_b")):
                sid, val = rule.get(key), rule.get(vkey)
                if sid in by_id and by_id[sid].get("type") == "enum" and val is not None \
                        and val not in by_id[sid].get("allowed", []):
                    return fail(f"{where}: rule value {val!r} is not allowed for setting {sid!r}")
        rules = {(r.get("setting_a"), r.get("value_a"), r.get("setting_b"), r.get("value_b"))
                 for r in row.get("rules", [])}
        if ("metadata", "strip", "exactness", "byte-identical") not in rules:
            return fail(f"{where}: missing the metadata-strip/byte-identical safety rule")
        exactness_setting = by_id.get("exactness", {})
        if "original-byte-reconstruction" in exactness_setting.get("allowed", []):
            if ("metadata", "strip", "exactness", "original-byte-reconstruction") not in rules:
                return fail(f"{where}: missing the metadata-strip/original-byte-reconstruction safety rule")
        has_frames = "frames" in by_id
        if name in ANIMATED and not has_frames:
            return fail(f"{where}: animated format is missing its frames setting")
        if name in ANIMATED and ("frames", "first-only", None, None) not in rules:
            return fail(f"{where}: missing the frames=first-only safety rule")
        if has_frames and ("frames", "first-only", None, None) not in rules:
            return fail(f"{where}: missing the frames=first-only safety rule")
        samples = row.get("native_samples", [])
        if not samples or any(s not in KNOWN_SAMPLES for s in samples):
            return fail(f"{where}: native_samples missing or unknown: {samples!r}")
        if row.get("availability") not in ("planned", "usable"):
            return fail(f"{where}: availability must be planned or usable")
        if row.get("availability") == "planned" and not row.get("unavailable_reason"):
            return fail(f"{where}: planned format needs an unavailable_reason")
        if row.get("availability") == "usable" and m and int(m.group(1)) not in shipped and int(m.group(1)) not in started_ready:
            return fail(f"{where}: marked usable but owning section §{m.group(1)} has not shipped")
        for setting in row.get("settings", []):
            if setting.get("type") not in SETTING_TYPES:
                return fail(f"{where}: setting {setting.get('id')!r} has unknown type")
            if setting.get("type") == "enum" and not setting.get("allowed"):
                return fail(f"{where}: enum setting {setting.get('id')!r} lists no allowed values")
            if setting.get("default") is None:
                return fail(f"{where}: setting {setting.get('id')!r} has no default")
        meta_items = row.get("metadata_items")
        if not isinstance(meta_items, list):
            return fail(f"{where}: missing metadata_items list")
        if any(item not in META_ITEMS for item in meta_items) or len(set(meta_items)) != len(meta_items):
            return fail(f"{where}: invalid metadata_items: {meta_items!r}")
        if [i for i in META_ITEMS if i in meta_items] != meta_items:
            return fail(f"{where}: metadata_items not in canonical order {META_ITEMS!r}: {meta_items!r}")
        if "frame-timing" in meta_items and name not in ANIMATED:
            return fail(f"{where}: frame-timing metadata only valid for animated/multi-frame formats")
    missing = [f for f in REQUIRED if f not in seen]
    if missing:
        return fail(f"converter/codecs.json: missing required formats: {missing}")
    print(f"contract: registry ok ({len(seen)} formats, owners resolve)")
    return 0


def run(cmd, *, timeout=60.0, env=None):
    print("+", " ".join(cmd))
    return _ref.run_process_tree(cmd, cwd=ROOT, env=env, timeout=timeout).returncode


def run_capture(cmd, *, timeout=60.0, env=None):
    return _ref.run_process_tree(cmd, cwd=ROOT, env=env, timeout=timeout, capture_output=True)


def _check_headers(tmp):
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    inc = os.path.join(ROOT, "converter", "include")
    tu = os.path.join(tmp, "tu.c")
    open(tu, "w").write('#include "rfc_codec.h"\n#include "rfc_codecs.h"\n#include "rfc_io.h"\n#include "rfc_job.h"\n#include "rfc_verify.h"\n#include "rfc_report.h"\nint main(void){return 0;}\n')
    if run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc, "-c", tu, "-o", os.path.join(tmp, "tu.o")]):
        return fail("headers do not compile as C11")
    tux = os.path.join(tmp, "tu.cpp")
    open(tux, "w").write('#include "rfc_codec.h"\n#include "rfc_codecs.h"\n#include "rfc_io.h"\n#include "rfc_job.h"\n#include "rfc_verify.h"\n#include "rfc_report.h"\nint main(){return 0;}\n')
    if run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", inc, "-c", tux, "-o", os.path.join(tmp, "tu_cpp.o")]):
        return fail("headers do not compile as C++")
    return 0


def case_contract():
    if validate_registry():
        return 1
    cc = os.environ.get("CC", "cc")
    inc = os.path.join(ROOT, "converter", "include")
    with tempfile.TemporaryDirectory() as tmp:
        if _check_headers(tmp):
            return 1
        exe = os.path.join(tmp, "test_contract")
        if run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc,
                os.path.join(ROOT, "converter", "src", "registry.c"),
                os.path.join(ROOT, "tests", "converter", "test_contract.c"),
                "-o", exe]):
            return fail("contract test does not build")
        if run([exe]):
            return fail("contract test failed")
        dump = run_capture([exe, "dump"])
        if dump.returncode:
            return fail("registry dump failed")
        doc = json.load(open(os.path.join(ROOT, "converter", "codecs.json"), encoding="utf-8"))
        expected = []
        for row in doc["formats"]:
            settings = ""
            for s in row["settings"]:
                allowed = ",".join(s.get("allowed", [])) + ("," if s.get("allowed") else "")
                settings += (f"{s['id']}:{s['type']}:{s['default']}:{allowed}"
                             f":{s.get('min_value', 0)}:{s.get('max_value', 0)}"
                             f":{s.get('refusal_reason')};")
            rules = ""
            for r in row.get("rules", []):
                rules += (f"{r['setting_a']}={r['value_a']}&{r.get('setting_b') or '-'}"
                          f"={r.get('value_b') or '-'}:{r.get('reason')};")
            meta = ",".join(row["metadata_items"]) + ("," if row["metadata_items"] else "")
            expected.append(
                f"{row['format']}|{row['provider']}|{row['owner']}|"
                f"{row.get('extension_owner') or '-'}|{row['availability']}|"
                f"{row.get('unavailable_reason') or '-'}|"
                f"{1 if row['modes']['read'] else 0}|{1 if row['modes']['write'] else 0}|"
                f"{','.join(row['native_samples'])},|{settings}|{rules}|{meta}")
        actual = dump.stdout.strip().splitlines()
        if actual != expected:
            for a, e in zip(actual, expected):
                if a != e:
                    return fail(f"registry JSON/C mismatch:\n  json: {e}\n  c:    {a}")
            return fail(f"registry JSON/C row count differs: {len(actual)} vs {len(expected)}")
        print("contract: registry JSON and C registry agree on the complete public contract")
    print("contract: PASS")
    return 0


def case_bundle():
    cmd = [sys.executable, "-m", "unittest", "discover", "-s",
           os.path.join(ROOT, "tests", "converter"), "-p", "test_bundle.py", "-v"]
    if run(cmd):
        return fail("converter bundle check failed")
    print("bundle: PASS")
    return 0


def _assert_jobs_output(label, stdout):
    required = [
        "jobs: config read-back",
        "submit_allocs=0",
        "jobs: hostile/unknown preflight refused",
        "jobs: concurrency",
        "jobs: oom_cap",
        "jobs: retry",
        "jobs: cancel_all_stages live_partial_bytes=0 published_cancelled=0 shutdown_late_callbacks=0 64x_restart=ok",
        "jobs: round 1 regressions ok",
        "jobs: completion gap regressions ok",
        "jobs: all checks passed",
    ]
    for marker in required:
        if marker not in stdout:
            return fail(f"{label} output missing checkpoint marker: {marker}")
    for line in stdout.strip().splitlines():
        print(f"  [{label}] {line}")
    return 0


def case_jobs(all_targets):
    cc = os.environ.get("CC", "cc")
    inc = os.path.join(ROOT, "converter", "include")
    src_dir = os.path.join(ROOT, "converter", "src")
    sources = [
        os.path.join(src_dir, "registry.c"),
        os.path.join(src_dir, "limits.c"),
        os.path.join(src_dir, "verify.c"),
        os.path.join(src_dir, "metadata.c"),
        os.path.join(src_dir, "input.c"),
        os.path.join(src_dir, "output.c"),
        os.path.join(src_dir, "metrics.c"),
        os.path.join(src_dir, "job.c"),
        os.path.join(src_dir, "scheduler.c"),
        os.path.join(src_dir, "gif_scan.c"),
        os.path.join(src_dir, "heif_scan.c"),
        os.path.join(ROOT, "tests", "converter", "test_jobs.c"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        if _check_headers(tmp):
            return 1
        exe = os.path.join(tmp, "test_jobs" + (".exe" if os.name == "nt" else ""))
        cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc, *sources]
        if os.name != "nt":
            cmd.append("-pthread")
        cmd.extend(["-o", exe])
        if run(cmd):
            return fail("jobs host test does not build")
        res = run_capture([exe], timeout=60.0)
        if res.returncode:
            print(res.stdout)
            print(res.stderr, file=sys.stderr)
            return fail(f"jobs host test exited {res.returncode}")
        if _assert_jobs_output("host", res.stdout):
            return 1
    print("jobs (host): PASS")

    if all_targets or os.name == "nt":
        if run([sys.executable, os.path.join(ROOT, "scripts", "build-converter.py"),
                "--target", "windows-x64"]):
            return fail("windows-x64 converter build failed for rfc_test_jobs")
        from pathlib import Path
        test_conv_dir = os.path.join(ROOT, "tests", "converter")
        if test_conv_dir not in sys.path:
            sys.path.insert(0, test_conv_dir)
        import test_bundle

        win_exe = Path(ROOT) / "bin" / "converter" / "windows-x64" / "rfc_test_jobs.exe"
        if not win_exe.is_file():
            return fail(f"missing built Windows binary: {win_exe}")
        pe_dlls = test_bundle.parse_pe64_imports(win_exe)
        ro_dlls = test_bundle.readobj_imports(win_exe)
        if set(pe_dlls) != set(ro_dlls):
            return fail(f"PE vs llvm-readobj import mismatch on {win_exe}: {pe_dlls} vs {ro_dlls}")
        try:
            test_bundle.validate_pe_imports(pe_dlls)
        except ValueError as exc:
            return fail(f"{win_exe} PE import check failed: {exc}")
        print(f"  [windows-x64] PE imports OK: {', '.join(pe_dlls)}")

        win_res = test_bundle.run_windows_binary_isolated(win_exe, timeout_sec=60.0)
        if win_res.returncode:
            print(win_res.stdout)
            print(win_res.stderr, file=sys.stderr)
            return fail(f"rfc_test_jobs.exe isolated run exited {win_res.returncode}")
        if _assert_jobs_output("windows-x64", win_res.stdout):
            return 1
        print("jobs (windows-x64 isolated): PASS")

    print("jobs: PASS")
    return 0


def _assert_exactness_output(label, stdout):
    required = [
        "exactness: sha256_and_digest ok",
        "exactness: sample_formats_and_mutations ok",
        "exactness: jpeg_coefficients ok",
        "exactness: stage_verify_modes ok",
        "exactness: metadata_policies ok",
        "exactness: preview_isolation ok",
        "exactness: cancel_and_faults ok",
        "exactness: all checks passed",
    ]
    for marker in required:
        if marker not in stdout:
            return fail(f"{label} output missing checkpoint marker: {marker}")
    for line in stdout.strip().splitlines():
        print(f"  [{label}] {line}")
    return 0


def case_exactness(all_targets):
    cc = os.environ.get("CC", "cc")
    inc = os.path.join(ROOT, "converter", "include")
    root_inc = os.path.join(ROOT, "include")
    src_dir = os.path.join(ROOT, "converter", "src")

    with tempfile.TemporaryDirectory() as tmp:
        if _check_headers(tmp):
            return 1

        # Prove verify.c + metadata.c (+ job.c/scheduler.c) compile, link, and execute without preview.c.
        noprev_c = os.path.join(tmp, "verify_no_preview.c")
        noprev_exe = os.path.join(tmp, "verify_no_preview" + (".exe" if os.name == "nt" else ""))
        with open(noprev_c, "w", encoding="utf-8") as f:
            f.write(
                '#include "rfc_verify.h"\n'
                '#include <string.h>\n'
                'int main(void) {\n'
                '    uint8_t px[12] = {1,2,3,4,5,6,7,8,9,10,11,12};\n'
                '    rfc_frame fr = {{RFC_SAMPLE_RGB8, RFC_ALPHA_NONE, 2u, 2u, 6u, 8u},\n'
                '                    {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}, NULL, 0u, NULL, 0u}, px, 12u, NULL};\n'
                '    rfc_sample_mismatch mm;\n'
                '    if (rfc_verify_frames_equal(&fr, 1u, &fr, 1u, &mm) != RFC_OK) return 1;\n'
                '    rfc_metadata m = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u, {0}, NULL, 0u, NULL, 0u};\n'
                '    rfc_metadata_report mr;\n'
                '    if (rfc_metadata_verify(RFC_FORMAT_PNG, RFC_META_POLICY_PRESERVE,\n'
                '                            RFC_EXACT_PIXEL_EXACT, &m, &fr, 1u,\n'
                '                            &m, &fr, 1u, 0u, &mr, &mm) != RFC_OK) return 2;\n'
                '    return 0;\n'
                '}\n'
            )
        noprev_cmd = [
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc,
            os.path.join(src_dir, "registry.c"),
            os.path.join(src_dir, "limits.c"),
            os.path.join(src_dir, "verify.c"),
            os.path.join(src_dir, "metadata.c"),
            os.path.join(src_dir, "input.c"),
            os.path.join(src_dir, "output.c"),
            os.path.join(src_dir, "metrics.c"),
            os.path.join(src_dir, "job.c"),
            os.path.join(src_dir, "scheduler.c"),
            os.path.join(src_dir, "gif_scan.c"),
            os.path.join(src_dir, "heif_scan.c"),
            noprev_c,
        ]
        if os.name != "nt":
            noprev_cmd.append("-pthread")
        noprev_cmd.extend(["-o", noprev_exe])
        if run(noprev_cmd):
            return fail("verify.c + metadata.c + job.c failed to link without preview.c")
        if run([noprev_exe]):
            return fail("verify.c + metadata.c + job.c standalone link smoke failed")
        print("  [host] verify.c + metadata.c + job.c link cleanly without preview.c: PASS")

        if os.name == "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build-converter.py"),
                    "--target", "windows-x64"]):
                return fail("windows-x64 converter build failed for rfc_test_exactness")
            host_exe = os.path.join(ROOT, "bin", "converter", "windows-x64", "rfc_test_exactness.exe")
        else:
            librf = os.path.join(ROOT, "bin", "linux-x64", "lib", "librasterfold.a")
            if not os.path.isfile(librf):
                if run([sys.executable, os.path.join(ROOT, "scripts", "build.py"),
                        "--target", "linux-x64"]):
                    return fail("failed to build linux-x64 librasterfold.a for exactness tests")
            sources = [
                os.path.join(src_dir, "registry.c"),
                os.path.join(src_dir, "limits.c"),
                os.path.join(src_dir, "verify.c"),
                os.path.join(src_dir, "metadata.c"),
                os.path.join(src_dir, "preview.c"),
                os.path.join(src_dir, "input.c"),
                os.path.join(src_dir, "output.c"),
                os.path.join(src_dir, "metrics.c"),
                os.path.join(src_dir, "job.c"),
                os.path.join(src_dir, "scheduler.c"),
                os.path.join(src_dir, "gif_scan.c"),
                os.path.join(src_dir, "heif_scan.c"),
                os.path.join(ROOT, "tests", "converter", "test_exactness.c"),
            ]
            host_exe = os.path.join(tmp, "test_exactness")
            cmd = [
                cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", inc, "-I", root_inc, *sources, librf, "-pthread", "-o", host_exe,
            ]
            if run(cmd):
                return fail("exactness host test does not build")

        res = run_capture([host_exe], timeout=60.0)
        if res.returncode:
            print(res.stdout)
            print(res.stderr, file=sys.stderr)
            return fail(f"exactness host test exited {res.returncode}")
        if _assert_exactness_output("host", res.stdout):
            return 1
        print("exactness (host): PASS")

        venv_py = os.path.join(ROOT, ".venv", "bin", "python")
        py_bin = venv_py if (os.name != "nt" and os.path.isfile(venv_py)) else sys.executable
        py_env = dict(os.environ)
        py_env["RFC_TEST_EXACTNESS_EXE"] = host_exe
        py_env["RFC_REQUIRE_EXACTNESS_EXE"] = "1"
        py_cmd = [
            py_bin, "-m", "unittest", "discover", "-s",
            os.path.join(ROOT, "tests", "converter"), "-p", "test_exactness.py", "-v",
        ]
        if run(py_cmd, timeout=60.0, env=py_env):
            return fail("exactness python reference suite failed")
        print("exactness (python reference & oracle): PASS")

    if all_targets or os.name == "nt":
        if run([sys.executable, os.path.join(ROOT, "scripts", "build-converter.py"),
                "--target", "windows-x64"]):
            return fail("windows-x64 converter build failed for rfc_test_exactness")
        from pathlib import Path
        test_conv_dir = os.path.join(ROOT, "tests", "converter")
        if test_conv_dir not in sys.path:
            sys.path.insert(0, test_conv_dir)
        import test_bundle

        win_exe = Path(ROOT) / "bin" / "converter" / "windows-x64" / "rfc_test_exactness.exe"
        if not win_exe.is_file():
            return fail(f"missing built Windows binary: {win_exe}")
        pe_dlls = test_bundle.parse_pe64_imports(win_exe)
        ro_dlls = test_bundle.readobj_imports(win_exe)
        if set(pe_dlls) != set(ro_dlls):
            return fail(f"PE vs llvm-readobj import mismatch on {win_exe}: {pe_dlls} vs {ro_dlls}")
        try:
            test_bundle.validate_pe_imports(pe_dlls)
        except ValueError as exc:
            return fail(f"{win_exe} PE import check failed: {exc}")
        print(f"  [windows-x64] PE imports OK: {', '.join(pe_dlls)}")

        win_res = test_bundle.run_windows_binary_isolated(win_exe, timeout_sec=60.0)
        if win_res.returncode:
            print(win_res.stdout)
            print(win_res.stderr, file=sys.stderr)
            return fail(f"rfc_test_exactness.exe isolated run exited {win_res.returncode}")
        if _assert_exactness_output("windows-x64", win_res.stdout):
            return 1
        print("exactness (windows-x64 isolated): PASS")

    print("exactness: PASS")
    return 0


def _load_conv_module(name, filename):
    import importlib.util
    path = os.path.join(ROOT, "tests", "converter", filename)
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load converter test helper {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _assert_file_safety_output(out_text, label):
    required = [
        "file-safety: sniffing ok",
        "file-safety: conflicts_and_bounds ok",
        "file-safety: path_identity_and_refusals ok",
        "file-safety: disk_full_and_cleanup ok",
        "file-safety: callback_ownership_and_retry ok",
        "file-safety: commit_cancel_serialization ok",
        "file-safety: fold_roundtrip ok",
        "file-safety: all checks passed",
    ]
    for marker in required:
        if marker not in out_text:
            return fail(f"{label} missing marker: {marker}\n{out_text}")
    return 0


def case_file_safety(all_targets=False):
    import tempfile
    from pathlib import Path

    _ref = _load_conv_module("_rfc_ref", "reference.py")
    _tb = _load_conv_module("_rfc_bundle", "test_bundle.py")

    inc = os.path.join(ROOT, "converter", "include")
    rf_inc = os.path.join(ROOT, "include")
    src_dir = os.path.join(ROOT, "converter", "src")
    win_exe = Path(ROOT) / "bin" / "converter" / "windows-x64" / "rfc_test_file_safety.exe"

    with tempfile.TemporaryDirectory(prefix="rfc_fs_gate_") as tmp:
        host_exe = ""
        if os.name != "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build.py"), "--target", "linux-x64"]):
                return fail("linux-x64 static library build failed for file-safety")
            rf_lib = os.path.join(ROOT, "bin", "linux-x64", "lib", "librasterfold.a")
            if not os.path.isfile(rf_lib):
                return fail(f"missing {rf_lib}")
            cc = os.environ.get("CC", "cc")
            host_exe = os.path.join(tmp, "test_file_safety")
            sources = [
                os.path.join(src_dir, "registry.c"),
                os.path.join(src_dir, "limits.c"),
                os.path.join(src_dir, "verify.c"),
                os.path.join(src_dir, "metadata.c"),
                os.path.join(src_dir, "preview.c"),
                os.path.join(src_dir, "input.c"),
                os.path.join(src_dir, "output.c"),
                os.path.join(src_dir, "metrics.c"),
                os.path.join(src_dir, "job.c"),
                os.path.join(src_dir, "scheduler.c"),
                os.path.join(src_dir, "gif_scan.c"),
                os.path.join(src_dir, "heif_scan.c"),
                os.path.join(ROOT, "tests", "converter", "test_file_safety.c"),
            ]
            cmd = [
                cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-I", inc, "-I", rf_inc,
                *sources, rf_lib, "-lm", "-pthread", "-o", host_exe,
            ]
            if run(cmd):
                return fail("test_file_safety failed to build cleanly on host")
            res = _ref.run_process_tree(
                [host_exe], cwd=ROOT, capture_output=True, text=True, timeout=120.0
            )
            if res.stdout:
                print(res.stdout.rstrip(), flush=True)
            if res.stderr:
                print(res.stderr.rstrip(), file=sys.stderr, flush=True)
            if res.returncode != 0:
                return fail(f"host test_file_safety failed with rc={res.returncode}")
            if _assert_file_safety_output(res.stdout or "", "host test_file_safety"):
                return 1

        if all_targets or os.name == "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build-converter.py"), "--target", "windows-x64"]):
                return fail("windows-x64 converter build failed for file-safety")
            if not win_exe.is_file():
                return fail(f"missing Windows test binary {win_exe}")
            dlls = _tb.parse_pe64_imports(win_exe)
            _tb.validate_pe_imports(dlls)
            ro_dlls = _tb.readobj_imports(win_exe)
            _tb.validate_pe_imports(ro_dlls)
            wres = _tb.run_windows_binary_isolated(win_exe, timeout_sec=120.0)
            if hasattr(wres, "stdout"):
                w_rc = wres.returncode
                w_out = wres.stdout or ""
                w_err = wres.stderr or ""
            else:
                w_rc = 0
                w_out = str(wres)
                w_err = ""
            if w_out:
                print(w_out.rstrip(), flush=True)
            if w_err:
                print(w_err.rstrip(), file=sys.stderr, flush=True)
            if w_rc != 0:
                return fail(f"windows-x64 rfc_test_file_safety.exe failed with rc={w_rc}")
            if _assert_file_safety_output(w_out, "windows-x64 rfc_test_file_safety.exe"):
                return 1

        env = dict(os.environ)
        env["RFC_HOST_FILE_SAFETY_EXE"] = host_exe if host_exe else str(win_exe)
        if (all_targets or os.name == "nt") and win_exe.is_file():
            env["RFC_WIN_FILE_SAFETY_EXE"] = str(win_exe)
        py_res = _ref.run_process_tree(
            [sys.executable, os.path.join(ROOT, "tests", "converter", "test_file_safety.py"), "-v"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=240.0
        )
        if py_res.stdout:
            print(py_res.stdout.rstrip(), flush=True)
        if py_res.stderr:
            print(py_res.stderr.rstrip(), file=sys.stderr, flush=True)
        if py_res.returncode != 0:
            return fail(f"test_file_safety.py failed with rc={py_res.returncode}")

    print("file-safety: PASS")
    return 0


def _assert_reports_output(out_text, label):
    required = [
        "reports: stage_timings_and_retry ok",
        "reports: size_outcomes_and_callback_readback ok",
        "reports: sequential_compare_and_ranking ok",
        "reports: cancel_and_partial_iterations ok",
        "reports: provenance_and_mixed_configs ok",
        "reports: json_and_history_chain ok",
        "reports: round1_audit_regressions ok",
        "reports: all checks passed",
    ]
    for marker in required:
        if marker not in out_text:
            return fail(f"{label} missing marker: {marker}\n{out_text}")
    return 0


def case_reports(all_targets=False):
    from pathlib import Path

    _ref = _load_conv_module("_rfc_ref", "reference.py")
    _tb = _load_conv_module("_rfc_bundle", "test_bundle.py")

    inc = os.path.join(ROOT, "converter", "include")
    rf_inc = os.path.join(ROOT, "include")
    src_dir = os.path.join(ROOT, "converter", "src")
    win_exe = Path(ROOT) / "bin" / "converter" / "windows-x64" / "rfc_test_reports.exe"

    with tempfile.TemporaryDirectory(prefix="rfc_rep_gate_") as tmp:
        if _check_headers(tmp):
            return 1

        # Prove metrics.c links cleanly without preview.c alongside core verify/job modules.
        if os.name != "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build.py"), "--target", "linux-x64"]):
                return fail("linux-x64 static library build failed for reports")
            rf_lib = os.path.join(ROOT, "bin", "linux-x64", "lib", "librasterfold.a")
            if not os.path.isfile(rf_lib):
                return fail(f"missing {rf_lib}")
            cc = os.environ.get("CC", "cc")
            prov_dir = os.path.join(tmp, "gen_prov")
            prov_hdr = os.path.join(prov_dir, "rfc_build_provenance.h")
            engine_flags = "-std=c11 -Wall -Wextra -Werror -pthread"
            if run([
                sys.executable,
                os.path.join(ROOT, "scripts", "gen_converter_provenance.py"),
                "--target", "linux-x64",
                "--engine-cc", cc,
                "--engine-flags", engine_flags,
                "--out", prov_hdr,
            ]):
                return fail("gen_converter_provenance.py failed on host")
            host_exe = os.path.join(tmp, "test_reports")
            sources = [
                os.path.join(src_dir, "registry.c"),
                os.path.join(src_dir, "limits.c"),
                os.path.join(src_dir, "verify.c"),
                os.path.join(src_dir, "metadata.c"),
                os.path.join(src_dir, "preview.c"),
                os.path.join(src_dir, "input.c"),
                os.path.join(src_dir, "output.c"),
                os.path.join(src_dir, "metrics.c"),
                os.path.join(src_dir, "compare.c"),
                os.path.join(src_dir, "report.c"),
                os.path.join(src_dir, "job.c"),
                os.path.join(src_dir, "scheduler.c"),
                os.path.join(src_dir, "gif_scan.c"),
                os.path.join(src_dir, "heif_scan.c"),
                os.path.join(ROOT, "tests", "converter", "test_reports.c"),
            ]
            cmd = [
                cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-DRFC_HAVE_GENERATED_PROVENANCE=1",
                "-I", prov_dir, "-I", inc, "-I", rf_inc,
                *sources, rf_lib, "-lm", "-pthread", "-o", host_exe,
            ]
            if run(cmd):
                return fail("test_reports failed to build cleanly on host")
            res = _ref.run_process_tree(
                [host_exe], cwd=ROOT, capture_output=True, text=True, timeout=120.0
            )
            if res.stdout:
                print(res.stdout.rstrip(), flush=True)
            if res.stderr:
                print(res.stderr.rstrip(), file=sys.stderr, flush=True)
            if res.returncode != 0:
                return fail(f"host test_reports failed with rc={res.returncode}")
            if _assert_reports_output(res.stdout or "", "host test_reports"):
                return 1
        else:
            host_exe = ""

        if all_targets or os.name == "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build-converter.py"), "--target", "windows-x64"]):
                return fail("windows-x64 converter build failed for reports")
            if not win_exe.is_file():
                return fail(f"missing Windows test binary {win_exe}")
            dlls = _tb.parse_pe64_imports(win_exe)
            _tb.validate_pe_imports(dlls)
            ro_dlls = _tb.readobj_imports(win_exe)
            _tb.validate_pe_imports(ro_dlls)
            wres = _tb.run_windows_binary_isolated(win_exe, timeout_sec=120.0)
            w_rc = wres.returncode if hasattr(wres, "stdout") else 0
            w_out = (wres.stdout or "") if hasattr(wres, "stdout") else str(wres)
            w_err = (wres.stderr or "") if hasattr(wres, "stdout") else ""
            if w_out:
                print(w_out.rstrip(), flush=True)
            if w_err:
                print(w_err.rstrip(), file=sys.stderr, flush=True)
            if w_rc != 0:
                return fail(f"windows-x64 rfc_test_reports.exe failed with rc={w_rc}")
            if _assert_reports_output(w_out, "windows-x64 rfc_test_reports.exe"):
                return 1

        venv_py = os.path.join(ROOT, ".venv", "bin", "python")
        py_bin = venv_py if (os.name != "nt" and os.path.isfile(venv_py)) else sys.executable
        env = dict(os.environ)
        env["RFC_HOST_REPORTS_EXE"] = host_exe if host_exe else str(win_exe)
        if (all_targets or os.name == "nt") and win_exe.is_file():
            env["RFC_WIN_REPORTS_EXE"] = str(win_exe)
        py_res = _ref.run_process_tree(
            [py_bin, os.path.join(ROOT, "tests", "converter", "test_reports.py"), "-v"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=240.0
        )
        if py_res.stdout:
            print(py_res.stdout.rstrip(), flush=True)
        if py_res.stderr:
            print(py_res.stderr.rstrip(), file=sys.stderr, flush=True)
        if py_res.returncode != 0:
            return fail(f"test_reports.py failed with rc={py_res.returncode}")

    print("reports: PASS")
    return 0


def _assert_suite_output(out_text, label, required):
    for marker in required:
        if marker not in out_text:
            return fail(f"{label} missing marker: {marker}\n{out_text}")
    for line in out_text.strip().splitlines():
        print(f"  [{label}] {line}")
    return 0


def _host_pkgconfig_flags(packages, fallback_cflags, fallback_libs):
    import shlex
    res = run_capture(["pkg-config", "--cflags", "--libs", *packages])
    if res.returncode == 0 and res.stdout.strip():
        cflags = []
        libs = []
        for tok in shlex.split(res.stdout.strip()):
            if tok.startswith(("-l", "-L", "-Wl")):
                libs.append(tok)
            else:
                cflags.append(tok)
        return cflags, libs
    return list(fallback_cflags), list(fallback_libs)


def _host_libpng_flags():
    return _host_pkgconfig_flags(
        ["libpng", "zlib"], ["-I/usr/include/libpng16"], ["-lpng16", "-lz"]
    )


def _host_libwebp_flags():
    from pathlib import Path

    deps_prefix = Path(ROOT) / "build" / "converter" / "linux-x64" / "deps-prefix"
    lib_dir = deps_prefix / "lib"
    inc_dir = deps_prefix / "include"
    bundled = [
        lib_dir / "libwebpmux.a",
        lib_dir / "libwebpdemux.a",
        lib_dir / "libwebp.a",
        lib_dir / "libsharpyuv.a",
    ]
    if (inc_dir / "webp" / "decode.h").is_file() and all(p.is_file() for p in bundled):
        return [f"-I{inc_dir.as_posix()}"], [*(str(p) for p in bundled), "-lm", "-pthread"]
    cflags, libs = _host_pkgconfig_flags(
        ["libwebpmux", "libwebpdemux", "libwebp"],
        [],
        ["-lwebpmux", "-lwebpdemux", "-lwebp", "-lm"],
    )
    if "-lm" not in libs:
        libs.append("-lm")
    return cflags, libs


def _ensure_host_vendor_deps(pkg_order):
    import hashlib
    import shutil
    import subprocess
    from pathlib import Path

    root_p = Path(ROOT)
    deps_prefix = root_p / "build" / "converter" / "linux-x64" / "deps-prefix"
    deps_build = root_p / "build" / "converter" / "linux-x64" / "deps-build"
    stamps_dir = root_p / "build" / "converter" / "linux-x64" / "deps-stamps"
    deps_prefix.mkdir(parents=True, exist_ok=True)
    stamps_dir.mkdir(parents=True, exist_ok=True)

    cmake_pin = root_p / "tools" / "cmake-linux" / "bin" / "cmake"
    ninja_pin = root_p / "tools" / "ninja-linux" / "ninja"
    nasm_pin = root_p / "tools" / "nasm-linux" / "bin" / "nasm"
    cmake_exe = str(cmake_pin) if cmake_pin.is_file() else (shutil.which("cmake") or "cmake")
    ninja_exe = str(ninja_pin) if ninja_pin.is_file() else shutil.which("ninja")
    nasm_exe = str(nasm_pin) if nasm_pin.is_file() else (shutil.which("nasm") or "nasm")

    cc_env = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or "cc"
    cxx_env = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or "c++"
    vendor = json.loads((root_p / "converter" / "vendor.json").read_text(encoding="utf-8"))
    by_id = {p["id"]: p for p in vendor["packages"]}
    pfx = deps_prefix.as_posix()

    resolved_stamps = {}
    for pid in pkg_order:
        pkg = by_id[pid]
        src_dir = root_p / "tools" / "converter-src" / pid
        if not src_dir.is_dir():
            raise RuntimeError(f"missing converter source tree for host build: {src_dir}")
        wrapped = pid in ("lcms2", "giflib")
        cmake_src = (root_p / "converter" / "cmake" / pid) if wrapped else src_dir
        extra = []
        if wrapped:
            extra.append(f"-DVENDOR_SOURCE_DIR={src_dir.as_posix()}")
        elif pid == "libjxl":
            extra.extend([
                f"-DLCMS2_INCLUDE_DIR={pfx}/include",
                f"-DLCMS2_LIBRARIES={(deps_prefix / 'lib' / 'liblcms2.a').as_posix()}",
                f"-DBROTLI_INCLUDE_DIR={pfx}/include",
                f"-DBROTLICOMMON_LIBRARY={(deps_prefix / 'lib' / 'libbrotlicommon.a').as_posix()}",
                f"-DBROTLIDEC_LIBRARY={(deps_prefix / 'lib' / 'libbrotlidec.a').as_posix()}",
                f"-DBROTLIENC_LIBRARY={(deps_prefix / 'lib' / 'libbrotlienc.a').as_posix()}",
            ])
        elif pid == "libwebp":
            extra.extend([
                "-DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_GIF=ON",
            ])
        elif pid == "libtiff":
            extra.extend([
                "-Dtiff-cxx=OFF",
                f"-DDeflate_INCLUDE_DIR={pfx}/include",
                f"-DDeflate_LIBRARY={(deps_prefix / 'lib' / 'libdeflate.a').as_posix()}",
                f"-DJPEG_INCLUDE_DIR={pfx}/include",
                f"-DJPEG_LIBRARY={(deps_prefix / 'lib' / 'libjpeg.a').as_posix()}",
                f"-DWebP_INCLUDE_DIR={pfx}/include",
                f"-DWebP_LIBRARY={(deps_prefix / 'lib' / 'libwebp.a').as_posix()}",
                "-DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_LibLZMA=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_LERC=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_JBIG=ON",
            ])
        elif pid == "libaom":
            extra.append(f"-DCMAKE_ASM_NASM_COMPILER={nasm_exe}")
        elif pid == "libavif":
            extra.extend([
                f"-DAOM_INCLUDE_DIR={pfx}/include",
                f"-DAOM_LIBRARY={(deps_prefix / 'lib' / 'libaom.a').as_posix()}",
                f"-DLIBSHARPYUV_INCLUDE_DIR={pfx}/include/webp",
                f"-DLIBSHARPYUV_LIBRARY={(deps_prefix / 'lib' / 'libsharpyuv.a').as_posix()}",
            ])
        cfg_flags = [
            f"-DCMAKE_C_COMPILER={cc_env}",
            f"-DCMAKE_CXX_COMPILER={cxx_env}",
            f"-DCMAKE_INSTALL_PREFIX={pfx}",
            "-DCMAKE_INSTALL_LIBDIR=lib",
            f"-DCMAKE_PREFIX_PATH={pfx}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            *pkg.get("cmake_options", []),
            *extra,
        ]
        if ninja_exe:
            cfg_flags = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja_exe}", *cfg_flags]
        stamp_payload = json.dumps(
            {
                "id": pid,
                "version": pkg["version"],
                "sha256": pkg["sha256"],
                "cfg": cfg_flags,
                "deps": {d: resolved_stamps[d] for d in pkg.get("deps", []) if d in resolved_stamps},
            },
            sort_keys=True,
        ).encode("utf-8")
        want_stamp = hashlib.sha256(stamp_payload).hexdigest()
        stamp_file = stamps_dir / f"{pid}.stamp"
        if stamp_file.is_file() and stamp_file.read_text(encoding="utf-8").strip() == want_stamp:
            resolved_stamps[pid] = want_stamp
            continue
        bdir = deps_build / pid
        if bdir.exists():
            shutil.rmtree(bdir)
        subprocess.run([cmake_exe, "-S", cmake_src.as_posix(), "-B", bdir.as_posix(), *cfg_flags], cwd=ROOT, check=True)
        subprocess.run([cmake_exe, "--build", bdir.as_posix(), "--parallel"], cwd=ROOT, check=True)
        subprocess.run([cmake_exe, "--install", bdir.as_posix()], cwd=ROOT, check=True)
        stamp_file.write_text(want_stamp + "\n", encoding="utf-8")
        resolved_stamps[pid] = want_stamp

    return deps_prefix


def _ensure_host_jxl_flags():
    deps_prefix = _ensure_host_vendor_deps(["highway", "brotli", "lcms2", "libjxl"])
    cflags = [
        f"-I{(deps_prefix / 'include').as_posix()}",
        "-DJXL_STATIC_DEFINE",
        "-DJXL_THREADS_STATIC_DEFINE",
        "-DJXL_CMS_STATIC_DEFINE",
    ]
    lib_dir = deps_prefix / "lib"
    libs = [
        str(lib_dir / "libjxl.a"),
        str(lib_dir / "libjxl_threads.a"),
        str(lib_dir / "libjxl_cms.a"),
        str(lib_dir / "libhwy.a"),
        str(lib_dir / "libbrotlienc.a"),
        str(lib_dir / "libbrotlidec.a"),
        str(lib_dir / "libbrotlicommon.a"),
        str(lib_dir / "liblcms2.a"),
        "-lstdc++",
        "-lm",
        "-pthread",
    ]
    return cflags, libs


def _ensure_host_avif_flags():
    deps_prefix = _ensure_host_vendor_deps(["libwebp", "libaom", "libavif"])
    cflags = [f"-I{(deps_prefix / 'include').as_posix()}"]
    lib_dir = deps_prefix / "lib"
    libs = [
        str(lib_dir / "libavif.a"),
        str(lib_dir / "libaom.a"),
        str(lib_dir / "libsharpyuv.a"),
        "-lm",
        "-pthread",
    ]
    return cflags, libs


def _ensure_host_jpeg_flags():
    deps_prefix = _ensure_host_vendor_deps(["libjpeg-turbo"])
    lib_dir = deps_prefix / "lib"
    return [f"-I{(deps_prefix / 'include').as_posix()}"], [str(lib_dir / "libjpeg.a")]


def _ensure_host_tiff_gif_flags():
    deps_prefix = _ensure_host_vendor_deps(
        ["libdeflate", "libjpeg-turbo", "libwebp", "libtiff", "giflib"]
    )
    lib_dir = deps_prefix / "lib"
    libs = [
        str(lib_dir / "libtiff.a"),
        str(lib_dir / "libdeflate.a"),
        str(lib_dir / "libjpeg.a"),
        str(lib_dir / "libwebp.a"),
        str(lib_dir / "libsharpyuv.a"),
        str(lib_dir / "libgif.a"),
        "-lz",
        "-lm",
    ]
    return [f"-I{(deps_prefix / 'include').as_posix()}"], libs


def _run_codec_case(
    *,
    case_name,
    win_target,
    codec_sources,
    c_test_file,
    py_test_file,
    host_env_var,
    win_env_var,
    require_win_env_var,
    markers,
    host_flags_fn,
    all_targets=False,
    host_link_cxx=False,
    win_markers=None,
):
    from pathlib import Path

    if validate_registry():
        return 1

    _ref = _load_conv_module("_rfc_ref", "reference.py")
    _tb = _load_conv_module("_rfc_bundle", "test_bundle.py")

    inc = os.path.join(ROOT, "converter", "include")
    rf_inc = os.path.join(ROOT, "include")
    codecs_dir = os.path.join(ROOT, "converter", "codecs")
    src_dir = os.path.join(ROOT, "converter", "src")
    win_exe = Path(ROOT) / "bin" / "converter" / "windows-x64" / f"{win_target}.exe"

    with tempfile.TemporaryDirectory(prefix=f"rfc_{case_name}_gate_") as tmp:
        if _check_headers(tmp):
            return 1

        host_exe = ""
        if os.name != "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build.py"), "--target", "linux-x64"]):
                return fail(f"linux-x64 librasterfold.a build failed for {case_name}")
            rf_lib = os.path.join(ROOT, "bin", "linux-x64", "lib", "librasterfold.a")
            if not os.path.isfile(rf_lib):
                return fail(f"missing {rf_lib}")
            cc = os.environ.get("CC", "cc")
            extra_cflags, extra_libs = host_flags_fn()
            if "-lm" not in extra_libs:
                extra_libs = [*extra_libs, "-lm"]
            host_exe = os.path.join(tmp, f"test_{case_name}")
            sources = [
                os.path.join(src_dir, "registry.c"),
                os.path.join(src_dir, "limits.c"),
                os.path.join(src_dir, "verify.c"),
                os.path.join(src_dir, "metadata.c"),
                os.path.join(src_dir, "preview.c"),
                os.path.join(src_dir, "input.c"),
                os.path.join(src_dir, "output.c"),
                os.path.join(src_dir, "metrics.c"),
                os.path.join(src_dir, "compare.c"),
                os.path.join(src_dir, "report.c"),
                os.path.join(src_dir, "job.c"),
                os.path.join(src_dir, "scheduler.c"),
                os.path.join(src_dir, "gif_scan.c"),
                os.path.join(src_dir, "heif_scan.c"),
                *(os.path.join(codecs_dir, s) for s in codec_sources),
                os.path.join(ROOT, "tests", "converter", c_test_file),
            ]
            if host_link_cxx:
                objs = []
                for idx, src in enumerate(sources):
                    obj = os.path.join(tmp, f"u_{idx}.o")
                    c_cmd = [
                        cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-I", inc, "-I", rf_inc, "-I", codecs_dir, *extra_cflags,
                        "-c", src, "-o", obj,
                    ]
                    if run(c_cmd):
                        return fail(f"{case_name} host compile failed on {src}")
                    objs.append(obj)
                cxx = os.environ.get("CXX", "c++")
                l_cmd = [cxx, "-pthread", *objs, rf_lib, *extra_libs, "-o", host_exe]
                if run(l_cmd):
                    return fail(f"{case_name} host link failed")
            else:
                cmd = [
                    cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", inc, "-I", rf_inc, "-I", codecs_dir, *extra_cflags,
                    *sources, rf_lib, *extra_libs, "-o", host_exe,
                ]
                if run(cmd):
                    return fail(f"{case_name} host test does not build")
            res = run_capture([host_exe], timeout=60.0)
            if res.returncode:
                print(res.stdout)
                print(res.stderr, file=sys.stderr)
                return fail(f"{case_name} host test exited {res.returncode}")
            if _assert_suite_output(res.stdout, "host", markers):
                return 1
            print(f"{case_name} (host): PASS")

        if all_targets or os.name == "nt":
            if run([sys.executable, os.path.join(ROOT, "scripts", "build-converter.py"),
                    "--target", "windows-x64"]):
                return fail(f"windows-x64 converter build failed for {win_target}")
            if not win_exe.is_file():
                return fail(f"missing built Windows binary: {win_exe}")
            pe_dlls = _tb.parse_pe64_imports(win_exe)
            ro_dlls = _tb.readobj_imports(win_exe)
            if set(pe_dlls) != set(ro_dlls):
                return fail(f"PE vs llvm-readobj import mismatch on {win_exe}: {pe_dlls} vs {ro_dlls}")
            try:
                _tb.validate_pe_imports(pe_dlls)
            except ValueError as exc:
                return fail(f"{win_exe} PE import check failed: {exc}")
            print(f"  [windows-x64] PE imports OK: {', '.join(pe_dlls)}")

            win_res = _tb.run_windows_binary_isolated(win_exe, timeout_sec=60.0)
            if win_res.returncode:
                print(win_res.stdout)
                print(win_res.stderr, file=sys.stderr)
                return fail(f"{win_target}.exe isolated run exited {win_res.returncode}")
            if _assert_suite_output(win_res.stdout, "windows-x64",
                                    win_markers if win_markers else markers):
                return 1
            print(f"{case_name} (windows-x64 isolated): PASS")

        venv_py = os.path.join(ROOT, ".venv", "bin", "python")
        py_bin = venv_py if (os.name != "nt" and os.path.isfile(venv_py)) else sys.executable
        env = dict(os.environ)
        if host_exe:
            env[host_env_var] = host_exe
        if (all_targets or os.name == "nt") and win_exe.is_file():
            env[win_env_var] = str(win_exe)
            env[require_win_env_var] = "1"
        py_res = _ref.run_process_tree(
            [py_bin, os.path.join(ROOT, "tests", "converter", py_test_file), "-v"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=240.0
        )
        if py_res.stdout:
            print(py_res.stdout.rstrip(), flush=True)
        if py_res.stderr:
            print(py_res.stderr.rstrip(), file=sys.stderr, flush=True)
        if py_res.returncode != 0:
            return fail(f"{py_test_file} failed with rc={py_res.returncode}")
        if (all_targets or os.name == "nt") and (
            "skipped=" in (py_res.stderr or "") or "skipped=" in (py_res.stdout or "")
        ):
            return fail(f"{py_test_file} skipped tests under strict target mode")

    print(f"{case_name}: PASS")
    return 0


def case_basic(all_targets=False):
    return _run_codec_case(
        case_name="basic",
        win_target="rfc_test_basic",
        codec_sources=["basic.c"],
        c_test_file="test_basic.c",
        py_test_file="test_basic.py",
        host_env_var="RFC_HOST_BASIC_EXE",
        win_env_var="RFC_WIN_BASIC_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_BASIC",
        markers=[
            "basic: registration_and_fold_v2_v3 ok",
            "basic: bmp_depths_alpha_and_icc ok",
            "basic: png_depths_routes_budget_and_exif ok",
            "basic: pnm_and_pam_matrix ok",
            "basic: jobs_file_safety_and_cancel ok",
            "basic: all checks passed",
        ],
        host_flags_fn=_host_libpng_flags,
        all_targets=all_targets,
    )


def case_webp(all_targets=False):
    def _flags():
        w_cf, w_lb = _host_libwebp_flags()
        p_cf, p_lb = _host_libpng_flags()
        cflags = list(dict.fromkeys([*w_cf, *p_cf]))
        libs = [*w_lb, *p_lb]
        return cflags, libs

    return _run_codec_case(
        case_name="webp",
        win_target="rfc_test_webp",
        codec_sources=["basic.c", "webp.c"],
        c_test_file="test_webp.c",
        py_test_file="test_webp.py",
        host_env_var="RFC_HOST_WEBP_EXE",
        win_env_var="RFC_WIN_WEBP_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_WEBP",
        markers=[
            "webp: registration_and_upstream_contract ok",
            "webp: mux_icc_exif_xmp_and_cross_format ok",
            "webp: refusals_limits_and_budget ok",
            "webp: malformed_riff_and_cancellation ok",
            "webp: all checks passed",
        ],
        host_flags_fn=_flags,
        all_targets=all_targets,
    )


def case_jxl(all_targets=False):
    def _flags():
        j_cf, j_lb = _ensure_host_jxl_flags()
        w_cf, w_lb = _host_libwebp_flags()
        p_cf, p_lb = _host_libpng_flags()
        cflags = list(dict.fromkeys([*j_cf, *w_cf, *p_cf]))
        libs = [*j_lb, *w_lb, *p_lb]
        return cflags, libs

    return _run_codec_case(
        case_name="jxl",
        win_target="rfc_test_jxl",
        codec_sources=["basic.c", "webp.c", "jpeg_scan.c", "jxl.c"],
        c_test_file="test_jxl.c",
        py_test_file="test_jxl.py",
        host_env_var="RFC_HOST_JXL_EXE",
        win_env_var="RFC_WIN_JXL_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_JXL",
        markers=[
            "jxl: registration_and_upstream_contract ok",
            "jxl: depths_padding_alpha_and_xyb ok",
            "jxl: metadata_boxes_orientation_and_cross_format ok",
            "jxl: jpeg_reconstruction_and_exactness ok",
            "jxl: cancellation_limits_budget_and_file_safety ok",
            "jxl: all checks passed",
        ],
        host_flags_fn=_flags,
        all_targets=all_targets,
        host_link_cxx=True,
    )


def case_avif(all_targets=False):
    def _flags():
        a_cf, a_lb = _ensure_host_avif_flags()
        j_cf, j_lb = _ensure_host_jxl_flags()
        w_cf, w_lb = _host_libwebp_flags()
        p_cf, p_lb = _host_libpng_flags()
        cflags = list(dict.fromkeys([*a_cf, *j_cf, *w_cf, *p_cf]))
        a_core = [x for x in a_lb if x.endswith(".a") and not x.endswith("libsharpyuv.a")]
        a_tail = [x for x in a_lb if x.endswith("libsharpyuv.a") and x not in w_lb]
        j_arch = [x for x in j_lb if x.endswith(".a")]
        libs = [*a_core, *j_arch, *w_lb, *a_tail, *p_lb, "-lstdc++", "-lm", "-pthread"]
        return cflags, libs

    return _run_codec_case(
        case_name="avif",
        win_target="rfc_test_avif",
        codec_sources=["basic.c", "webp.c", "jpeg_scan.c", "jxl.c", "avif.c"],
        c_test_file="test_avif.c",
        py_test_file="test_avif.py",
        host_env_var="RFC_HOST_AVIF_EXE",
        win_env_var="RFC_WIN_AVIF_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_AVIF",
        markers=[
            "avif: registration_and_upstream_contract ok",
            "avif: lossless_depths_alpha_padding_and_import ok",
            "avif: metadata_colorimetry_and_orientation ok",
            "avif: byte_identical_and_cross_format ok",
            "avif: refusals_preflight_limits_and_cancel ok",
            "avif: all checks passed",
        ],
        host_flags_fn=_flags,
        all_targets=all_targets,
        host_link_cxx=True,
    )


def case_jpeg(all_targets=False):
    def _flags():
        g_cf, g_lb = _ensure_host_jpeg_flags()
        j_cf, j_lb = _ensure_host_jxl_flags()
        w_cf, w_lb = _host_libwebp_flags()
        p_cf, p_lb = _host_libpng_flags()
        cflags = list(dict.fromkeys([*g_cf, *j_cf, *w_cf, *p_cf]))
        j_arch = [x for x in j_lb if x.endswith(".a")]
        libs = [*g_lb, *j_arch, *w_lb, *p_lb, "-lstdc++", "-lm", "-pthread"]
        return cflags, libs

    return _run_codec_case(
        case_name="jpeg",
        win_target="rfc_test_jpeg",
        codec_sources=["basic.c", "webp.c", "jpeg_scan.c", "jxl.c", "jpeg.c"],
        c_test_file="test_jpeg.c",
        py_test_file="test_jpeg.py",
        host_env_var="RFC_HOST_JPEG_EXE",
        win_env_var="RFC_WIN_JPEG_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_JPEG",
        markers=[
            "jpeg: registration_and_upstream_contract ok",
            "jpeg: import_pinned_decoder_and_metadata ok",
            "jpeg: coefficient_optimization_and_markers ok",
            "jpeg: jobs_defaults_delegation_and_file_safety ok",
            "jpeg: refusals_errors_limits_and_cancel ok",
            "jpeg: all checks passed",
        ],
        host_flags_fn=_flags,
        all_targets=all_targets,
        host_link_cxx=True,
    )


def case_tiff_gif(all_targets=False):
    def _flags():
        t_cf, t_lb = _ensure_host_tiff_gif_flags()
        a_cf, a_lb = _ensure_host_avif_flags()
        j_cf, j_lb = _ensure_host_jxl_flags()
        w_cf, w_lb = _host_libwebp_flags()
        p_cf, p_lb = _host_libpng_flags()
        cflags = list(dict.fromkeys([*t_cf, *a_cf, *j_cf, *w_cf, *p_cf]))
        a_core = [x for x in a_lb if x.endswith(".a") and not x.endswith("libsharpyuv.a")]
        j_arch = [x for x in j_lb if x.endswith(".a")]
        libs = [*t_lb, *a_core, *j_arch, *w_lb, *p_lb, "-lstdc++", "-lm", "-pthread"]
        return cflags, libs

    return _run_codec_case(
        case_name="tiff_gif",
        win_target="rfc_test_tiff_gif",
        codec_sources=["basic.c", "webp.c", "jpeg_scan.c", "jxl.c", "avif.c", "jpeg.c", "tiff_gif.c"],
        c_test_file="test_tiff_gif.c",
        py_test_file="test_tiff_gif.py",
        host_env_var="RFC_HOST_TIFF_GIF_EXE",
        win_env_var="RFC_WIN_TIFF_GIF_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_TIFF_GIF",
        markers=[
            "tiff_gif: registration_and_geometry ok",
            "tiff_gif: tiff_import_matrix_and_refusals ok",
            "tiff_gif: tiff_export_and_metadata ok",
            "tiff_gif: gif_import_lzw_and_refusals ok",
            "tiff_gif: gif_export_palette_timing ok",
            "tiff_gif: jobs_limits_budget_and_cancel ok",
            "tiff_gif: review_round1_regressions ok",
            "tiff_gif: completion_gap_regressions ok",
            "tiff_gif: all checks passed",
        ],
        host_flags_fn=_flags,
        all_targets=all_targets,
        host_link_cxx=True,
    )


HEVC_NAMES = ("x265", "libde265", "de265", "kvazaar", "openhevc", "libheif")


def _no_hevc_in_build_inputs():
    """The converter's actual build inputs name no HEVC implementation: its
    sources and CMake (beyond the comments that say none is bundled), and
    the vendor manifest's packages."""
    import re as _re
    word = _re.compile(r"\b(" + "|".join(HEVC_NAMES) + r")\b", _re.I)
    roots = [os.path.join(ROOT, "converter", d) for d in ("src", "codecs", "include", "cmake")]
    files = [os.path.join(ROOT, "converter", "CMakeLists.txt")]
    for r in roots:
        for dp, _dn, fn in os.walk(r):
            files.extend(os.path.join(dp, f) for f in fn)
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for no, line in enumerate(fh, 1):
                code = line.split("//")[0]
                if word.search(code) and "/*" not in line and "*" != line.strip()[:1]:
                    return f"{os.path.relpath(path, ROOT)}:{no} names an HEVC implementation"
    with open(os.path.join(ROOT, "converter", "vendor.json"), encoding="utf-8") as fh:
        vendor = json.load(fh)
    for pkg in vendor.get("packages", []):
        if word.search(pkg.get("id", "")) or word.search(pkg.get("name", "")):
            return f"converter/vendor.json packages include {pkg.get('id')}"
    return None


HEVC_SYMBOL_PREFIXES = ("x265_", "de265_", "kvz_", "heif_context_", "heif_image_",
                        "heif_decode_", "heif_init", "heif_get_version")


def _no_hevc_in_link_inputs():
    """The resolved Windows link commands (the generated build.ninja files of
    the converter and its dependencies) and the dependency archives name no
    HEVC implementation."""
    import re as _re
    word = _re.compile(r"(?<![A-Za-z0-9])(" + "|".join(HEVC_NAMES) + r")(?![A-Za-z0-9])", _re.I)
    base = os.path.join(ROOT, "build", "converter", "windows-x64")
    for dp, _dn, fn in os.walk(base):
        for f in fn:
            if f == "build.ninja":
                path = os.path.join(dp, f)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    for no, line in enumerate(fh, 1):
                        if ("LINK_LIBRARIES" in line or "LINK_PATH" in line) and word.search(line):
                            return f"{os.path.relpath(path, ROOT)}:{no} links an HEVC implementation"
    deps = os.path.join(base, "deps-prefix", "lib")
    for f in sorted(os.listdir(deps)) if os.path.isdir(deps) else []:
        if word.search(f):
            return f"dependency archive {f} is an HEVC implementation"
    return None


def _no_hevc_symbols(paths):
    """llvm-nm finds no external symbol of an HEVC implementation (x265,
    libde265, kvazaar, or libheif's API) in the built artifacts; the
    adapter's own file-local helpers are not external."""
    tool = os.path.join(ROOT, "tools", "llvm-mingw-linux", "bin", "llvm-nm")
    if os.name == "nt":
        tool = os.path.join(ROOT, "tools", "llvm-mingw", "bin", "llvm-nm.exe")
    for path in paths:
        if not os.path.isfile(path):
            return f"missing artifact {path}"
        res = run_capture([tool, "--defined-only", "--extern-only", path], timeout=120.0)
        if res.returncode:
            return f"llvm-nm failed on {path}"
        for line in res.stdout.splitlines():
            sym = line.split()[-1] if line.split() else ""
            if sym.startswith(HEVC_SYMBOL_PREFIXES):
                return f"{os.path.relpath(path, ROOT)} defines HEVC symbol {sym}"
    return None


def case_heif(all_targets=False):
    msg = _no_hevc_in_build_inputs()
    if msg:
        return fail(f"heif: {msg}")
    print("heif: no HEVC implementation in the converter's build inputs")

    def _flags():
        w_cf, w_lb = _host_libwebp_flags()
        p_cf, p_lb = _host_libpng_flags()
        j_cf, j_lb = _ensure_host_jxl_flags()
        a_cf, a_lb = _ensure_host_avif_flags()
        t_cf, t_lb = _ensure_host_tiff_gif_flags()
        cflags = list(dict.fromkeys([*t_cf, *a_cf, *j_cf, *w_cf, *p_cf]))
        a_core = [x for x in a_lb if x.endswith(".a") and not x.endswith("libsharpyuv.a")]
        j_arch = [x for x in j_lb if x.endswith(".a")]
        libs = [*t_lb, *a_core, *j_arch, *w_lb, *p_lb, "-lstdc++", "-lm", "-pthread"]
        for lib in libs:
            if any(n in os.path.basename(lib).lower() for n in HEVC_NAMES):
                raise RuntimeError(f"heif: the host link list includes HEVC implementation {lib}")
        return cflags, libs

    markers_host = [
        "heif: registration_and_scanner ok",
        "heif: platform_refusal ok",
        "heif: all checks passed",
    ]
    rc = _run_codec_case(
        case_name="heif",
        win_target="rfc_test_heif",
        codec_sources=["basic.c", "webp.c", "jpeg_scan.c", "jxl.c", "avif.c", "jpeg.c", "tiff_gif.c", "heif.c"],
        c_test_file="test_heif.c",
        py_test_file="test_heif.py",
        host_env_var="RFC_HOST_HEIF_EXE",
        win_env_var="RFC_WIN_HEIF_EXE",
        require_win_env_var="RFC_REQUIRE_WIN_HEIF",
        markers=markers_host,
        win_markers=[
            "heif: registration_and_scanner ok",
            "heif: wic_decode_matrix ok",
            "heif: wic_metadata_refusals_limits_and_cancel ok",
            "heif: all checks passed",
        ],
        host_flags_fn=_flags,
        all_targets=all_targets,
        host_link_cxx=True,
    )
    if rc:
        return rc
    if all_targets or os.name == "nt":
        msg = _no_hevc_in_link_inputs()
        if msg:
            return fail(f"heif: {msg}")
        print("heif: no HEVC implementation among the Windows link inputs and dependency archives")
        bindir = os.path.join(ROOT, "bin", "converter", "windows-x64")
        deps = os.path.join(ROOT, "build", "converter", "windows-x64", "deps-prefix", "lib")
        arts = [os.path.join(bindir, n) for n in ("rfc_test_heif.exe", "rfc_bundle_smoke.exe")]
        arts += sorted(os.path.join(deps, f) for f in os.listdir(deps) if f.endswith(".a"))
        msg = _no_hevc_symbols(arts)
        if msg:
            return fail(f"heif: {msg}")
        print(f"heif: llvm-nm finds no HEVC symbol in {len(arts)} built Windows artifacts and archives")
    return 0


def main():
    raw_args = sys.argv[1:]
    all_targets = "--all-targets" in raw_args
    args = [a for a in raw_args if a != "--all-targets"]
    if not args:
        args = ["--case", "all"]
    if args == ["--case", "contract"]:
        return case_contract()
    if args == ["--case", "bundle"]:
        return case_bundle()
    if args == ["--case", "jobs"]:
        return case_jobs(all_targets)
    if args == ["--case", "exactness"]:
        return case_exactness(all_targets)
    if args == ["--case", "file-safety"]:
        return case_file_safety(all_targets)
    if args == ["--case", "reports"]:
        return case_reports(all_targets)
    if args == ["--case", "basic"]:
        return case_basic(all_targets)
    if args == ["--case", "webp"]:
        return case_webp(all_targets)
    if args == ["--case", "jxl"]:
        return case_jxl(all_targets)
    if args == ["--case", "avif"]:
        return case_avif(all_targets)
    if args == ["--case", "jpeg"]:
        return case_jpeg(all_targets)
    if args == ["--case", "tiff_gif"]:
        return case_tiff_gif(all_targets)
    if args == ["--case", "heif"]:
        return case_heif(all_targets)
    if args == ["--case", "all"]:
        rc = case_contract()
        if rc:
            return rc
        rc = case_jobs(all_targets)
        if rc:
            return rc
        rc = case_exactness(all_targets)
        if rc:
            return rc
        rc = case_file_safety(all_targets)
        if rc:
            return rc
        rc = case_reports(all_targets)
        if rc:
            return rc
        rc = case_basic(all_targets)
        if rc:
            return rc
        rc = case_webp(all_targets)
        if rc:
            return rc
        rc = case_jxl(all_targets)
        if rc:
            return rc
        rc = case_avif(all_targets)
        if rc:
            return rc
        rc = case_jpeg(all_targets)
        if rc:
            return rc
        rc = case_tiff_gif(all_targets)
        if rc:
            return rc
        rc = case_heif(all_targets)
        if rc:
            return rc
        return case_bundle()
    print("usage: check-converter.py --case <contract|jobs|exactness|file-safety|reports|basic|webp|jxl|avif|jpeg|tiff_gif|heif|bundle|all> [--all-targets]")
    return 1


if __name__ == "__main__":
    sys.exit(main())
