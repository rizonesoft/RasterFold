#!/usr/bin/env python3
"""Generate build-captured provider provenance C header for converter reports."""
from __future__ import annotations

import argparse
import hashlib
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
                flags = _format_bounded_build_flags(kept, "", 255)

    if not compiler_id:
        llvm_dir = ROOT / ("tools/llvm-mingw/bin" if os.name == "nt" else "tools/llvm-mingw-linux/bin")
        clang_bin = llvm_dir / ("clang.exe" if os.name == "nt" else "clang")
        compiler_id = _probe_compiler_version(str(clang_bin) if clang_bin.is_file() else "clang", cc_cache)
    if not flags:
        triple = "x86_64-w64-mingw32" if target == "windows-x64" else "x86_64-linux-gnu"
        extra = "-ffreestanding -fno-stack-protector" if target == "windows-x64" else "-fPIC -pthread"
        pflag = " -DRF_PORTABLE_SIMD" if portable else ""
        flags = _format_bounded_build_flags(
            shlex.split(
                f"--target={triple} -std=c11 -O2 -g -Wall -Wextra -Werror -march=x86-64-v2 -mtune=generic{pflag} {extra}"
            ),
            "",
            255,
        )

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


PACKAGE_SUBDEPS: dict[str, tuple[str, ...]] = {
    "libpng": ("zlib",),
    "libjxl": ("highway", "brotli", "lcms2"),
    "libavif": ("libaom",),
    "libtiff": ("zlib", "libdeflate", "libwebp", "libjpeg-turbo"),
    "openexr": ("imath", "openjph", "libdeflate"),
    "libraw": ("zlib", "lcms2", "libjpeg-turbo"),
}


def _normalize_search_path(raw: str, deps_root: Path | None) -> str:
    p = Path(raw)
    if not p.is_absolute():
        return raw.replace("\\", "/")
    if p.is_relative_to(ROOT):
        return "<repo>/" + p.relative_to(ROOT).as_posix()
    if deps_root is not None and p.is_relative_to(deps_root):
        return "<deps>/" + p.relative_to(deps_root).as_posix()
    digest = hashlib.sha256(raw.encode("utf-8", errors="replace")).hexdigest()[:16]
    return f"{p.name or 'path'}#p={digest}"


def _extract_semantic_cache_options(
    path: Path,
    is_cxx: bool,
    has_c_blk: bool,
    has_cxx_blk: bool,
) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    skip_prefixes = (
        "CPACK_",
        "CTEST_",
        "FETCHCONTENT_",
        "FIND_PACKAGE_",
        "pkgcfg_",
    )
    skip_suffixes = (
        "_DIR",
        "_DIRS",
        "_LIBRARY",
        "_LIBRARIES",
        "_INCLUDE_DIR",
        "_INCLUDE_DIRS",
        "_BINARY_DIR",
        "_SOURCE_DIR",
        "_FILEPATH",
        "_PATH",
        "_EXECUTABLE",
        "_COMMAND",
    )
    want_cxx_std = is_cxx or has_cxx_blk
    want_c_std = (not is_cxx) or has_c_blk
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#") or line.startswith("//") or ":" not in line or "=" not in line:
            continue
        left, val = line.split("=", 1)
        key, _, ktype = left.partition(":")
        key = key.strip()
        ktype = ktype.strip().upper()
        val = val.strip()
        if ktype not in ("BOOL", "STRING", "UNINITIALIZED"):
            continue
        if key.startswith("CMAKE_"):
            allowed_cmake = (
                key.startswith("CMAKE_INTERPROCEDURAL_OPTIMIZATION")
                or key in ("CMAKE_POSITION_INDEPENDENT_CODE", "CMAKE_MSVC_RUNTIME_LIBRARY")
                or (want_cxx_std and (key.startswith("CMAKE_CXX_STANDARD") or key == "CMAKE_CXX_EXTENSIONS"))
                or (want_c_std and (key.startswith("CMAKE_C_STANDARD") or key == "CMAKE_C_EXTENSIONS"))
            )
            if not allowed_cmake:
                continue
        if key.startswith(skip_prefixes) or key.endswith(skip_suffixes):
            continue
        if "/" in val or "\\" in val:
            digest = hashlib.sha256(val.encode("utf-8", errors="replace")).hexdigest()[:16]
            val = f"{Path(val).name or 'path'}#p={digest}"
        out[key] = val
    return out


def _is_truthy(val: str | None) -> bool:
    if val is None:
        return False
    return val.strip().upper() in ("1", "ON", "TRUE", "YES")


def _is_falsy(val: str | None) -> bool:
    if val is None:
        return False
    return val.strip().upper() in ("0", "OFF", "FALSE", "NO")


def _is_timestamp_define(tok: str) -> bool:
    return (
        tok.startswith("-D__DATE__")
        or tok.startswith("-D__TIME__")
        or tok.startswith("-D__TIMESTAMP__")
    )


def _is_noise_define(tok: str) -> bool:
    return (
        tok in (
            "-DUNICODE",
            "-D_UNICODE",
            "-DHAVE_CONFIG_H",
            "-D_FILE_OFFSET_BITS=64",
            "-D_LARGEFILE_SOURCE",
            "-D_LARGEFILE64_SOURCE=1",
            "-DWIN32_LEAN_AND_MEAN",
            "-DNOMINMAX",
        )
        or tok.startswith("-D_CRT_")
        or tok.startswith("-D__STDC_")
        or _is_timestamp_define(tok)
    )


def _resolve_input_path(arg: str, search_dirs: tuple[Path, ...]) -> Path:
    p = Path(arg)
    if p.is_absolute():
        return p
    for base in search_dirs:
        cand = base / p
        if cand.is_file():
            return cand
    return p


def _extract_token_search_dirs(
    flags_tokens: list[str], compile_dir: Path | None
) -> tuple[Path, ...]:
    dirs: list[Path] = []
    prefixes = ("-iquote", "-isystem", "-I", "-idirafter")
    i = 0
    while i < len(flags_tokens):
        tok = flags_tokens[i]
        raw_dir = ""
        if tok in prefixes and i + 1 < len(flags_tokens):
            raw_dir = flags_tokens[i + 1]
            i += 2
        else:
            for pfx in prefixes:
                if tok.startswith(pfx) and len(tok) > len(pfx):
                    raw_dir = tok[len(pfx):]
                    break
            i += 1
        if raw_dir:
            dp = Path(raw_dir)
            if not dp.is_absolute() and compile_dir is not None:
                dp = compile_dir / dp
            if dp not in dirs:
                dirs.append(dp)
    return tuple(dirs)


def _sanitize_forced_include_token(
    opt: str,
    arg: str,
    include_fingerprint: bool,
    search_dirs: tuple[Path, ...] = (),
) -> str:
    p = Path(arg)
    name = p.name or arg
    sep = "" if opt == "@" else ":"
    if not include_fingerprint:
        return f"{opt}{sep}{name}"
    arg_sha = hashlib.sha256(arg.encode("utf-8", errors="replace")).hexdigest()[:16]
    resolved = _resolve_input_path(arg, search_dirs)
    content_sha = (
        hashlib.sha256(resolved.read_bytes()).hexdigest()[:16]
        if resolved.is_file()
        else "absent"
    )
    return f"{opt}{sep}{name}#p={arg_sha}#c={content_sha}"


def _semantic_ninja_tokens(
    flags_tokens: list[str],
    define_tokens: list[str],
    include_fingerprint: bool = False,
    deps_root: Path | None = None,
    search_dirs: tuple[Path, ...] = (),
) -> list[str]:
    out: list[str] = []
    compile_dir = search_dirs[0] if search_dirs else None
    inc_dirs = _extract_token_search_dirs(flags_tokens, compile_dir)
    all_search: list[Path] = []
    for d in list(search_dirs) + list(inc_dirs):
        if d not in all_search:
            all_search.append(d)
    eff_search = tuple(all_search)
    i = 0
    two_tok_search = (
        "-isystem",
        "-I",
        "-iquote",
        "-idirafter",
        "-isysroot",
        "--sysroot",
        "-sysroot",
        "-L",
        "-B",
        "--gcc-toolchain",
    )
    glued_search = (
        "--sysroot=",
        "-isysroot",
        "-isystem",
        "-iquote",
        "-idirafter",
        "-I",
        "-L",
        "-B",
    )
    two_tok_io = ("-o", "-MF", "-MT", "-MQ")
    while i < len(flags_tokens):
        tok = flags_tokens[i]
        if tok in two_tok_io:
            i += 2
            continue
        if tok in two_tok_search and i + 1 < len(flags_tokens):
            if include_fingerprint:
                out.append(f"{tok}:{_normalize_search_path(flags_tokens[i + 1], deps_root)}")
            i += 2
            continue
        if tok in ("-include", "--include", "-imacros", "-include-pch") and i + 1 < len(flags_tokens):
            opt_norm = "-include" if tok == "--include" else tok
            out.append(_sanitize_forced_include_token(opt_norm, flags_tokens[i + 1], include_fingerprint, eff_search))
            i += 2
            continue
        if tok in ("-Xclang", "-mllvm") and i + 1 < len(flags_tokens):
            nxt = flags_tokens[i + 1]
            if (
                tok == "-Xclang"
                and nxt in ("-include", "-imacros", "-include-pch")
                and i + 3 < len(flags_tokens)
                and flags_tokens[i + 2] == "-Xclang"
            ):
                out.append(f"-Xclang {_sanitize_forced_include_token(nxt, flags_tokens[i + 3], include_fingerprint, eff_search)}")
                i += 4
                continue
            if "/" in nxt or "\\" in nxt:
                out.append(_sanitize_forced_include_token(tok, nxt, include_fingerprint, eff_search))
            else:
                out.append(f"{tok} {nxt}")
            i += 2
            continue
        if tok in ("-x", "--param", "-target", "-D", "-U") and i + 1 < len(flags_tokens):
            nxt = flags_tokens[i + 1]
            if tok in ("-D", "-U"):
                if not nxt.startswith("-"):
                    paired = f"{tok}{nxt}"
                    skip_def = _is_timestamp_define(paired) if include_fingerprint else _is_noise_define(paired)
                    if not skip_def:
                        out.append(paired)
                    i += 2
                    continue
            else:
                out.append(f"{tok} {nxt}")
                i += 2
                continue
        i += 1
        if not tok or tok in ("-c", "-pipe", "-MD", "-MMD", "-MP", "-MG", "-MV", "-w") or tok.startswith("-W"):
            continue
        if tok.startswith("@") and len(tok) > 1:
            rsp_search = (compile_dir,) if compile_dir is not None else eff_search
            out.append(_sanitize_forced_include_token("@", tok[1:], include_fingerprint, rsp_search))
            continue
        if (
            tok.startswith("-fmacro-prefix-map=")
            or tok.startswith("-ffile-prefix-map=")
            or tok.startswith("-fdebug-prefix-map=")
            or tok.startswith("-fcoverage-prefix-map=")
            or tok.startswith("-fdiagnostics")
            or tok.startswith("-fcolor-diagnostics")
        ):
            continue
        if not include_fingerprint and (
            tok in ("-fPIC", "-fpic", "-fPIE", "-fpie") or tok.startswith("-fvisibility")
        ):
            continue
        if _is_timestamp_define(tok) or (not include_fingerprint and _is_noise_define(tok)):
            continue
        matched_glued_search = False
        for gp in glued_search:
            if tok.startswith(gp) and len(tok) > len(gp):
                if include_fingerprint:
                    out.append(f"{gp}:{_normalize_search_path(tok[len(gp):], deps_root)}")
                matched_glued_search = True
                break
        if matched_glued_search:
            continue
        if tok.startswith("--include="):
            out.append(_sanitize_forced_include_token("-include", tok[len("--include="):], include_fingerprint, eff_search))
            continue
        if tok.startswith("-include-pch") and len(tok) > len("-include-pch"):
            out.append(_sanitize_forced_include_token("-include-pch", tok[len("-include-pch"):], include_fingerprint, eff_search))
            continue
        if tok.startswith("-include") and len(tok) > len("-include"):
            out.append(_sanitize_forced_include_token("-include", tok[len("-include"):], include_fingerprint, eff_search))
            continue
        if tok.startswith("-imacros") and len(tok) > len("-imacros"):
            out.append(_sanitize_forced_include_token("-imacros", tok[len("-imacros"):], include_fingerprint, eff_search))
            continue
        if tok.startswith("-"):
            out.append(tok)

    for tok in define_tokens:
        if _is_timestamp_define(tok) or (not include_fingerprint and _is_noise_define(tok)):
            continue
        if tok.startswith("-D") or tok.startswith("-U"):
            out.append(tok)
    return out


def _parse_ninja_compile_blocks(
    ninja_path: Path,
    is_cxx: bool,
    deps_root: Path | None = None,
    extra_search_dirs: tuple[Path, ...] = (),
) -> dict[str, object]:
    if not ninja_path.is_file():
        return {"blocks": [], "has_cxx": False, "has_c": False, "has_asm": False}
    blocks: list[tuple[str, tuple[str, ...], tuple[str, ...], tuple[str, ...], tuple[str, ...]]] = []
    seen_blocks: set[tuple[str, tuple[str, ...], tuple[str, ...]]] = set()
    has_cxx = False
    has_c = False
    has_asm = False
    cur_lang = ""
    cur_flags: list[str] = []
    cur_defs: list[str] = []
    cur_includes: list[str] = []
    search_dirs_list: list[Path] = [ninja_path.parent]
    for sd in extra_search_dirs:
        if sd not in search_dirs_list:
            search_dirs_list.append(sd)
    base_search_dirs = tuple(search_dirs_list)

    def flush_block() -> None:
        if not cur_lang:
            return
        fp_f = tuple(
            _semantic_ninja_tokens(
                cur_flags + cur_includes,
                [],
                include_fingerprint=True,
                deps_root=deps_root,
                search_dirs=base_search_dirs,
            )
        )
        fp_d = tuple(
            _semantic_ninja_tokens(
                [], cur_defs, include_fingerprint=True, deps_root=deps_root, search_dirs=base_search_dirs
            )
        )
        uniq_key = (cur_lang, fp_f, fp_d)
        if uniq_key not in seen_blocks:
            seen_blocks.add(uniq_key)
            disp_f = tuple(
                _semantic_ninja_tokens(
                    cur_flags + cur_includes,
                    [],
                    include_fingerprint=False,
                    deps_root=deps_root,
                    search_dirs=base_search_dirs,
                )
            )
            disp_d = tuple(
                _semantic_ninja_tokens(
                    [], cur_defs, include_fingerprint=False, deps_root=deps_root, search_dirs=base_search_dirs
                )
            )
            blocks.append((cur_lang, disp_f, disp_d, fp_f, fp_d))

    for raw_line in ninja_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw_line.startswith(" ") and not raw_line.startswith("\t"):
            flush_block()
            cur_lang = ""
            cur_flags = []
            cur_defs = []
            cur_includes = []
            if raw_line.startswith("build ") and ":" in raw_line:
                after_colon = raw_line.split(":", 1)[1].lstrip()
                rule = after_colon.split(None, 1)[0] if after_colon else ""
                if rule.startswith("CXX_COMPILER__"):
                    cur_lang = "cxx"
                    has_cxx = True
                elif rule.startswith("C_COMPILER__"):
                    cur_lang = "c"
                    has_c = True
                elif (
                    rule.startswith("ASM_NASM_COMPILER__")
                    or rule.startswith("ASM_COMPILER__")
                    or rule.startswith("ASM_MASM_COMPILER__")
                ):
                    cur_lang = "asm"
                    has_asm = True
            continue
        if not cur_lang:
            continue
        stripped = raw_line.strip()
        if stripped.startswith("FLAGS ="):
            cur_flags = shlex.split(stripped[len("FLAGS ="):].strip(), posix=True)
        elif stripped.startswith("DEFINES ="):
            cur_defs = shlex.split(stripped[len("DEFINES ="):].strip(), posix=True)
        elif stripped.startswith("INCLUDES ="):
            cur_includes = shlex.split(stripped[len("INCLUDES ="):].strip(), posix=True)
    flush_block()

    want_langs: set[str] = set()
    if is_cxx:
        if has_cxx:
            want_langs.add("cxx")
        if has_c:
            want_langs.add("c")
    else:
        if has_c:
            want_langs.add("c")
        if has_cxx:
            want_langs.add("cxx")
    if has_asm:
        want_langs.add("asm")
    selected = [b for b in blocks if b[0] in want_langs] if want_langs else []
    return {"blocks": selected, "has_cxx": has_cxx, "has_c": has_c, "has_asm": has_asm}


def _format_bounded_build_flags(
    tokens: list[str],
    fallback: str,
    max_bytes: int = 255,
    fingerprint_payload: object | None = None,
) -> str:
    clean = [t for t in tokens if t]
    if fingerprint_payload is None:
        if not clean:
            return fallback
        full = " ".join(clean)
        if len(full.encode("utf-8")) <= max_bytes:
            return full
        digest = hashlib.sha256(full.encode("utf-8")).hexdigest()[:12]
    else:
        canon = json.dumps(fingerprint_payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        digest = hashlib.sha256(canon.encode("utf-8")).hexdigest()[:12]
        if not clean:
            return f"#[{digest}]"[:max_bytes]
        full_with_fp = f"{' '.join(clean)} #[{digest}]"
        if len(full_with_fp.encode("utf-8")) <= max_bytes:
            return full_with_fp

    count_guess = len(clean)
    for _ in range(3):
        suffix = f"+{count_guess}#[{digest}]"
        suffix_bytes = len(suffix.encode("utf-8"))
        budget = max_bytes - suffix_bytes - 1
        kept: list[str] = []
        used = 0
        if budget > 0:
            for tok in clean:
                need = len(tok.encode("utf-8")) + (1 if kept else 0)
                if used + need <= budget:
                    kept.append(tok)
                    used += need
                else:
                    break
        new_omitted = len(clean) - len(kept)
        if new_omitted == count_guess:
            return f"{' '.join(kept)} {suffix}" if kept else suffix[:max_bytes]
        count_guess = new_omitted
    suffix = f"+{count_guess}#[{digest}]"
    budget = max_bytes - len(suffix.encode("utf-8")) - 1
    kept_final: list[str] = []
    used_final = 0
    if budget > 0:
        for tok in clean:
            need = len(tok.encode("utf-8")) + (1 if kept_final else 0)
            if used_final + need <= budget:
                kept_final.append(tok)
                used_final += need
            else:
                break
    return f"{' '.join(kept_final)} {suffix}" if kept_final else suffix[:max_bytes]


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
    no_vec = False
    for t in all_flag_tokens:
        if t in ("-fno-vectorize", "-fno-slp-vectorize", "-mno-sse2"):
            no_vec = True
        elif t in ("-fvectorize", "-fslp-vectorize", "-msse2", "-msse4", "-mavx", "-mavx2"):
            no_vec = False
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
    flag_openmp = False
    saw_openmp_flag = False
    for t in all_flag_tokens:
        if t == "-fno-openmp":
            flag_openmp = False
            saw_openmp_flag = True
        elif t.startswith("-fopenmp"):
            flag_openmp = True
            saw_openmp_flag = True
    has_openmp = flag_openmp if saw_openmp_flag else (
        _is_truthy(cache.get("LIBRAW_USE_OPENMP"))
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


def _collect_pkg_fingerprint_state(
    pkg: dict,
    pkg_by_id: dict[str, dict],
    deps_build_dir: Path | None,
    cc_cache: dict[str, str],
    seen_ids: set[str] | None = None,
) -> dict[str, object]:
    pid = pkg["id"]
    llvm_dir = ROOT / ("tools/llvm-mingw/bin" if os.name == "nt" else "tools/llvm-mingw-linux/bin")
    default_cc = str(llvm_dir / ("x86_64-w64-mingw32-clang" + (".exe" if os.name == "nt" else "")))
    default_cxx = str(llvm_dir / ("x86_64-w64-mingw32-clang++" + (".exe" if os.name == "nt" else "")))

    lang = str(pkg.get("language", "c")).lower()
    is_cxx = ("cxx" in lang or "c++" in lang)

    cache: dict[str, str] = {}
    sem_cache: dict[str, str] = {}
    ninja_info: dict[str, object] = {"blocks": [], "has_cxx": False, "has_c": False, "has_asm": False}
    deps_root = (
        deps_build_dir.parent
        if (deps_build_dir is not None and deps_build_dir.name == "deps-build")
        else deps_build_dir
    )
    extra_search: list[Path] = []
    if deps_build_dir is not None:
        pkg_build_dir = deps_build_dir / pid
        extra_search.append(pkg_build_dir)
        cache_file = pkg_build_dir / "CMakeCache.txt"
        cache = _parse_cmake_cache(cache_file)
        for src_key in ("CMAKE_HOME_DIRECTORY", f"{pid}_SOURCE_DIR"):
            sval = cache.get(src_key, "").strip()
            if sval and Path(sval) not in extra_search:
                extra_search.append(Path(sval))
        ninja_info = _parse_ninja_compile_blocks(
            pkg_build_dir / "build.ninja",
            is_cxx,
            deps_root=deps_root,
            extra_search_dirs=tuple(extra_search),
        )
        sem_cache = _extract_semantic_cache_options(
            cache_file,
            is_cxx,
            bool(ninja_info["has_c"]),
            bool(ninja_info["has_cxx"]),
        )
    base_search_dirs = tuple(extra_search)

    btype = (cache.get("CMAKE_BUILD_TYPE") or "Release").strip()
    cc_path = (
        (cache.get("CMAKE_CXX_COMPILER") or default_cxx)
        if is_cxx
        else (cache.get("CMAKE_C_COMPILER") or default_cc)
    )
    compiler_id = _probe_compiler_version(cc_path, cc_cache)

    cxx_init = cache["CMAKE_CXX_FLAGS"] if "CMAKE_CXX_FLAGS" in cache else "-march=x86-64-v2"
    cxx_cfg = cache[f"CMAKE_CXX_FLAGS_{btype.upper()}"] if f"CMAKE_CXX_FLAGS_{btype.upper()}" in cache else "-O3 -DNDEBUG"
    c_init = cache["CMAKE_C_FLAGS"] if "CMAKE_C_FLAGS" in cache else "-march=x86-64-v2"
    c_cfg = cache[f"CMAKE_C_FLAGS_{btype.upper()}"] if f"CMAKE_C_FLAGS_{btype.upper()}" in cache else "-O3 -DNDEBUG"

    has_cxx_blk = bool(ninja_info["has_cxx"])
    has_c_blk = bool(ninja_info["has_c"])
    if is_cxx:
        raw_cache_str = f"{cxx_init} {cxx_cfg}"
        if has_c_blk and ("CMAKE_C_FLAGS" in cache or f"CMAKE_C_FLAGS_{btype.upper()}" in cache):
            raw_cache_str = f"{raw_cache_str} {c_init} {c_cfg}"
    else:
        raw_cache_str = f"{c_init} {c_cfg}"
        if has_cxx_blk and ("CMAKE_CXX_FLAGS" in cache or f"CMAKE_CXX_FLAGS_{btype.upper()}" in cache):
            raw_cache_str = f"{raw_cache_str} {cxx_init} {cxx_cfg}"

    raw_cache_toks = shlex.split(raw_cache_str, posix=True)
    cache_cmd = _semantic_ninja_tokens(
        raw_cache_toks,
        [],
        include_fingerprint=False,
        deps_root=deps_root,
        search_dirs=base_search_dirs,
    )
    cache_cmd_fp = _semantic_ninja_tokens(
        raw_cache_toks,
        [],
        include_fingerprint=True,
        deps_root=deps_root,
        search_dirs=base_search_dirs,
    )

    blocks = list(ninja_info["blocks"])
    ninja_blocks_json = [
        {"lang": bl, "flags": list(fp_f), "defs": list(fp_d)}
        for bl, _df, _dd, fp_f, fp_d in blocks
    ]

    visited = set(seen_ids or ())
    visited.add(pid)
    subdeps_json: dict[str, object] = {}
    if deps_build_dir is not None:
        for sub_id in PACKAGE_SUBDEPS.get(pid, ()):
            if sub_id in visited or sub_id not in pkg_by_id:
                continue
            if (deps_build_dir / sub_id).is_dir():
                subdeps_json[sub_id] = _collect_pkg_fingerprint_state(
                    pkg_by_id[sub_id], pkg_by_id, deps_build_dir, cc_cache, visited
                )["payload"]

    payload = {
        "id": pid,
        "btype": btype,
        "compiler_id": compiler_id,
        "cache_cmd": cache_cmd_fp,
        "ninja_blocks": ninja_blocks_json,
        "cache_options": sem_cache,
        "cmake_options": list(pkg.get("cmake_options", [])),
        "subdeps": subdeps_json,
    }
    return {
        "cache": cache,
        "btype": btype,
        "compiler_id": compiler_id,
        "cache_cmd": cache_cmd,
        "blocks": blocks,
        "sem_cache": sem_cache,
        "payload": payload,
    }


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

    state = _collect_pkg_fingerprint_state(pkg, pkg_by_id, deps_build_dir, cc_cache)
    cache = state["cache"]
    btype = str(state["btype"])
    compiler_id = str(state["compiler_id"])
    cache_cmd = list(state["cache_cmd"])
    blocks = list(state["blocks"])

    cache_non_def = [t for t in cache_cmd if (not t.startswith("-D") and not t.startswith("-U")) or t == "-DNDEBUG"]
    cache_def = [t for t in cache_cmd if (t.startswith("-D") or t.startswith("-U")) and t != "-DNDEBUG"]

    block_flags: list[str] = []
    block_defs: list[str] = []
    disp_ninja_flags: list[str] = []
    first_bf: list[str] | None = None
    for bl, bf, bd, _fpf, _fpd in blocks:
        if bl != "asm":
            bf_non_def = [t for t in bf if (not t.startswith("-D") and not t.startswith("-U")) or t == "-DNDEBUG"]
            if first_bf is None:
                first_bf = bf_non_def
                disp_ninja_flags.extend(bf_non_def)
            else:
                k = 0
                while k < len(first_bf) and k < len(bf_non_def) and first_bf[k] == bf_non_def[k]:
                    k += 1
                disp_ninja_flags.extend(bf_non_def[k:])
        block_flags.extend(bf)
        block_defs.extend(bd)

    ninja_def_only = [
        t for t in (block_flags + block_defs) if (t.startswith("-D") or t.startswith("-U")) and t != "-DNDEBUG"
    ]
    if cache_non_def and disp_ninja_flags[: len(cache_non_def)] == cache_non_def:
        combined_non_def = disp_ninja_flags
    else:
        combined_non_def = cache_non_def + disp_ninja_flags

    combined_flag_tokens = cache_cmd + block_flags + block_defs

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

    ordered_tokens = (
        [f"-DCMAKE_BUILD_TYPE={btype}"]
        + combined_non_def
        + toggles
        + cache_def
        + ninja_def_only
        + opts
    )
    build_flags = _format_bounded_build_flags(
        ordered_tokens,
        f"-DCMAKE_BUILD_TYPE={btype}",
        255,
        fingerprint_payload=state["payload"],
    )

    aom_cache: dict[str, str] = {}
    aom_tokens: list[str] = []
    if pid == "libavif" and deps_build_dir is not None:
        aom_cache = _parse_cmake_cache(deps_build_dir / "libaom" / "CMakeCache.txt")
        aom_blk_info = _parse_ninja_compile_blocks(deps_build_dir / "libaom" / "build.ninja", True)
        for _bl, bf, bd, _fpf, _fpd in aom_blk_info["blocks"]:
            aom_tokens.extend(bf)
            aom_tokens.extend(bd)

    feats = pkg.get("features", {})
    simd = _derive_simd_summary(pid, feats, nasm_ver, cache, combined_flag_tokens, block_defs, aom_cache, aom_tokens)
    thr = _derive_threading_summary(pid, feats, cache, combined_flag_tokens, block_defs)
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
    raw_eng_flags = engine_flags.strip() if engine_flags else (
        "-std=c11 -O3 -DNDEBUG -march=x86-64-v2 -Wall -Wextra -Werror"
        if target == "windows-x64"
        else "-std=c11 -Wall -Wextra -Werror -pthread"
    )
    eng_flags = _format_bounded_build_flags(shlex.split(raw_eng_flags), raw_eng_flags, 255)

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
