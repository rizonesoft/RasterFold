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


def _is_truthy(val: str | None) -> bool:
    if val is None:
        return False
    return val.strip().upper() in ("1", "ON", "TRUE", "YES")


def _is_falsy(val: str | None) -> bool:
    if val is None:
        return False
    return val.strip().upper() in ("0", "OFF", "FALSE", "NO")


def _parse_ninja_compile_info(ninja_path: Path, is_cxx: bool) -> tuple[list[str], list[str]]:
    if not ninja_path.is_file():
        return ([], [])
    preferred_prefix = "CXX_COMPILER__" if is_cxx else "C_COMPILER__"
    any_prefix = ("CXX_COMPILER__", "C_COMPILER__")

    def _collect(prefix_tuple: tuple[str, ...]) -> tuple[list[str], list[str]]:
        flags_tokens: list[str] = []
        define_tokens: list[str] = []
        in_block = False
        for raw_line in ninja_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not raw_line.startswith(" ") and not raw_line.startswith("\t"):
                in_block = False
                if raw_line.startswith("build ") and ":" in raw_line:
                    after_colon = raw_line.split(":", 1)[1].lstrip()
                    rule = after_colon.split(None, 1)[0] if after_colon else ""
                    if any(rule.startswith(p) for p in prefix_tuple):
                        in_block = True
                continue
            if not in_block:
                continue
            stripped = raw_line.strip()
            if stripped.startswith("FLAGS ="):
                raw_flags = stripped[len("FLAGS ="):].strip()
                flags_tokens.extend(shlex.split(raw_flags, posix=True))
            elif stripped.startswith("DEFINES ="):
                raw_defs = stripped[len("DEFINES ="):].strip()
                define_tokens.extend(shlex.split(raw_defs, posix=True))
        return (flags_tokens, define_tokens)

    ft, dt = _collect((preferred_prefix,))
    if not ft and not dt:
        ft, dt = _collect(any_prefix)
    return (ft, dt)


def _semantic_ninja_tokens(flags_tokens: list[str], define_tokens: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    skip_next = False
    for tok in flags_tokens:
        if skip_next:
            skip_next = False
            continue
        if tok in ("-isystem", "-I", "-o", "-MF", "-MT", "-x"):
            skip_next = True
            continue
        if (
            tok.startswith("-I")
            or tok.startswith("-W")
            or tok.startswith("-fmacro-prefix-map=")
            or tok.startswith("-ffile-prefix-map=")
            or tok.startswith("-fdiagnostics")
            or tok.startswith("-fcolor-diagnostics")
            or tok.startswith("-fvisibility")
            or tok.startswith("-D__DATE__")
            or tok.startswith("-D__TIME__")
            or tok.startswith("-D__TIMESTAMP__")
        ):
            continue
        if (
            tok.startswith("-march=")
            or tok.startswith("-mtune=")
            or tok.startswith("-m")
            or tok.startswith("-O")
            or tok.startswith("-g")
            or tok.startswith("-std=")
            or tok.startswith("-fno-")
            or tok.startswith("-flto")
            or tok.startswith("-fopenmp")
            or tok in ("-pthread", "-fexceptions", "-frtti", "-fstrict-aliasing")
            or (
                tok.startswith("-D")
                and not tok.startswith("-D_CRT_")
                and tok not in ("-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX")
            )
        ):
            if tok not in seen:
                seen.add(tok)
                out.append(tok)

    for tok in define_tokens:
        if (
            tok in ("-DUNICODE", "-D_UNICODE", "-DHAVE_CONFIG_H", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE", "-D_LARGEFILE64_SOURCE=1")
            or tok.startswith("-D_CRT_")
            or tok.startswith("-D__DATE__")
            or tok.startswith("-D__TIME__")
            or tok.startswith("-D__TIMESTAMP__")
            or tok.startswith("-D__STDC_")
        ):
            continue
        if tok.startswith("-D") and tok not in seen:
            seen.add(tok)
            out.append(tok)
    return out


def _effective_march(tokens: list[str]) -> str:
    march = "x86-64-v2"
    for tok in tokens:
        if tok.startswith("-march="):
            march = tok.split("=", 1)[1]
    return march


def _derive_simd_summary(
    pid: str,
    feats: dict,
    nasm_ver: str,
    cache: dict[str, str],
    all_flag_tokens: list[str],
    define_tokens: list[str],
    aom_cache: dict[str, str],
    aom_tokens: list[str],
) -> str:
    march = _effective_march(all_flag_tokens)
    no_vec = any(t in ("-fno-vectorize", "-fno-slp-vectorize", "-mno-sse2") for t in all_flag_tokens)
    nasm_note = f"NASM {nasm_ver}" if nasm_ver else "NASM"
    has_nasm_compiler = bool(cache.get("CMAKE_ASM_NASM_COMPILER")) or bool(feats.get("nasm"))

    if no_vec:
        return f"vectorize-disabled ({march})"

    if pid == "libjxl":
        disabled_def = next((d for d in define_tokens + all_flag_tokens if "HWY_DISABLED_TARGETS=" in d), "")
        hwy_avx2 = cache.get("JPEGXL_ENABLE_HWY_AVX2")
        hwy_sse4 = cache.get("JPEGXL_ENABLE_HWY_SSE4")
        hwy_avx3 = cache.get("JPEGXL_ENABLE_HWY_AVX3")
        hwy_avx3_dl = cache.get("JPEGXL_ENABLE_HWY_AVX3_DL")
        if _is_truthy(cache.get("HWY_COMPILE_ONLY_SCALAR")) or "HWY_SCALAR" in disabled_def and _is_falsy(hwy_avx2):
            return f"scalar-only ({march})"
        targets: list[str] = []
        if not _is_falsy(cache.get("JPEGXL_ENABLE_HWY_SSE2")):
            targets.append("SSE2")
        if not _is_falsy(hwy_sse4):
            targets.append("SSE4")
        if _is_truthy(hwy_avx2) and "HWY_AVX2" not in disabled_def:
            targets.append("AVX2")
        disabled_set = set(disabled_def.replace("=", "|").replace("(", "|").replace(")", "|").split("|"))
        if _is_truthy(hwy_avx3) and "HWY_AVX3" not in disabled_set:
            targets.append("AVX3")
        elif _is_truthy(hwy_avx3_dl) and "HWY_AVX3_DL" not in disabled_set:
            targets.append("AVX3_DL")
        if targets:
            return f"Highway dynamic ({march}; {','.join(targets)})"
        return f"Highway ({march}; scalar-fallback)"

    if pid == "highway":
        if _is_truthy(cache.get("HWY_COMPILE_ONLY_SCALAR")):
            return f"scalar-only (HWY_COMPILE_ONLY_SCALAR=ON; {march})"
        return f"Highway dynamic dispatch ({march})"

    if pid == "libjpeg-turbo":
        if _is_falsy(cache.get("WITH_SIMD")):
            return f"disabled (WITH_SIMD=OFF; {march})"
        req = "REQUIRE_SIMD=ON" if _is_truthy(cache.get("REQUIRE_SIMD")) or "WITH_SIMD" not in cache else "WITH_SIMD=ON"
        return f"{nasm_note} x86-64 SIMD ({req}; {march})" if has_nasm_compiler else f"SIMD ({req}; {march})"

    if pid in ("libaom", "libavif"):
        c_src = aom_cache if (pid == "libavif" and aom_cache) else cache
        t_src = aom_tokens if (pid == "libavif" and aom_tokens) else all_flag_tokens
        aom_cpu = c_src.get("AOM_TARGET_CPU", "").strip()
        if aom_cpu == "generic":
            return f"scalar generic (AOM_TARGET_CPU=generic; {march})"
        aom_march = _effective_march(t_src) if t_src else march
        return f"{nasm_note} x86_64 SIMD ({aom_cpu or 'x86_64'}; {aom_march})"

    if pid == "libpng":
        if _is_falsy(cache.get("PNG_HARDWARE_OPTIMIZATIONS")):
            return f"disabled (PNG_HARDWARE_OPTIMIZATIONS=OFF; {march})"
        if any("PNG_INTEL_SSE_OPT=0" in d for d in define_tokens):
            return f"disabled (PNG_INTEL_SSE_OPT=0; {march})"
        return f"SSE2 hardware opts (PNG_HARDWARE_OPTIMIZATIONS=ON; {march})"

    if pid == "libwebp":
        if _is_falsy(cache.get("WEBP_ENABLE_SIMD")):
            return f"disabled (WEBP_ENABLE_SIMD=OFF; {march})"
        avx2 = "+AVX2" if cache.get("WEBP_HAVE_FLAG_AVX2") == "1" else ""
        return f"SSE2{avx2} runtime (WEBP_ENABLE_SIMD=ON; {march})"

    if pid == "openjph":
        if _is_truthy(cache.get("OJPH_DISABLE_SIMD")):
            return f"disabled (OJPH_DISABLE_SIMD=ON; {march})"
        dis_avx2 = _is_truthy(cache.get("OJPH_DISABLE_AVX2"))
        return f"OpenJPH runtime ({'SSE' if dis_avx2 else 'SSE/AVX2'}; {march})"

    if feats.get("simd"):
        base = str(feats["simd"])
        if march != "x86-64-v2" and march not in base:
            return f"{base} ({march})"
        return base if not (feats.get("nasm") and nasm_ver and nasm_ver not in base) else f"{base} ({nasm_note})"
    return march


def _derive_threading_summary(
    pid: str,
    feats: dict,
    cache: dict[str, str],
    all_flag_tokens: list[str],
    define_tokens: list[str],
) -> str:
    has_openmp = (
        any(t.startswith("-fopenmp") for t in all_flag_tokens)
        or _is_truthy(cache.get("LIBRAW_USE_OPENMP"))
        or _is_truthy(cache.get("ENABLE_OPENMP"))
    )
    if pid == "libraw":
        if has_openmp:
            return "OpenMP enabled (libraw)"
        return "serial-deterministic (OpenMP disabled)"

    if pid == "libwebp":
        if _is_falsy(cache.get("WEBP_USE_THREAD")):
            return "single-threaded (WEBP_USE_THREAD=OFF)"
        return "libwebp threads (WEBP_USE_THREAD=ON)"

    if pid == "openjpeg":
        if _is_falsy(cache.get("OPJ_USE_THREAD")):
            return "single-threaded (OPJ_USE_THREAD=OFF)"
        return "openjpeg threads (OPJ_USE_THREAD=ON)"

    if pid == "libjxl":
        if _is_falsy(cache.get("JPEGXL_ENABLE_THREADS")):
            return "single-threaded (JPEGXL_ENABLE_THREADS=OFF)"
        return "libjxl worker threads (budgeted)"

    if pid == "openexr":
        if _is_falsy(cache.get("OPENEXR_ENABLE_THREADING")):
            return "single-threaded (OPENEXR_ENABLE_THREADING=OFF)"
        return "IlmThread pool (budgeted)"

    if pid == "libavif":
        if _is_falsy(cache.get("ENABLE_MULTITHREAD")) or ("THREADS_HAVE_PTHREAD_ARG" not in cache and "-pthread" not in all_flag_tokens and cache):
            if "-pthread" not in all_flag_tokens and not cache.get("CMAKE_HAVE_PTHREAD_CREATE"):
                return "single-threaded (pthreads unavailable)"
        return "libavif/aom threads (budgeted)"

    if pid == "libaom":
        if cache.get("CONFIG_MULTITHREAD") == "0":
            return "single-threaded (CONFIG_MULTITHREAD=0)"
        return "libaom row-mt threads (budgeted)"

    if feats.get("threads"):
        return f"{pid} threads (budgeted)"
    if any("MUTEX_" in d for d in define_tokens) or has_openmp:
        return f"{pid} threads (enabled)"
    return "single-threaded"


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
    ninja_flags: list[str] = []
    ninja_defs: list[str] = []
    aom_cache: dict[str, str] = {}
    aom_flags: list[str] = []

    lang = str(pkg.get("language", "c")).lower()
    is_cxx = ("cxx" in lang or "c++" in lang)

    if deps_build_dir is not None:
        cache = _parse_cmake_cache(deps_build_dir / pid / "CMakeCache.txt")
        ninja_flags, ninja_defs = _parse_ninja_compile_info(deps_build_dir / pid / "build.ninja", is_cxx)
        if pid == "libavif":
            aom_cache = _parse_cmake_cache(deps_build_dir / "libaom" / "CMakeCache.txt")
            aom_flags, _ = _parse_ninja_compile_info(deps_build_dir / "libaom" / "build.ninja", False)

    if is_cxx:
        cc_path = cache.get("CMAKE_CXX_COMPILER") or default_cxx
        btype = (cache.get("CMAKE_BUILD_TYPE") or "Release").strip()
        init_flags = cache["CMAKE_CXX_FLAGS"] if "CMAKE_CXX_FLAGS" in cache else "-march=x86-64-v2"
        cfg_key = f"CMAKE_CXX_FLAGS_{btype.upper()}"
        c_flags = cache[cfg_key] if cfg_key in cache else "-O3 -DNDEBUG"
    else:
        cc_path = cache.get("CMAKE_C_COMPILER") or default_cc
        btype = (cache.get("CMAKE_BUILD_TYPE") or "Release").strip()
        init_flags = cache["CMAKE_C_FLAGS"] if "CMAKE_C_FLAGS" in cache else "-march=x86-64-v2"
        cfg_key = f"CMAKE_C_FLAGS_{btype.upper()}"
        c_flags = cache[cfg_key] if cfg_key in cache else "-O3 -DNDEBUG"
    compiler_id = _probe_compiler_version(cc_path, cc_cache)

    raw_cache_tokens = shlex.split(f"{init_flags} {c_flags}", posix=True)
    cache_flag_tokens = _semantic_ninja_tokens(raw_cache_tokens, [])
    cache_non_def = [t for t in cache_flag_tokens if not t.startswith("-D") or t == "-DNDEBUG"]
    cache_def = [t for t in cache_flag_tokens if t.startswith("-D") and t != "-DNDEBUG"]

    ninja_all = _semantic_ninja_tokens(ninja_flags, ninja_defs)
    ninja_flag_only = [t for t in ninja_all if not t.startswith("-D") or t == "-DNDEBUG"]
    ninja_def_only = [t for t in ninja_all if t.startswith("-D") and t != "-DNDEBUG"]
    combined_flag_tokens = cache_flag_tokens + ninja_all

    cache_toggle_keys = (
        "WEBP_ENABLE_SIMD",
        "WEBP_USE_THREAD",
        "WITH_SIMD",
        "REQUIRE_SIMD",
        "PNG_HARDWARE_OPTIMIZATIONS",
        "OJPH_DISABLE_SIMD",
        "OJPH_DISABLE_AVX2",
        "OPJ_USE_THREAD",
        "AOM_TARGET_CPU",
        "OPENEXR_ENABLE_THREADING",
        "JPEGXL_ENABLE_THREADS",
        "JPEGXL_ENABLE_HWY_AVX2",
        "JPEGXL_ENABLE_HWY_AVX3",
        "JPEGXL_ENABLE_HWY_AVX3_DL",
        "HWY_COMPILE_ONLY_SCALAR",
        "LIBRAW_USE_OPENMP",
    )
    toggles = [f"-D{k}={cache[k]}" for k in cache_toggle_keys if k in cache]
    opts = [
        opt
        for opt in pkg.get("cmake_options", [])
        if not opt.endswith("INSTALL=ON")
        and not any(opt.startswith(f"-D{k}=") for k in cache_toggle_keys)
    ]

    ordered_tokens: list[str] = []
    seen_tokens: set[str] = set()
    for tok in (
        [f"-DCMAKE_BUILD_TYPE={btype}"]
        + cache_non_def
        + ninja_flag_only
        + toggles
        + cache_def
        + ninja_def_only
        + opts[:3]
    ):
        if tok and tok not in seen_tokens:
            seen_tokens.add(tok)
            ordered_tokens.append(tok)

    bounded_parts: list[str] = []
    used_bytes = 0
    for tok in ordered_tokens:
        need = len(tok.encode("utf-8")) + (1 if bounded_parts else 0)
        if used_bytes + need <= 255:
            bounded_parts.append(tok)
            used_bytes += need
    build_flags = " ".join(bounded_parts) if bounded_parts else f"-DCMAKE_BUILD_TYPE={btype}"

    feats = pkg.get("features", {})
    simd = _derive_simd_summary(pid, feats, nasm_ver, cache, combined_flag_tokens, ninja_defs, aom_cache, aom_flags)
    thr = _derive_threading_summary(pid, feats, cache, combined_flag_tokens, ninja_defs)
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
