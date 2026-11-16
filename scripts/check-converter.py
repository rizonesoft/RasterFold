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
META_ITEMS = ("icc", "orientation", "frame-timing")


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
        if row.get("availability") == "usable" and m and int(m.group(1)) not in shipped:
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
    open(tu, "w").write('#include "rfc_codec.h"\n#include "rfc_io.h"\n#include "rfc_job.h"\n#include "rfc_verify.h"\nint main(void){return 0;}\n')
    if run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", inc, "-c", tu, "-o", os.path.join(tmp, "tu.o")]):
        return fail("headers do not compile as C11")
    tux = os.path.join(tmp, "tu.cpp")
    open(tux, "w").write('#include "rfc_codec.h"\n#include "rfc_io.h"\n#include "rfc_job.h"\n#include "rfc_verify.h"\nint main(){return 0;}\n')
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
        os.path.join(src_dir, "job.c"),
        os.path.join(src_dir, "scheduler.c"),
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
                '                    {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u}, px, 12u, NULL};\n'
                '    rfc_sample_mismatch mm;\n'
                '    if (rfc_verify_frames_equal(&fr, 1u, &fr, 1u, &mm) != RFC_OK) return 1;\n'
                '    rfc_metadata m = {NULL, 0u, RFC_ORIENTATION_NORMAL, 0u};\n'
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
            os.path.join(src_dir, "job.c"),
            os.path.join(src_dir, "scheduler.c"),
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
                os.path.join(src_dir, "job.c"),
                os.path.join(src_dir, "scheduler.c"),
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
                os.path.join(src_dir, "job.c"),
                os.path.join(src_dir, "scheduler.c"),
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


def main():
    raw_args = sys.argv[1:]
    all_targets = "--all-targets" in raw_args
    args = [a for a in raw_args if a != "--all-targets"]
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
        return case_bundle()
    print("usage: check-converter.py --case <contract|jobs|exactness|file-safety|bundle|all> [--all-targets]")
    return 1


if __name__ == "__main__":
    sys.exit(main())
