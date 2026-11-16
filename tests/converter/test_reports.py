#!/usr/bin/env python3
"""End-to-end host and Windows comparison/measurement/report/history suite for D10 T01 s6."""
from __future__ import annotations

import atexit
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
KODIM_PATH = ROOT / "testdata" / "photos" / "kodim23.png"
KODIM_EXPECTED_SHA256 = "97adc1006e4c306d6639b5a2d60fe4ea7ca79eed666897945fd69e6ef924bcd6"


def _load_mod(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module from {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_ref = _load_mod("_rfc_ref_rep", ROOT / "tests" / "converter" / "reference.py")
_rfi_ref = _load_mod("_rf_oracle_rep", ROOT / "tests" / "rfi_ref.py")

_TEMP_COMPILE_DIRS: list[tempfile.TemporaryDirectory[str]] = []


def _cleanup_compile_dirs() -> None:
    while _TEMP_COMPILE_DIRS:
        d = _TEMP_COMPILE_DIRS.pop()
        d.cleanup()


atexit.register(_cleanup_compile_dirs)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def resolve_host_exe() -> Path:
    env_exe = os.environ.get("RFC_HOST_REPORTS_EXE", "")
    if env_exe and Path(env_exe).is_file():
        return Path(env_exe)

    if os.name == "nt":
        win_exe = ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_reports.exe"
        if not win_exe.is_file():
            res = _ref.run_process_tree(
                [sys.executable, str(ROOT / "scripts" / "build-converter.py"), "--target", "windows-x64"],
                cwd=ROOT,
                capture_output=True,
                timeout=300.0,
            )
            if res.returncode != 0:
                raise RuntimeError(f"build-converter failed:\n{res.stdout}\n{res.stderr}")
        return win_exe

    rf_lib = ROOT / "bin" / "linux-x64" / "lib" / "librasterfold.a"
    if not rf_lib.is_file():
        res = _ref.run_process_tree(
            [sys.executable, str(ROOT / "scripts" / "build.py"), "--target", "linux-x64"],
            cwd=ROOT,
            capture_output=True,
            timeout=180.0,
        )
        if res.returncode != 0:
            raise RuntimeError(f"build.py linux-x64 failed:\n{res.stdout}\n{res.stderr}")

    tmp = tempfile.TemporaryDirectory(prefix="rfc_rep_host_bin_")
    _TEMP_COMPILE_DIRS.append(tmp)
    exe = Path(tmp.name) / "test_reports"
    prov_dir = Path(tmp.name) / "gen_prov"
    prov_hdr = prov_dir / "rfc_build_provenance.h"
    cc = os.environ.get("CC", "cc")
    engine_flags = "-std=c11 -Wall -Wextra -Werror -pthread"
    prov_res = _ref.run_process_tree(
        [
            sys.executable,
            str(ROOT / "scripts" / "gen_converter_provenance.py"),
            "--target",
            "linux-x64",
            "--engine-cc",
            cc,
            "--engine-flags",
            engine_flags,
            "--out",
            str(prov_hdr),
        ],
        cwd=ROOT,
        capture_output=True,
        timeout=60.0,
    )
    if prov_res.returncode != 0:
        raise RuntimeError(f"gen_converter_provenance failed:\n{prov_res.stdout}\n{prov_res.stderr}")
    src_dir = ROOT / "converter" / "src"
    sources = [
        src_dir / "registry.c",
        src_dir / "limits.c",
        src_dir / "verify.c",
        src_dir / "metadata.c",
        src_dir / "preview.c",
        src_dir / "input.c",
        src_dir / "output.c",
        src_dir / "metrics.c",
        src_dir / "compare.c",
        src_dir / "report.c",
        src_dir / "job.c",
        src_dir / "scheduler.c",
        ROOT / "tests" / "converter" / "test_reports.c",
    ]
    cmd = [
        cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pthread",
        "-DRFC_HAVE_GENERATED_PROVENANCE=1",
        "-I",
        str(prov_dir),
        "-I",
        str(ROOT / "converter" / "include"),
        "-I",
        str(ROOT / "include"),
        *(str(s) for s in sources),
        str(rf_lib),
        "-lm",
        "-pthread",
        "-o",
        str(exe),
    ]
    res = _ref.run_process_tree(cmd, cwd=ROOT, capture_output=True, timeout=120.0)
    if res.returncode != 0:
        raise RuntimeError(f"host compile failed:\n{res.stdout}\n{res.stderr}")
    return exe


def resolve_win_exe() -> Path | None:
    env_exe = os.environ.get("RFC_WIN_REPORTS_EXE", "")
    if env_exe and Path(env_exe).is_file():
        return Path(env_exe)
    candidate = ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_reports.exe"
    if candidate.is_file():
        return candidate
    return None


def ps_quote(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def verify_history_jsonl_independently(raw: bytes, expected_count: int) -> list[dict]:
    assert raw.endswith(b"\n"), "history log missing trailing newline"
    lines = raw.decode("utf-8").splitlines()
    assert len(lines) == expected_count, f"expected {expected_count} entries, got {len(lines)}"
    prev_hex = "0" * 64
    parsed = []
    marker = ',"entry_sha256":"'
    for idx, line in enumerate(lines, 1):
        obj = json.loads(line)
        assert obj["schema"] == "rasterfold.converter.history.entry"
        assert obj["schema_version"] == 1
        assert obj["sequence"] == idx
        assert obj["prev_entry_sha256"] == prev_hex
        assert isinstance(obj["acknowledged_unsupported_metadata"], int) and not isinstance(
            obj["acknowledged_unsupported_metadata"], bool
        )
        assert isinstance(obj["configuration_notes"], str) and obj["configuration_notes"]
        assert "limits" in obj and isinstance(obj["limits"], dict)
        assert obj["limits"]["max_width"] > 0
        for hc in obj["candidates"]:
            assert isinstance(hc["measurement_only"], bool)
            assert isinstance(hc["failing_stage"], str)
            assert isinstance(hc["effective_threads"], int)
            assert isinstance(hc["settings"], list)
            pc = hc["provider_config"]
            assert isinstance(pc, dict)
            for pk in (
                "provider_name",
                "codec_version",
                "compiler_id",
                "target_abi",
                "build_flags",
                "simd_features",
                "threading_features",
            ):
                assert isinstance(pc[pk], str) and pc[pk], f"missing provider_config.{pk}"
            vf = hc["verification"]
            assert isinstance(vf, dict)
            assert isinstance(vf["valid"], bool)
            assert isinstance(vf["independent_decoder"], bool)
            for vk in ("status", "exactness", "meta_outcome", "decoder_provenance"):
                assert isinstance(vf[vk], str)
            for hk in ("source_hash", "verified_hash"):
                assert isinstance(vf[hk], str) and len(vf[hk]) == 64
            assert isinstance(hc["produced_sha256"], str)
            assert len(hc["produced_sha256"]) in (0, 64)
            assert isinstance(hc["final_publish_ns"], int)
        pos = line.rfind(marker)
        assert pos > 0 and line.endswith('"}')
        canonical = (line[:pos] + "}").encode("utf-8")
        calc_hex = sha256_bytes(canonical)
        assert obj["entry_sha256"] == calc_hex, (
            f"entry {idx} hash mismatch: expected {calc_hex}, got {obj['entry_sha256']}"
        )
        prev_hex = calc_hex
        parsed.append(obj)
    return parsed


def assert_kodim_report(rep: dict, expected_req_threads: int) -> None:
    assert rep["schema"] == "rasterfold.converter.report"
    assert rep["schema_version"] == 1
    assert rep["run_status"] == "ok"
    assert rep["history_status"] == "ok"
    assert rep["history_recorded"] is True
    assert rep["ranking_complete"] is True
    assert rep["cancelled"] is False

    src = rep["source"]
    assert src["format"] == "png"
    assert src["file_bytes"] == 556888
    assert src["raw_frame_bytes"] == 768 * 512 * 3
    assert src["sha256"] == KODIM_EXPECTED_SHA256

    mc = rep["measurement_conditions"]
    assert mc["requested_threads"] == expected_req_threads
    if expected_req_threads == 1:
        assert mc["effective_threads"] == 1
    else:
        assert mc["effective_threads"] >= 2
    assert mc["warmup_iterations"] == 1
    assert mc["measured_iterations"] == 2
    assert mc["sequential_execution"] is True
    assert "limits" in mc and isinstance(mc["limits"], dict)
    assert mc["limits"]["max_width"] > 0
    assert rep["mixed_provider_configurations"] is True
    assert "simd_features" in rep["configuration_notes"]

    by_fmt = {c["target_format"]: c for c in rep["candidates"]}
    fold_c = by_fmt["rasterfold"]
    bmp_c = by_fmt["bmp"]
    webp_c = by_fmt["webp"]
    heic_c = by_fmt["heic"]

    assert fold_c["outcome"] == "completed"
    assert fold_c["rank"] == 1
    assert fold_c["has_produced_output"] is True
    assert fold_c["measurement_only"] is False
    assert fold_c["size"]["outcome"] == "smaller"
    assert fold_c["size"]["produced_file_bytes"] < 556888
    assert fold_c["verification"]["status"] == "ok"
    assert fold_c["verification"]["exactness"] == "pixel-exact"
    assert fold_c["verification"]["decoder_provenance"] == "same-provider-fresh-session"
    assert "final_publish_ns" in fold_c["timings_ns"]
    assert fold_c["timings_ns"]["median"]["valid"] is True
    assert fold_c["timings_ns"]["median"]["wall_ns"] >= (
        fold_c["timings_ns"]["median"]["import_ns"]
        + fold_c["timings_ns"]["median"]["encode_ns"]
        + fold_c["timings_ns"]["median"]["verify_ns"]
        + fold_c["timings_ns"]["median"]["export_ns"]
    )

    assert bmp_c["outcome"] == "completed"
    assert bmp_c["rank"] == 2
    assert bmp_c["has_produced_output"] is True
    assert bmp_c["measurement_only"] is False
    assert bmp_c["size"]["outcome"] == "growth"
    assert bmp_c["size"]["produced_file_bytes"] == 1179702
    assert bmp_c["size"]["growth_bytes"] == 1179702 - 556888

    for unsup in (webp_c, heic_c):
        assert unsup["outcome"] == "unsupported"
        assert unsup["rank"] == 0
        assert unsup["has_produced_output"] is False
        assert unsup["measurement_only"] is False
        assert unsup["size"]["outcome"] == "not-applicable"
        assert unsup["size"]["produced_file_bytes"] == 0


def expected_vendor_versions() -> dict[str, str]:
    vdoc = json.loads((ROOT / "converter" / "vendor.json").read_text(encoding="utf-8"))
    rf_version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    out = {
        "rasterfold": rf_version,
        "bmp": "builtin-c11",
        "pnm": "builtin-c11",
        "pam": "builtin-c11",
        "heif": "system-wic",
        "heic": "system-wic",
    }
    pkg_map = {p["id"]: p for p in vdoc["packages"]}
    for p in vdoc["packages"]:
        ver = p["version"]
        if p["id"] == "libavif":
            ver = f"{p['version']}+aom-{pkg_map['libaom']['version']}"
        for fmt in p["formats"]:
            out[fmt] = ver
    return out


class TestHostReports(unittest.TestCase):
    host_exe: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.host_exe = resolve_host_exe()

    def test_host_c_suite_markers(self) -> None:
        res = _ref.run_process_tree([str(self.host_exe)], cwd=ROOT, capture_output=True, timeout=120.0)
        self.assertEqual(res.returncode, 0, res.stdout + "\n" + res.stderr)
        for marker in (
            "reports: stage_timings_and_retry ok",
            "reports: size_outcomes_and_callback_readback ok",
            "reports: sequential_compare_and_ranking ok",
            "reports: cancel_and_partial_iterations ok",
            "reports: provenance_and_mixed_configs ok",
            "reports: json_and_history_chain ok",
            "reports: round1_audit_regressions ok",
            "reports: all checks passed",
        ):
            self.assertIn(marker, res.stdout)

    def test_provider_configs_match_vendor_json(self) -> None:
        res = _ref.run_process_tree(
            [str(self.host_exe), "dump-provider-configs"],
            cwd=ROOT,
            capture_output=True,
            timeout=60.0,
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        expected = expected_vendor_versions()
        seen = {}
        rows = {}
        for line in res.stdout.strip().splitlines():
            parts = line.split("|")
            self.assertEqual(len(parts), 7, line)
            fmt, _prov, ver, comp, abi, simd, thr = parts
            self.assertTrue(all([ver, comp, abi, simd, thr]), line)
            seen[fmt] = ver
            rows[fmt] = (comp, abi, simd, thr)
        self.assertEqual(seen, expected)
        if os.name != "nt":
            bjson = json.loads((ROOT / "bin" / "linux-x64" / "build.json").read_text(encoding="utf-8"))
            self.assertEqual(rows["rasterfold"][0][:20], bjson["compiler"][:20])
            self.assertEqual(rows["rasterfold"][1], "linux-x64")

    def test_provenance_generator_tracks_cxx_flags_and_feature_toggles(self) -> None:
        def write_pkg(
            root: Path,
            pid: str,
            is_cxx: bool,
            c_flags: str,
            c_rel: str,
            cxx_flags: str,
            cxx_rel: str,
            toggles: dict[str, tuple[str, str]],
            ninja_flags: str,
            ninja_defs: str = "",
            ninja_includes: str = "",
        ) -> None:
            d = root / pid
            d.mkdir(parents=True, exist_ok=True)
            lines = [
                "CMAKE_BUILD_TYPE:STRING=Release",
                f"CMAKE_C_FLAGS:STRING={c_flags}",
                f"CMAKE_C_FLAGS_RELEASE:STRING={c_rel}",
                f"CMAKE_CXX_FLAGS:STRING={cxx_flags}",
                f"CMAKE_CXX_FLAGS_RELEASE:STRING={cxx_rel}",
            ]
            for k, (tp, v) in toggles.items():
                lines.append(f"{k}:{tp}={v}")
            (d / "CMakeCache.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
            rule = f"CXX_COMPILER__{pid}_Release" if is_cxx else f"C_COMPILER__{pid}_Release"
            inc_line = f"  INCLUDES = {ninja_includes}\n" if ninja_includes else ""
            (d / "build.ninja").write_text(
                f"build CMakeFiles/{pid}.dir/src.obj: {rule} src.c\n"
                f"  FLAGS = {ninja_flags}\n"
                f"{inc_line}"
                f"  DEFINES = {ninja_defs}\n",
                encoding="utf-8",
            )

        def parse_header(hdr_text: str) -> dict[str, dict[str, str]]:
            row_re = re.compile(
                r'^\s*\{(RFC_FORMAT_[A-Z0-9_]+),\s*"((?:[^"\\]|\\.)*)",\s*'
                r'"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*'
                r'"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*'
                r'"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\s*\}',
                re.MULTILINE,
            )
            parsed: dict[str, dict[str, str]] = {}
            for m in row_re.finditer(hdr_text):
                parsed[m.group(1)] = {
                    "provider_name": m.group(2),
                    "codec_version": m.group(3),
                    "compiler_id": m.group(4),
                    "target_abi": m.group(5),
                    "build_flags": m.group(6),
                    "simd_features": m.group(7),
                    "threading_features": m.group(8),
                }
            return parsed

        with tempfile.TemporaryDirectory(prefix="rfc_prov_syn_") as td:
            base = Path(td)
            deps_a = base / "deps_a"
            deps_b = base / "deps_b"

            # Keep C flags completely unchanged across A and B on C++ packages to prove C++ flags drive C++ provenance.
            write_pkg(
                deps_a,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={
                    "JPEGXL_ENABLE_THREADS": ("BOOL", "ON"),
                    "JPEGXL_ENABLE_HWY_SSE2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_SSE4": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX3": ("BOOL", "false"),
                    "JPEGXL_ENABLE_HWY_AVX3_DL": ("BOOL", "false"),
                },
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17",
            )
            write_pkg(
                deps_b,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -fno-vectorize",
                cxx_rel="-O2 -DNDEBUG",
                toggles={
                    "JPEGXL_ENABLE_THREADS": ("BOOL", "OFF"),
                    "JPEGXL_ENABLE_HWY_SSE2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_SSE4": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX2": ("BOOL", "false"),
                    "JPEGXL_ENABLE_HWY_AVX3": ("BOOL", "false"),
                    "JPEGXL_ENABLE_HWY_AVX3_DL": ("BOOL", "false"),
                },
                ninja_flags="-march=x86-64-v2 -fno-vectorize -O2 -DNDEBUG -std=c++20",
            )
            write_pkg(
                deps_a,
                "libraw",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"LIBRAW_USE_OPENMP": ("BOOL", "OFF")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=gnu++17",
            )
            write_pkg(
                deps_b,
                "libraw",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -fopenmp",
                cxx_rel="-O2 -DNDEBUG",
                toggles={"LIBRAW_USE_OPENMP": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -fopenmp -O2 -DNDEBUG -std=gnu++17",
            )
            write_pkg(
                deps_a,
                "openexr",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"OPENEXR_ENABLE_THREADING": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -pthread",
            )
            write_pkg(
                deps_b,
                "openexr",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"OPENEXR_ENABLE_THREADING": ("BOOL", "OFF")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )
            write_pkg(
                deps_a,
                "libwebp",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"WEBP_ENABLE_SIMD": ("BOOL", "ON"), "WEBP_USE_THREAD": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -pthread",
            )
            write_pkg(
                deps_b,
                "libwebp",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"WEBP_ENABLE_SIMD": ("BOOL", "OFF"), "WEBP_USE_THREAD": ("BOOL", "OFF")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )
            write_pkg(
                deps_a,
                "libpng",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"PNG_HARDWARE_OPTIMIZATIONS": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )
            write_pkg(
                deps_b,
                "libpng",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"PNG_HARDWARE_OPTIMIZATIONS": ("BOOL", "OFF")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )
            write_pkg(
                deps_a,
                "libjpeg-turbo",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"WITH_SIMD": ("BOOL", "ON"), "REQUIRE_SIMD": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )
            write_pkg(
                deps_b,
                "libjpeg-turbo",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"WITH_SIMD": ("BOOL", "OFF")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )

            hdr_a = base / "prov_a.h"
            hdr_b = base / "prov_b.h"
            for dpath, out_h in ((deps_a, hdr_a), (deps_b, hdr_b)):
                res = _ref.run_process_tree(
                    [
                        sys.executable,
                        str(ROOT / "scripts" / "gen_converter_provenance.py"),
                        "--target",
                        "windows-x64",
                        "--deps-build",
                        str(dpath),
                        "--out",
                        str(out_h),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=60.0,
                )
                self.assertEqual(res.returncode, 0, res.stdout + "\n" + res.stderr)

            pa = parse_header(hdr_a.read_text(encoding="utf-8"))
            pb = parse_header(hdr_b.read_text(encoding="utf-8"))
            self.assertEqual(len(pa), 19)
            self.assertEqual(len(pb), 19)

            self.assertNotIn("-DC_SENTINEL_ONLY", pa["RFC_FORMAT_JPEG_XL"]["build_flags"])
            self.assertNotIn("-fno-vectorize", pa["RFC_FORMAT_JPEG_XL"]["build_flags"])
            self.assertIn("-std=c++20", pb["RFC_FORMAT_JPEG_XL"]["build_flags"])
            self.assertIn("-fno-vectorize", pb["RFC_FORMAT_JPEG_XL"]["build_flags"])
            self.assertIn("-O2", pb["RFC_FORMAT_JPEG_XL"]["build_flags"])
            self.assertIn("SSE2,SSE4,AVX2", pa["RFC_FORMAT_JPEG_XL"]["simd_features"])
            self.assertIn("vectorize-disabled", pb["RFC_FORMAT_JPEG_XL"]["simd_features"])
            self.assertIn("worker threads", pa["RFC_FORMAT_JPEG_XL"]["threading_features"])
            self.assertIn("JPEGXL_ENABLE_THREADS=OFF", pb["RFC_FORMAT_JPEG_XL"]["threading_features"])

            self.assertNotIn("-fopenmp", pa["RFC_FORMAT_CAMERA_RAW"]["build_flags"])
            self.assertIn("-fopenmp", pb["RFC_FORMAT_CAMERA_RAW"]["build_flags"])
            self.assertIn("OpenMP disabled", pa["RFC_FORMAT_CAMERA_RAW"]["threading_features"])
            self.assertIn("OpenMP enabled", pb["RFC_FORMAT_CAMERA_RAW"]["threading_features"])

            self.assertIn("IlmThread", pa["RFC_FORMAT_OPENEXR"]["threading_features"])
            self.assertIn("OPENEXR_ENABLE_THREADING=OFF", pb["RFC_FORMAT_OPENEXR"]["threading_features"])

            self.assertIn("WEBP_ENABLE_SIMD=ON", pa["RFC_FORMAT_WEBP"]["simd_features"])
            self.assertIn("disabled (WEBP_ENABLE_SIMD=OFF", pb["RFC_FORMAT_WEBP"]["simd_features"])
            self.assertIn("WEBP_USE_THREAD=ON", pa["RFC_FORMAT_WEBP"]["threading_features"])
            self.assertIn("WEBP_USE_THREAD=OFF", pb["RFC_FORMAT_WEBP"]["threading_features"])

            self.assertIn("PNG_HARDWARE_OPTIMIZATIONS=ON", pa["RFC_FORMAT_PNG"]["simd_features"])
            self.assertIn("PNG_HARDWARE_OPTIMIZATIONS=OFF", pb["RFC_FORMAT_PNG"]["simd_features"])
            self.assertIn("REQUIRE_SIMD=ON", pa["RFC_FORMAT_JPEG"]["simd_features"])
            self.assertIn("WITH_SIMD=OFF", pb["RFC_FORMAT_JPEG"]["simd_features"])

            # Mutating ONLY -ffast-math in C++ flags must change libjxl build_flags
            deps_ff = base / "deps_ff"
            write_pkg(
                deps_ff,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2 -DC_SENTINEL_ONLY",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -ffast-math",
                cxx_rel="-O3 -DNDEBUG",
                toggles={
                    "JPEGXL_ENABLE_THREADS": ("BOOL", "ON"),
                    "JPEGXL_ENABLE_HWY_SSE2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_SSE4": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX3": ("BOOL", "false"),
                    "JPEGXL_ENABLE_HWY_AVX3_DL": ("BOOL", "false"),
                },
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17",
            )
            hdr_ff = base / "prov_ff.h"
            res_ff = _ref.run_process_tree(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "gen_converter_provenance.py"),
                    "--target",
                    "windows-x64",
                    "--deps-build",
                    str(deps_ff),
                    "--out",
                    str(hdr_ff),
                ],
                cwd=ROOT,
                capture_output=True,
                timeout=60.0,
            )
            self.assertEqual(res_ff.returncode, 0, res_ff.stdout + "\n" + res_ff.stderr)
            p_ff = parse_header(hdr_ff.read_text(encoding="utf-8"))
            self.assertIn("-ffast-math", p_ff["RFC_FORMAT_JPEG_XL"]["build_flags"])
            self.assertNotEqual(
                p_ff["RFC_FORMAT_JPEG_XL"]["build_flags"],
                pa["RFC_FORMAT_JPEG_XL"]["build_flags"],
            )

            # Overflow (>255 bytes) differing only in a trailing define must carry distinct digests
            long_prefix_defs = " ".join(
                f"-DJPEGXL_LONG_BUILD_DEFINE_{idx:02d}=VALUE_{idx:02d}" for idx in range(16)
            )
            deps_ov1 = base / "deps_ov1"
            deps_ov2 = base / "deps_ov2"
            for root_ov, tail_def in (
                (deps_ov1, "-DTAIL_VARIANT=alpha"),
                (deps_ov2, "-DTAIL_VARIANT=beta"),
            ):
                write_pkg(
                    root_ov,
                    "libjxl",
                    True,
                    c_flags="-march=x86-64-v2",
                    c_rel="-O3 -DNDEBUG",
                    cxx_flags="-march=x86-64-v2",
                    cxx_rel="-O3 -DNDEBUG",
                    toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                    ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17",
                    ninja_defs=f"{long_prefix_defs} {tail_def}",
                )
            hdr_ov1 = base / "prov_ov1.h"
            hdr_ov2 = base / "prov_ov2.h"
            for d_ov, h_ov in ((deps_ov1, hdr_ov1), (deps_ov2, hdr_ov2)):
                res_ov = _ref.run_process_tree(
                    [
                        sys.executable,
                        str(ROOT / "scripts" / "gen_converter_provenance.py"),
                        "--target",
                        "windows-x64",
                        "--deps-build",
                        str(d_ov),
                        "--out",
                        str(h_ov),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=60.0,
                )
                self.assertEqual(res_ov.returncode, 0, res_ov.stdout + "\n" + res_ov.stderr)
            p_ov1 = parse_header(hdr_ov1.read_text(encoding="utf-8"))
            p_ov2 = parse_header(hdr_ov2.read_text(encoding="utf-8"))
            bf1 = p_ov1["RFC_FORMAT_JPEG_XL"]["build_flags"]
            bf2 = p_ov2["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertLessEqual(len(bf1.encode("utf-8")), 255)
            self.assertLessEqual(len(bf2.encode("utf-8")), 255)
            self.assertIn("#[", bf1)
            self.assertIn("#[", bf2)
            self.assertNotEqual(bf1, bf2)

            # Ordered / repeated flags (-O3 -O0 vs -O3 -O0 -O3, last-flag-wins, and subdep changes)
            def run_prov(deps_dir: Path, out_hdr: Path) -> dict[str, dict[str, str]]:
                res_p = _ref.run_process_tree(
                    [
                        sys.executable,
                        str(ROOT / "scripts" / "gen_converter_provenance.py"),
                        "--target",
                        "windows-x64",
                        "--deps-build",
                        str(deps_dir),
                        "--out",
                        str(out_hdr),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=60.0,
                )
                self.assertEqual(res_p.returncode, 0, res_p.stdout + "\n" + res_p.stderr)
                return parse_header(out_hdr.read_text(encoding="utf-8"))

            deps_ord1 = base / "deps_ord1"
            deps_ord2 = base / "deps_ord2"
            write_pkg(
                deps_ord1,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -O0 -DNDEBUG",
                toggles={
                    "JPEGXL_ENABLE_THREADS": ("BOOL", "ON"),
                    "JPEGXL_ENABLE_HWY_SSE2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_SSE4": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX2": ("BOOL", "true"),
                },
                ninja_flags="-march=x86-64-v2 -fno-vectorize -fvectorize -O3 -O0 -DNDEBUG -std=c++17",
            )
            write_pkg(
                deps_ord2,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -O0 -O3 -DNDEBUG",
                toggles={
                    "JPEGXL_ENABLE_THREADS": ("BOOL", "ON"),
                    "JPEGXL_ENABLE_HWY_SSE2": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_SSE4": ("BOOL", "true"),
                    "JPEGXL_ENABLE_HWY_AVX2": ("BOOL", "true"),
                },
                ninja_flags="-march=x86-64-v2 -fvectorize -fno-vectorize -O3 -O0 -O3 -DNDEBUG -std=c++17",
            )
            write_pkg(
                deps_ord1,
                "libraw",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -fopenmp -fno-openmp",
            )
            write_pkg(
                deps_ord2,
                "libraw",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -fno-openmp -fopenmp",
            )
            write_pkg(
                deps_ord1,
                "libavif",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"ENABLE_MULTITHREAD": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -pthread",
            )
            write_pkg(
                deps_ord2,
                "libavif",
                False,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="",
                cxx_rel="",
                toggles={"ENABLE_MULTITHREAD": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -pthread",
            )
            write_pkg(
                deps_ord1,
                "libaom",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"CONFIG_MULTITHREAD": ("STRING", "1")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG",
            )
            write_pkg(
                deps_ord2,
                "libaom",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -fno-strict-aliasing",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"CONFIG_MULTITHREAD": ("STRING", "1")},
                ninja_flags="-march=x86-64-v2 -fno-strict-aliasing -O3 -DNDEBUG",
            )

            p_ord1 = run_prov(deps_ord1, base / "prov_ord1.h")
            p_ord2 = run_prov(deps_ord2, base / "prov_ord2.h")

            j1 = p_ord1["RFC_FORMAT_JPEG_XL"]
            j2 = p_ord2["RFC_FORMAT_JPEG_XL"]
            self.assertNotEqual(j1["build_flags"], j2["build_flags"])
            self.assertIn("-O3 -O0 -DNDEBUG", j1["build_flags"])
            self.assertIn("-O3 -O0 -O3 -DNDEBUG", j2["build_flags"])
            self.assertIn("SSE2,SSE4,AVX2", j1["simd_features"])
            self.assertIn("vectorize-disabled", j2["simd_features"])

            self.assertNotEqual(
                p_ord1["RFC_FORMAT_CAMERA_RAW"]["build_flags"],
                p_ord2["RFC_FORMAT_CAMERA_RAW"]["build_flags"],
            )
            self.assertIn("OpenMP disabled", p_ord1["RFC_FORMAT_CAMERA_RAW"]["threading_features"])
            self.assertIn("OpenMP enabled", p_ord2["RFC_FORMAT_CAMERA_RAW"]["threading_features"])

            self.assertNotEqual(
                p_ord1["RFC_FORMAT_AVIF"]["build_flags"],
                p_ord2["RFC_FORMAT_AVIF"]["build_flags"],
            )

            # Forced includes (-include / -imacros) must change provenance without leaking local paths
            deps_inc_a = base / "deps_inc_a"
            deps_inc_b = base / "deps_inc_b"
            deps_inc_same_dir1 = base / "deps_inc_same1"
            deps_inc_same_dir2 = base / "deps_inc_same2"
            real_hdr = base / "forced_cfg.h"
            real_hdr.write_text("#define JXL_CUSTOM_KNOB 1\n", encoding="utf-8")

            write_pkg(
                deps_inc_a,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -include /virtual/a.h",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -include /virtual/a.h",
            )
            write_pkg(
                deps_inc_b,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -include /virtual/b.h",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -include /virtual/b.h",
            )
            write_pkg(
                deps_inc_same_dir1,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -imacros /virtual/dir1/opts.h",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -imacros /virtual/dir1/opts.h",
            )
            write_pkg(
                deps_inc_same_dir2,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -imacros /virtual/dir2/opts.h",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -imacros /virtual/dir2/opts.h",
            )
            p_inc_a = run_prov(deps_inc_a, base / "prov_inc_a.h")["RFC_FORMAT_JPEG_XL"]
            p_inc_b = run_prov(deps_inc_b, base / "prov_inc_b.h")["RFC_FORMAT_JPEG_XL"]
            p_same1 = run_prov(deps_inc_same_dir1, base / "prov_same1.h")["RFC_FORMAT_JPEG_XL"]
            p_same2 = run_prov(deps_inc_same_dir2, base / "prov_same2.h")["RFC_FORMAT_JPEG_XL"]

            self.assertNotIn("/virtual/", p_inc_a["build_flags"])
            self.assertNotIn("/virtual/", p_inc_b["build_flags"])
            self.assertIn("-include:a.h", p_inc_a["build_flags"])
            self.assertIn("-include:b.h", p_inc_b["build_flags"])
            self.assertNotEqual(p_inc_a["build_flags"], p_inc_b["build_flags"])
            self.assertNotIn("/virtual/", p_same1["build_flags"])
            self.assertIn("-imacros:opts.h", p_same1["build_flags"])
            self.assertNotEqual(p_same1["build_flags"], p_same2["build_flags"])

            deps_real = base / "deps_real_hdr"
            write_pkg(
                deps_real,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags=f"-march=x86-64-v2 -include {real_hdr}",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                ninja_flags=f"-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -include {real_hdr}",
            )
            bf_hdr1 = run_prov(deps_real, base / "prov_hdr1.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotIn(str(base), bf_hdr1)
            self.assertIn("-include:forced_cfg.h", bf_hdr1)
            real_hdr.write_text("#define JXL_CUSTOM_KNOB 2\n", encoding="utf-8")
            bf_hdr2 = run_prov(deps_real, base / "prov_hdr2.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_hdr1, bf_hdr2)

            # Response files (@file), normalized search paths (-I), visibility, and CMake C++ standard
            for name, fl_a, fl_b, tg_a, tg_b in (
                (
                    "rsp",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 @/virtual/a.rsp",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 @/virtual/b.rsp",
                    {},
                    {},
                ),
                (
                    "incdir",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -I /virtual/inc1",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -I /virtual/inc2",
                    {},
                    {},
                ),
                (
                    "vis",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -fvisibility=default",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -fvisibility=hidden",
                    {},
                    {},
                ),
                (
                    "cxxstd",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17",
                    "-march=x86-64-v2 -O3 -DNDEBUG -std=c++17",
                    {"CMAKE_CXX_STANDARD": ("STRING", "17")},
                    {"CMAKE_CXX_STANDARD": ("STRING", "20")},
                ),
            ):
                d_x = base / f"deps_{name}_x"
                d_y = base / f"deps_{name}_y"
                write_pkg(
                    d_x,
                    "libjxl",
                    True,
                    c_flags="-march=x86-64-v2",
                    c_rel="-O3 -DNDEBUG",
                    cxx_flags=fl_a,
                    cxx_rel="-O3 -DNDEBUG",
                    toggles=tg_a,
                    ninja_flags=fl_a,
                )
                write_pkg(
                    d_y,
                    "libjxl",
                    True,
                    c_flags="-march=x86-64-v2",
                    c_rel="-O3 -DNDEBUG",
                    cxx_flags=fl_b,
                    cxx_rel="-O3 -DNDEBUG",
                    toggles=tg_b,
                    ninja_flags=fl_b,
                )
                rx = run_prov(d_x, base / f"prov_{name}_x.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
                ry = run_prov(d_y, base / f"prov_{name}_y.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
                self.assertNotIn("/virtual/", rx)
                self.assertNotIn("/virtual/", ry)
                self.assertNotEqual(rx, ry, f"failed differentiation for {name}")

            # Provider-local relative forced include (-include config.h and -I inc -include sub.h)
            deps_rel = base / "deps_rel_inc"
            write_pkg(
                deps_rel,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -include config.h",
                cxx_rel="-O3 -DNDEBUG",
                toggles={"JPEGXL_ENABLE_THREADS": ("BOOL", "ON")},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -include config.h -I inc -include sub.h",
            )
            rel_cfg = deps_rel / "libjxl" / "config.h"
            rel_cfg.write_text("#define FEATURE 1\n", encoding="utf-8")
            (deps_rel / "libjxl" / "inc").mkdir(parents=True, exist_ok=True)
            rel_sub = deps_rel / "libjxl" / "inc" / "sub.h"
            rel_sub.write_text("#define SUB_MODE 1\n", encoding="utf-8")
            bf_rel1 = run_prov(deps_rel, base / "prov_rel1.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            rel_cfg.write_text("#define FEATURE 2\n", encoding="utf-8")
            bf_rel2 = run_prov(deps_rel, base / "prov_rel2.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertIn("-include:config.h", bf_rel1)
            self.assertNotEqual(bf_rel1, bf_rel2)
            rel_sub.write_text("#define SUB_MODE 2\n", encoding="utf-8")
            bf_rel3 = run_prov(deps_rel, base / "prov_rel3.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_rel2, bf_rel3)

            # Shadowing regression: un-included source root config.h must not shadow -I inc/config.h
            src_shadow = base / "src_libjxl_shadow"
            src_shadow.mkdir(parents=True, exist_ok=True)
            decoy_cfg = src_shadow / "config.h"
            decoy_cfg.write_text("#define FEATURE decoy_1\n", encoding="utf-8")
            deps_shadow = base / "deps_shadow"
            write_pkg(
                deps_shadow,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2 -include config.h",
                cxx_rel="-O3 -DNDEBUG",
                toggles={
                    "CMAKE_HOME_DIRECTORY": ("INTERNAL", str(src_shadow)),
                    "libjxl_SOURCE_DIR": ("STATIC", str(src_shadow)),
                },
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -include config.h",
                ninja_includes="-I inc",
            )
            eff_inc_cfg = deps_shadow / "libjxl" / "inc" / "config.h"
            eff_inc_cfg.parent.mkdir(parents=True, exist_ok=True)
            eff_inc_cfg.write_text("#define FEATURE real_1\n", encoding="utf-8")
            bf_sh1 = run_prov(deps_shadow, base / "prov_sh1.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            decoy_cfg.write_text("#define FEATURE decoy_2\n", encoding="utf-8")
            bf_sh2 = run_prov(deps_shadow, base / "prov_sh2.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertEqual(bf_sh1, bf_sh2)
            eff_inc_cfg.write_text("#define FEATURE real_2\n", encoding="utf-8")
            bf_sh3 = run_prov(deps_shadow, base / "prov_sh3.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_sh2, bf_sh3)

            # Compiler search precedence for -include/-imacros: compile_dir -> -iquote -> -I -> -isystem -> -idirafter
            deps_prec = base / "deps_prec"
            write_pkg(
                deps_prec,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 -idirafter dir_after -isystem dir_sys -I dir_inc -iquote dir_quote -include prec.h",
            )
            pkg_prec = deps_prec / "libjxl"
            prec_files: dict[str, Path] = {}
            for dname in ("dir_after", "dir_sys", "dir_inc", "dir_quote"):
                dp = pkg_prec / dname
                dp.mkdir(parents=True, exist_ok=True)
                f = dp / "prec.h"
                f.write_text(f"#define SRC_{dname} 1\n", encoding="utf-8")
                prec_files[dname] = f
            bf_p1 = run_prov(deps_prec, base / "prov_p1.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            for shadow_d in ("dir_after", "dir_sys", "dir_inc"):
                prec_files[shadow_d].write_text(f"#define SRC_{shadow_d} 2\n", encoding="utf-8")
            self.assertEqual(bf_p1, run_prov(deps_prec, base / "prov_p2.h")["RFC_FORMAT_JPEG_XL"]["build_flags"])
            prec_files["dir_quote"].write_text("#define SRC_dir_quote 2\n", encoding="utf-8")
            bf_p3 = run_prov(deps_prec, base / "prov_p3.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_p1, bf_p3)
            prec_files["dir_quote"].unlink()
            bf_p4 = run_prov(deps_prec, base / "prov_p4.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_p3, bf_p4)
            prec_files["dir_sys"].write_text("#define SRC_dir_sys 3\n", encoding="utf-8")
            self.assertEqual(bf_p4, run_prov(deps_prec, base / "prov_p5.h")["RFC_FORMAT_JPEG_XL"]["build_flags"])
            prec_files["dir_inc"].write_text("#define SRC_dir_inc 3\n", encoding="utf-8")
            bf_p6 = run_prov(deps_prec, base / "prov_p6.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_p4, bf_p6)
            cwd_prec = pkg_prec / "prec.h"
            cwd_prec.write_text("#define SRC_cwd 1\n", encoding="utf-8")
            bf_p7 = run_prov(deps_prec, base / "prov_p7.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_p6, bf_p7)
            prec_files["dir_inc"].write_text("#define SRC_dir_inc 4\n", encoding="utf-8")
            self.assertEqual(bf_p7, run_prov(deps_prec, base / "prov_p8.h")["RFC_FORMAT_JPEG_XL"]["build_flags"])
            cwd_prec.write_text("#define SRC_cwd 2\n", encoding="utf-8")
            self.assertNotEqual(bf_p7, run_prov(deps_prec, base / "prov_p9.h")["RFC_FORMAT_JPEG_XL"]["build_flags"])

            # Real @response files: expand -include/-imacros/-I/defines and nested @file in compile_dir
            deps_rsp = base / "deps_rsp_real"
            write_pkg(
                deps_rsp,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 @opts.rsp",
                ninja_defs="@defs.rsp",
            )
            pkg_rsp = deps_rsp / "libjxl"
            (pkg_rsp / "rsp_inc").mkdir(parents=True, exist_ok=True)
            rsp_hdr = pkg_rsp / "rsp_inc" / "config.h"
            rsp_hdr.write_text("#define FEATURE 1\n", encoding="utf-8")
            (pkg_rsp / "rsp_dir").mkdir(parents=True, exist_ok=True)
            decoy_inner_hdr = pkg_rsp / "rsp_dir" / "nested_only.h"
            decoy_inner_hdr.write_text("#define NESTED 1\n", encoding="utf-8")
            (pkg_rsp / "rsp_dir" / "inner.rsp").write_text("-include rsp_dir/nested_only.h\n", encoding="utf-8")
            real_inner_hdr = pkg_rsp / "nested_real.h"
            real_inner_hdr.write_text("#define REAL_NEST 1\n", encoding="utf-8")
            (pkg_rsp / "inner.rsp").write_text("-include nested_real.h\n", encoding="utf-8")
            (pkg_rsp / "rsp_dir" / "outer.rsp").write_text("-I rsp_inc\n@inner.rsp\n", encoding="utf-8")
            (pkg_rsp / "opts.rsp").write_text("@rsp_dir/outer.rsp -include config.h\n", encoding="utf-8")
            (pkg_rsp / "defs.rsp").write_text("-DRSP_DEF=1\n", encoding="utf-8")
            bf_r1 = run_prov(deps_rsp, base / "prov_r1.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertIn("-include:config.h", bf_r1)
            self.assertIn("-DRSP_DEF=1", bf_r1)
            rsp_hdr.write_text("#define FEATURE 2\n", encoding="utf-8")
            bf_r2 = run_prov(deps_rsp, base / "prov_r2.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_r1, bf_r2)
            real_inner_hdr.write_text("#define REAL_NEST 2\n", encoding="utf-8")
            bf_r3 = run_prov(deps_rsp, base / "prov_r3.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_r2, bf_r3)
            decoy_inner_hdr.write_text("#define NESTED 2\n", encoding="utf-8")
            self.assertEqual(bf_r3, run_prov(deps_rsp, base / "prov_r4.h")["RFC_FORMAT_JPEG_XL"]["build_flags"])
            # Self-referential response-file cycle terminates safely
            (pkg_rsp / "cyc.rsp").write_text("@cyc.rsp -DACYC=1\n", encoding="utf-8")
            (pkg_rsp / "opts.rsp").write_text("@cyc.rsp\n", encoding="utf-8")
            bf_r5 = run_prov(deps_rsp, base / "prov_r5.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertIn("-DACYC=1", bf_r5)

            # Separate two-token -D / -U inside @defs.rsp and @opts.rsp
            deps_def_spc = base / "deps_def_spc"
            write_pkg(
                deps_def_spc,
                "libjxl",
                True,
                c_flags="-march=x86-64-v2",
                c_rel="-O3 -DNDEBUG",
                cxx_flags="-march=x86-64-v2",
                cxx_rel="-O3 -DNDEBUG",
                toggles={},
                ninja_flags="-march=x86-64-v2 -O3 -DNDEBUG -std=c++17 @opts_spc.rsp",
                ninja_defs="@defs.rsp",
            )
            pkg_def_spc = deps_def_spc / "libjxl"
            (pkg_def_spc / "opts_spc.rsp").write_text("-D FLAG_SPC=1 -U DROP_ME\n", encoding="utf-8")
            (pkg_def_spc / "defs.rsp").write_text("-D FEATURE=1\n", encoding="utf-8")
            bf_ds1 = run_prov(deps_def_spc, base / "prov_ds1.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertIn("-DFLAG_SPC=1", bf_ds1)
            self.assertIn("-UDROP_ME", bf_ds1)
            self.assertIn("-DFEATURE=1", bf_ds1)
            (pkg_def_spc / "defs.rsp").write_text("-D FEATURE=2\n", encoding="utf-8")
            bf_ds2 = run_prov(deps_def_spc, base / "prov_ds2.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertIn("-DFEATURE=2", bf_ds2)
            self.assertNotEqual(bf_ds1, bf_ds2)
            (pkg_def_spc / "defs.rsp").write_text("-U FEATURE\n", encoding="utf-8")
            bf_ds3 = run_prov(deps_def_spc, base / "prov_ds3.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertIn("-UFEATURE", bf_ds3)
            self.assertNotEqual(bf_ds2, bf_ds3)
            (pkg_def_spc / "opts_spc.rsp").write_text("-D FLAG_SPC=2 -U DROP_ME\n", encoding="utf-8")
            bf_ds4 = run_prov(deps_def_spc, base / "prov_ds4.h")["RFC_FORMAT_JPEG_XL"]["build_flags"]
            self.assertNotEqual(bf_ds3, bf_ds4)

    def test_kodim23_compare_threads_oracle_and_history(self) -> None:
        self.assertEqual(sha256_file(KODIM_PATH), KODIM_EXPECTED_SHA256)
        with Image.open(KODIM_PATH) as im:
            src_rgb = im.convert("RGB").tobytes()
            src_w, src_h = im.size

        with tempfile.TemporaryDirectory(prefix="rfc_kodim_host_") as td:
            d = Path(td)
            src_copy = d / "kodim23.png"
            shutil.copy2(KODIM_PATH, src_copy)
            d1 = d / "t1"
            ddef = d / "tdef"
            d1.mkdir()
            ddef.mkdir()
            j1 = d / "r1.json"
            jdef = d / "rdef.json"
            hist = d / "runs.jsonl"

            for th, out_d, jp, rid in (
                ("1", d1, j1, "kodim-host-t1"),
                ("0", ddef, jdef, "kodim-host-tdef"),
            ):
                res = _ref.run_process_tree(
                    [
                        str(self.host_exe),
                        "compare-kodim",
                        str(src_copy),
                        str(out_d),
                        th,
                        str(jp),
                        str(hist),
                        rid,
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=120.0,
                )
                self.assertEqual(res.returncode, 0, res.stdout + "\n" + res.stderr)

            r1 = json.loads(j1.read_text(encoding="utf-8"))
            rdef = json.loads(jdef.read_text(encoding="utf-8"))
            assert_kodim_report(r1, 1)
            assert_kodim_report(rdef, 0)

            fold1 = (d1 / "kodim23.fold").read_bytes()
            folddef = (ddef / "kodim23.fold").read_bytes()
            self.assertEqual(sha256_bytes(fold1), sha256_bytes(folddef))
            self.assertEqual(
                sha256_bytes(fold1),
                r1["candidates"][0]["size"]["produced_sha256"],
            )

            ow, oh, orgb = _rfi_ref.decode_image(fold1)
            self.assertEqual((ow, oh), (src_w, src_h))
            self.assertEqual(orgb, src_rgb)

            hist_raw = hist.read_bytes()
            entries = verify_history_jsonl_independently(hist_raw, 2)
            self.assertEqual(entries[0]["run_id"], "kodim-host-t1")
            self.assertEqual(entries[1]["run_id"], "kodim-host-tdef")
            self.assertEqual(entries[0]["source_sha256"], KODIM_EXPECTED_SHA256)
            hc0 = entries[0]["candidates"][0]
            self.assertEqual(hc0["provider_config"], r1["candidates"][0]["provider_config"])
            self.assertEqual(hc0["verification"]["exactness"], "pixel-exact")
            self.assertEqual(hc0["verification"]["decoder_provenance"], "same-provider-fresh-session")
            self.assertEqual(hc0["produced_sha256"], r1["candidates"][0]["size"]["produced_sha256"])
            self.assertEqual(hc0["final_publish_ns"], r1["candidates"][0]["timings_ns"]["final_publish_ns"])
            self.assertEqual(hc0["effective_threads"], 1)
            self.assertGreaterEqual(entries[1]["candidates"][0]["effective_threads"], 2)

            vres = _ref.run_process_tree(
                [str(self.host_exe), "verify-history", str(hist)],
                cwd=ROOT,
                capture_output=True,
                timeout=30.0,
            )
            self.assertEqual(vres.returncode, 0, vres.stdout + vres.stderr)
            self.assertIn("ok entries=2", vres.stdout)

            # Mutated entry 1 -> verify-history fails and file untouched
            tampered = d / "tampered.jsonl"
            tampered.write_bytes(hist_raw.replace(b"kodim-host-t1", b"kodim-host-X1"))
            t_before = tampered.read_bytes()
            tres = _ref.run_process_tree(
                [str(self.host_exe), "verify-history", str(tampered)],
                cwd=ROOT,
                capture_output=True,
                timeout=30.0,
            )
            self.assertNotEqual(tres.returncode, 0)
            self.assertIn("verify-mismatch", tres.stdout)
            self.assertEqual(tampered.read_bytes(), t_before)

            # Truncated trailing newline -> verify-history returns truncated-input
            trunc = d / "trunc.jsonl"
            trunc.write_bytes(hist_raw[:-1])
            trres = _ref.run_process_tree(
                [str(self.host_exe), "verify-history", str(trunc)],
                cwd=ROOT,
                capture_output=True,
                timeout=30.0,
            )
            self.assertNotEqual(trres.returncode, 0)
            self.assertIn("truncated-input", trres.stdout)

            print(
                f" [kodim23-host src={src_w}x{src_h} {KODIM_EXPECTED_SHA256[:12]}"
                f" fold={len(fold1)}B({sha256_bytes(fold1)[:12]}) bmp={(d1 / 'kodim23.bmp').stat().st_size}B]",
                end="",
                flush=True,
            )

        self.assertEqual(sha256_file(KODIM_PATH), KODIM_EXPECTED_SHA256)


class TestWindowsReports(unittest.TestCase):
    win_exe: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.win_exe = resolve_win_exe()
        if cls.win_exe is None:
            raise unittest.SkipTest("windows-x64 rfc_test_reports.exe not built")
        if os.name != "nt":
            res = _ref.run_process_tree(
                ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", "[IO.Path]::GetTempPath()"],
                capture_output=True,
                timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")

    def _run_ps(self, script: str, timeout: float = 120.0) -> str:
        res = _ref.run_process_tree(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
            cwd=ROOT,
            capture_output=True,
            timeout=timeout,
        )
        if res.returncode != 0:
            raise AssertionError(
                f"PowerShell failed (rc={res.returncode}):\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"
            )
        return res.stdout or ""

    def _win_to_py_path(self, win_path: str) -> Path:
        if os.name == "nt":
            return Path(win_path)
        res = _ref.run_process_tree(["wslpath", "-u", win_path], capture_output=True, timeout=15.0)
        if res.returncode != 0:
            raise RuntimeError(f"wslpath -u failed for {win_path}: {res.stderr}")
        return Path((res.stdout or "").strip())

    def test_windows_kodim23_and_history(self) -> None:
        assert self.win_exe is not None
        win_temp_dir = self._run_ps(
            "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_rep_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
            "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d"
        ).strip().splitlines()[-1].strip()
        py_temp = self._win_to_py_path(win_temp_dir)

        try:
            exe_dst = py_temp / "rfc_test_reports.exe"
            shutil.copy2(self.win_exe, exe_dst)
            src_copy = py_temp / "kodim23.png"
            shutil.copy2(KODIM_PATH, src_copy)
            (py_temp / "t1").mkdir()
            (py_temp / "tdef").mkdir()

            ps = f"""
$ErrorActionPreference = 'Stop'
$root = {ps_quote(win_temp_dir)}
$exe = Join-Path $root 'rfc_test_reports.exe'
$src = Join-Path $root 'kodim23.png'
$hist = Join-Path $root 'runs.jsonl'
$env:PATH = "$env:SystemRoot\\System32;$env:SystemRoot"

& $exe compare-kodim $src (Join-Path $root 't1') '1' (Join-Path $root 'r1.json') $hist 'kodim-win-t1'
if ($LASTEXITCODE -ne 0) {{ throw "compare-kodim t1 failed ($LASTEXITCODE)" }}
& $exe compare-kodim $src (Join-Path $root 'tdef') '0' (Join-Path $root 'rdef.json') $hist 'kodim-win-tdef'
if ($LASTEXITCODE -ne 0) {{ throw "compare-kodim tdef failed ($LASTEXITCODE)" }}
& $exe verify-history $hist
if ($LASTEXITCODE -ne 0) {{ throw "verify-history failed ($LASTEXITCODE)" }}
& $exe dump-provider-configs | Out-File -FilePath (Join-Path $root 'cfg.txt') -Encoding utf8
if ($LASTEXITCODE -ne 0) {{ throw "dump-provider-configs failed ($LASTEXITCODE)" }}
"""
            self._run_ps(ps, timeout=180.0)

            r1 = json.loads((py_temp / "r1.json").read_text(encoding="utf-8"))
            rdef = json.loads((py_temp / "rdef.json").read_text(encoding="utf-8"))
            assert_kodim_report(r1, 1)
            assert_kodim_report(rdef, 0)

            fold1 = (py_temp / "t1" / "kodim23.fold").read_bytes()
            folddef = (py_temp / "tdef" / "kodim23.fold").read_bytes()
            self.assertEqual(sha256_bytes(fold1), sha256_bytes(folddef))
            verify_history_jsonl_independently((py_temp / "runs.jsonl").read_bytes(), 2)

            expected = expected_vendor_versions()
            cfg_lines = (py_temp / "cfg.txt").read_text(encoding="utf-8-sig").strip().splitlines()
            seen = {}
            for line in cfg_lines:
                parts = line.split("|")
                self.assertEqual(len(parts), 7, line)
                self.assertEqual(parts[4], "windows-x64", line)
                if parts[0] == "jpeg-xl":
                    self.assertIn("AVX3_DL", parts[5], line)
                    self.assertNotIn(",AVX3)", parts[5], line)
                seen[parts[0]] = parts[2]
            self.assertEqual(seen, expected)
        finally:
            self._run_ps(
                f"if (Test-Path {ps_quote(win_temp_dir)}) {{ "
                f"Remove-Item -LiteralPath {ps_quote(win_temp_dir)} -Recurse -Force }}"
            )


if __name__ == "__main__":
    unittest.main()
