#!/usr/bin/env python3
"""Generate build-captured provider provenance C header for converter reports."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

FORMAT_ENUM_BY_NAME = {
    "rasterfold": "RFC_FORMAT_RASTERFOLD",
    "bmp": "RFC_FORMAT_BMP",
    "png": "RFC_FORMAT_PNG",
    "apng": "RFC_FORMAT_APNG",
    "pnm": "RFC_FORMAT_PNM",
    "pam": "RFC_FORMAT_PAM",
    "webp": "RFC_FORMAT_WEBP",
    "jpeg-xl": "RFC_FORMAT_JPEG_XL",
    "avif": "RFC_FORMAT_AVIF",
    "jpeg": "RFC_FORMAT_JPEG",
    "gif": "RFC_FORMAT_GIF",
    "tiff": "RFC_FORMAT_TIFF",
    "heif": "RFC_FORMAT_HEIF",
    "heic": "RFC_FORMAT_HEIC",
    "jpeg-2000": "RFC_FORMAT_JPEG2000",
    "qoi": "RFC_FORMAT_QOI",
    "openexr": "RFC_FORMAT_OPENEXR",
    "camera-raw": "RFC_FORMAT_CAMERA_RAW",
    "dng": "RFC_FORMAT_DNG",
}


def _compact_version(raw: str, max_len: int = 95) -> str:
    s = " ".join(raw.split()).strip()
    s = re.sub(r"\(https?://[^()\s]+\s+([0-9a-fA-F]{8,})\)", lambda m: f"({m.group(1)[:12]})", s)
    s = re.sub(r"\(https?://[^()]+\)", "", s).strip()
    s = " ".join(s.split())
    if len(s) > max_len:
        s = s[:max_len].rstrip()
    return s


def _probe_compiler_version(cc_cmd: str, cache: dict[str, str]) -> str:
    if not cc_cmd:
        return "c11-compiler"
    if cc_cmd in cache:
        return cache[cc_cmd]
    val = ""
    try:
        proc = subprocess.run(
            [cc_cmd, "--version"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            val = _compact_version(proc.stdout.splitlines()[0])
    except OSError:
        val = ""
    if not val:
        val = _compact_version(Path(cc_cmd).name)
    cache[cc_cmd] = val
    return val


def _parse_cmake_cache(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        if ":" not in line or "=" not in line:
            continue
        left, val = line.split("=", 1)
        key = left.split(":", 1)[0].strip()
        out[key] = val.strip()
    return out


def _c_escape(s: str, max_len: int) -> str:
    b = s.encode("utf-8", errors="replace")
    if len(b) > max_len:
        # Trim at UTF-8 codepoint boundary.
        cut = max_len
        while cut > 0 and (b[cut] & 0xC0) == 0x80:
            cut -= 1
        b = b[:cut]
    text = b.decode("utf-8", errors="ignore")
    out = ['"']
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append(f"\\x{ord(ch):02x}")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def _rasterfold_provenance(
    target: str,
    version_text: str,
    cc_cache: dict[str, str],
) -> tuple[str, str, str, str, str, str]:
    bjson_path = ROOT / "bin" / target / "build.json"
    compdb_path = ROOT / "build" / target / "compile_commands.json"

    product_ver = version_text
    compiler_id = ""
    portable = False
    if bjson_path.is_file():
        bdoc = json.loads(bjson_path.read_text(encoding="utf-8"))
        product_ver = str(bdoc.get("product_version") or version_text)
        if bdoc.get("compiler"):
            compiler_id = _compact_version(str(bdoc["compiler"]))
        portable = bool(bdoc.get("portable_simd", False))

    flags = ""
    if compdb_path.is_file():
        cdb = json.loads(compdb_path.read_text(encoding="utf-8"))
        cm_entry = None
        for ent in cdb:
            if Path(ent.get("file", "")).name == "cm.c":
                cm_entry = ent
                break
        if cm_entry is None and cdb:
            cm_entry = cdb[0]
        if cm_entry is not None:
            args = list(cm_entry.get("arguments") or shlex.split(cm_entry.get("command", "")))
            if args:
                if not compiler_id:
                    compiler_id = _probe_compiler_version(args[0], cc_cache)
                skip_next = False
                kept: list[str] = []
                for a in args[1:]:
                    if skip_next:
                        skip_next = False
                        continue
                    if a == "-o":
                        skip_next = True
                        continue
                    if a == "-c" or a.endswith(".c") or a.endswith(".o"):
                        continue
                    if a == "-DRF_PORTABLE_SIMD":
                        portable = True
                    kept.append(a)
                flags = " ".join(kept)

    if not compiler_id:
        llvm_dir = ROOT / ("tools/llvm-mingw/bin" if os.name == "nt" else "tools/llvm-mingw-linux/bin")
        clang_bin = llvm_dir / ("clang.exe" if os.name == "nt" else "clang")
        compiler_id = _probe_compiler_version(str(clang_bin) if clang_bin.is_file() else "clang", cc_cache)
    if not flags:
        triple = "x86_64-w64-mingw32" if target == "windows-x64" else "x86_64-linux-gnu"
        extra = "-ffreestanding -fno-stack-protector" if target == "windows-x64" else "-fPIC -pthread"
        pflag = " -DRF_PORTABLE_SIMD" if portable else ""
        flags = f"--target={triple} -std=c11 -O2 -g -Wall -Wextra -Werror -march=x86-64-v2 -mtune=generic{pflag} {extra}"

    simd = "portable-c11 (RF_PORTABLE_SIMD)" if portable else "x86-64-v2 + SSE2/SIMD paths"
    threads = (
        "win32-threads (strip/channel pool)"
        if target == "windows-x64"
        else "pthreads (strip/channel pool)"
    )
    return (product_ver, compiler_id, target, flags, simd, threads)


def _find_pkg_by_format(vendor: dict) -> tuple[dict[str, dict], dict[str, dict]]:
    by_id = {p["id"]: p for p in vendor["packages"]}
    by_fmt: dict[str, dict] = {}
    for p in vendor["packages"]:
        for fmt in p.get("formats", []):
            by_fmt[fmt] = p
    return by_id, by_fmt


def _extract_package_build_info(
    pkg: dict,
    pkg_by_id: dict[str, dict],
    deps_build_dir: Path | None,
    toolchain: dict,
    cc_cache: dict[str, str],
) -> tuple[str, str, str, str, str]:
    pid = pkg["id"]
    ver = pkg["version"]
    if pid == "libavif" and "libaom" in pkg_by_id:
        ver = f"{pkg['version']}+aom-{pkg_by_id['libaom']['version']}"

    nasm_ver = ""
    for comp in toolchain.get("components", []):
        if comp.get("name") == "nasm":
            nasm_ver = str(comp.get("version", ""))
            break

    llvm_dir = ROOT / ("tools/llvm-mingw/bin" if os.name == "nt" else "tools/llvm-mingw-linux/bin")
    default_cc = str(llvm_dir / ("x86_64-w64-mingw32-clang" + (".exe" if os.name == "nt" else "")))
    default_cxx = str(llvm_dir / ("x86_64-w64-mingw32-clang++" + (".exe" if os.name == "nt" else "")))

    cache: dict[str, str] = {}
    if deps_build_dir is not None:
        cache = _parse_cmake_cache(deps_build_dir / pid / "CMakeCache.txt")

    lang = str(pkg.get("language", "c")).lower()
    if "cxx" in lang or "c++" in lang:
        cc_path = cache.get("CMAKE_CXX_COMPILER") or default_cxx
    else:
        cc_path = cache.get("CMAKE_C_COMPILER") or default_cc
    compiler_id = _probe_compiler_version(cc_path, cc_cache)

    c_flags = cache.get("CMAKE_C_FLAGS_RELEASE") or cache.get("CMAKE_CXX_FLAGS_RELEASE") or "-O3 -DNDEBUG"
    init_flags = cache.get("CMAKE_C_FLAGS") or "-march=x86-64-v2"
    opts = [opt for opt in pkg.get("cmake_options", []) if not opt.endswith("OFF") and not opt.endswith("INSTALL=ON")]
    if pid == "libjpeg-turbo" and "-isystem" not in init_flags:
        init_flags = f"{init_flags} -DREQUIRE_SIMD=ON".strip()
    flags_parts = [f"-DCMAKE_BUILD_TYPE={cache.get('CMAKE_BUILD_TYPE', 'Release')}", init_flags, c_flags, *opts[:4]]
    build_flags = " ".join(p for p in flags_parts if p).strip()

    feats = pkg.get("features", {})
    simd = str(feats.get("simd") or "x86-64-v2")
    if feats.get("nasm") and nasm_ver and nasm_ver not in simd:
        simd = f"{simd} (NASM {nasm_ver})"
    if pid == "libraw":
        thr = "thread-safe instances (OpenMP disabled)"
    elif feats.get("threads"):
        thr = f"{pid} threads (budgeted)" if pid != "libwebp" else "libwebp threads (WEBP_USE_THREAD=ON)"
    else:
        thr = "single-threaded"
    return (ver, compiler_id, build_flags, simd, thr)


def generate(
    target: str,
    engine_cc: str,
    engine_flags: str,
    deps_build_dir: Path | None,
    out_path: Path,
) -> None:
    version_text = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    codecs = json.loads((ROOT / "converter" / "codecs.json").read_text(encoding="utf-8"))
    vendor = json.loads((ROOT / "converter" / "vendor.json").read_text(encoding="utf-8"))
    toolchain = json.loads((ROOT / "toolchain.json").read_text(encoding="utf-8"))

    cc_cache: dict[str, str] = {}
    rf_ver, rf_cc, rf_abi, rf_flags, rf_simd, rf_thr = _rasterfold_provenance(
        target, version_text, cc_cache
    )

    eng_cc_resolved = engine_cc.strip() if engine_cc else ""
    if not eng_cc_resolved and target == "windows-x64":
        llvm_dir = ROOT / ("tools/llvm-mingw/bin" if os.name == "nt" else "tools/llvm-mingw-linux/bin")
        eng_cc_resolved = str(llvm_dir / ("x86_64-w64-mingw32-clang" + (".exe" if os.name == "nt" else "")))
    if not eng_cc_resolved:
        eng_cc_resolved = os.environ.get("CC", "cc")
    eng_cc_id = _probe_compiler_version(eng_cc_resolved, cc_cache)
    eng_flags = engine_flags.strip() if engine_flags else (
        "-std=c11 -O3 -DNDEBUG -march=x86-64-v2 -Wall -Wextra -Werror"
        if target == "windows-x64"
        else "-std=c11 -Wall -Wextra -Werror -pthread"
    )

    pkg_by_id, pkg_by_fmt = _find_pkg_by_format(vendor)
    sys_by_fmt = {}
    for s in vendor.get("system_codecs", []):
        for fmt in s.get("formats", []):
            sys_by_fmt[fmt] = s

    rows: list[str] = []
    for cap in codecs["formats"]:
        fmt = cap["format"]
        enum_name = FORMAT_ENUM_BY_NAME[fmt]
        provider_name = cap["provider"]
        if fmt == "rasterfold":
            ver, cc_id, abi, bflags, simd, thr = (rf_ver, rf_cc, rf_abi, rf_flags, rf_simd, rf_thr)
        elif fmt in ("bmp", "pnm", "pam"):
            ver = "builtin-c11"
            cc_id = eng_cc_id
            abi = target
            bflags = eng_flags
            simd = "scalar-c11 (RF_PORTABLE_SIMD)" if "RF_PORTABLE_SIMD" in eng_flags else "scalar-c11 (x86-64-v2)"
            thr = "single-threaded"
        elif fmt in sys_by_fmt:
            ver = "system-wic"
            cc_id = "Microsoft Windows SDK / WIC"
            abi = "windows-x64"
            bflags = "system COM runtime (CoCreateInstance CLSID_WICImagingFactory; no HEVC bundled)"
            simd = "Windows Imaging Component system codec"
            thr = "os-managed (import-only)"
        elif fmt in pkg_by_fmt:
            ver, cc_id, bflags, simd, thr = _extract_package_build_info(
                pkg_by_fmt[fmt], pkg_by_id, deps_build_dir, toolchain, cc_cache
            )
            abi = vendor.get("target", "windows-x64")
        elif fmt == "gif" and "giflib" in pkg_by_id:
            ver, cc_id, bflags, simd, thr = _extract_package_build_info(
                pkg_by_id["giflib"], pkg_by_id, deps_build_dir, toolchain, cc_cache
            )
            abi = vendor.get("target", "windows-x64")
        else:
            raise ValueError(f"unmapped provenance format: {fmt}")

        rows.append(
            "    {"
            f"{enum_name}, "
            f"{_c_escape(provider_name, 95)}, "
            f"{_c_escape(ver, 95)}, "
            f"{_c_escape(cc_id, 95)}, "
            f"{_c_escape(abi, 95)}, "
            f"{_c_escape(bflags, 255)}, "
            f"{_c_escape(simd, 95)}, "
            f"{_c_escape(thr, 95)}"
            "}"
        )

    header = (
        "/* Auto-generated by scripts/gen_converter_provenance.py; do not edit by hand. */\n"
        "#ifndef RFC_BUILD_PROVENANCE_H\n"
        "#define RFC_BUILD_PROVENANCE_H\n\n"
        "#include \"rfc_codec.h\"\n\n"
        "typedef struct rfc_generated_prov_entry {\n"
        "    rfc_format_id format;\n"
        "    const char *provider_name;\n"
        "    const char *codec_version;\n"
        "    const char *compiler_id;\n"
        "    const char *target_abi;\n"
        "    const char *build_flags;\n"
        "    const char *simd_features;\n"
        "    const char *threading_features;\n"
        "} rfc_generated_prov_entry;\n\n"
        f"static const rfc_generated_prov_entry k_rfc_generated_provenance[{len(rows)}] = {{\n"
        + ",\n".join(rows)
        + "\n};\n\n"
        "#endif /* RFC_BUILD_PROVENANCE_H */\n"
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.is_file() and out_path.read_text(encoding="utf-8") == header:
        return
    out_path.write_text(header, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", required=True, choices=("linux-x64", "windows-x64"))
    ap.add_argument("--engine-cc", default="")
    ap.add_argument("--engine-flags", default="")
    ap.add_argument("--deps-build", default="")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    deps_dir = Path(a.deps_build) if a.deps_build else None
    generate(a.target, a.engine_cc, a.engine_flags, deps_dir, Path(a.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
