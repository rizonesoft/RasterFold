"""Converter verification provenance inventory, frame digest, and reference decoders.

Provides:
1. A complete 19-format provenance inventory cross-checked against converter/codecs.json
   and converter/vendor.json, distinguishing executable D10 T01 s4 verifiers from
   unbuilt D10 T02 formats ("provenance-only-pending-adapter").
2. Runtime Pillow C-library inspection that explicitly denies independent-oracle status
   for formats backed by shared C codec families (libwebp, libtiff, libjpeg, openjpeg, zlib).
3. Canonical RFC-FRAMES-V1 SHA-256 frame digest matching converter/src/verify.c.
4. Clean-room pure-Python / oracle decoders and encoders for BMP, PNM, PAM, QOI, PNG,
   uncompressed scanline OpenEXR, and .fold (via tests/rfi_ref.py).
5. Executable verification contracts for JPEG DCT coefficients, OpenEXR IEEE-754 bit
   patterns (-0.0 vs +0.0 and NaN payloads), and LibRaw import-only deterministic rendering.
"""

from __future__ import annotations

import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import zlib
from typing import Any

ROOT = Path(__file__).resolve().parents[2]


def _load_rfi_ref():
    path = ROOT / "tests" / "rfi_ref.py"
    spec = importlib.util.spec_from_file_location("rf_rfi_ref", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rfi_ref = _load_rfi_ref()

SAMPLE_GRAY8 = 1
SAMPLE_GRAY16 = 2
SAMPLE_RGB8 = 3
SAMPLE_RGBA8 = 4
SAMPLE_RGB16 = 5
SAMPLE_RGBA16 = 6
SAMPLE_RGB_HALF = 7
SAMPLE_RGBA_HALF = 8
SAMPLE_RGB_FLOAT = 9
SAMPLE_RGBA_FLOAT = 10

ALPHA_NONE = 0
ALPHA_OPAQUE = 1
ALPHA_STRAIGHT = 2
ALPHA_PREMULTIPLIED = 3

_SAMPLE_LAYOUT_INFO: dict[int, tuple[int, int, bool]] = {
    SAMPLE_GRAY8: (1, 1, False),
    SAMPLE_GRAY16: (1, 2, False),
    SAMPLE_RGB8: (3, 1, False),
    SAMPLE_RGBA8: (4, 1, False),
    SAMPLE_RGB16: (3, 2, False),
    SAMPLE_RGBA16: (4, 2, False),
    SAMPLE_RGB_HALF: (3, 2, True),
    SAMPLE_RGBA_HALF: (4, 2, True),
    SAMPLE_RGB_FLOAT: (3, 4, True),
    SAMPLE_RGBA_FLOAT: (4, 4, True),
}

PROVENANCE_TABLE: tuple[dict[str, Any], ...] = (
    {
        "format_name": "rasterfold",
        "owner_section": "D10 T02 \u00a71",
        "exactness_class": "pixel-exact",
        "converter_provider": "RasterFold (this repository)",
        "pinned_package_ids": (),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-oracle",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Independent Python spec oracle tests/rfi_ref.py decodes .fold output without calling librasterfold.",
    },
    {
        "format_name": "bmp",
        "owner_section": "D10 T02 \u00a71",
        "exactness_class": "pixel-exact",
        "converter_provider": "in-tree converter code",
        "pinned_package_ids": (),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Pure-Python BMP BGR24/BGRA32 parser independently decodes test payloads.",
    },
    {
        "format_name": "png",
        "owner_section": "D10 T02 \u00a71",
        "exactness_class": "pixel-exact",
        "converter_provider": "libpng (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libpng", "zlib"),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Pure-Python PNG chunk/scanline unfilter reference using stdlib zlib (never libpng; Pillow zlib binding is flagged non-independent).",
    },
    {
        "format_name": "apng",
        "owner_section": "D10 T02 \u00a71",
        "exactness_class": "pixel-exact",
        "converter_provider": "libpng (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libpng", "zlib"),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "same-provider-fresh-session",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "provenance-only-pending-adapter",
        "provenance_note": "No APNG adapter until D10 T02 s1 ships; runtime verification uses fresh session decode plus multi-frame digest.",
    },
    {
        "format_name": "pnm",
        "owner_section": "D10 T02 \u00a71",
        "exactness_class": "pixel-exact",
        "converter_provider": "in-tree converter code",
        "pinned_package_ids": (),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Pure-Python P5/P6 big-endian-to-little-endian parser.",
    },
    {
        "format_name": "pam",
        "owner_section": "D10 T02 \u00a71",
        "exactness_class": "pixel-exact",
        "converter_provider": "in-tree converter code",
        "pinned_package_ids": (),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Pure-Python P7 TUPLTYPE parser for GRAYSCALE/RGB/RGB_ALPHA.",
    },
    {
        "format_name": "webp",
        "owner_section": "D10 T02 \u00a72",
        "exactness_class": "pixel-exact",
        "converter_provider": "libwebp (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libwebp",),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "shared-upstream-library",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t02-s2",
        "provenance_note": "Verified by fresh WebPDecode/WebPDemux session and Pillow (shared libwebp), plus independent clean-room FFmpeg libavcodec webp decoder when available.",
    },
    {
        "format_name": "jpeg-xl",
        "owner_section": "D10 T02 \u00a73",
        "exactness_class": "original-byte-reconstruction",
        "converter_provider": "libjxl (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libjxl", "highway", "brotli", "lcms2"),
        "verification_path": "reconstructed-jpeg-byte-compare",
        "reference_decoder_kind": "shared-upstream-library",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t02-s3",
        "provenance_note": "Verified by fresh JxlDecoder/reconstruct session, upstream libjxl contract, and FFmpeg libavcodec jpegxl (shared libjxl); original-byte-reconstruction hashes reconstructed JPEG bytes against source JPEG bytes.",
    },
    {
        "format_name": "avif",
        "owner_section": "D10 T02 \u00a74",
        "exactness_class": "pixel-exact",
        "converter_provider": "libavif (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libavif", "libaom"),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "shared-upstream-library",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t02-s4",
        "provenance_note": "Verified by fresh avifDecoder session preserving n-bit left-alignment, clean-room Windows FFmpeg libavformat+libdav1d decode, tests/rfi_ref.py .fold oracle, and non-independent Pillow PIL._avif cross-check.",
    },
    {
        "format_name": "jpeg",
        "owner_section": "D10 T02 \u00a75",
        "exactness_class": "coefficient-exact",
        "converter_provider": "libjpeg-turbo (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libjpeg-turbo",),
        "verification_path": "jpeg-dct-quant-compare",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t02-s5",
        "provenance_note": "Huffman (baseline, extended, progressive) coefficients and ISLOW/fancy-upsampled pixels verified by the independent pure-Python tests/converter/jpeg_ref.py; arithmetic-coded JPEG is verified by a fresh libjpeg-turbo session only (shared provider); Pillow jpg shares libjpeg.",
    },
    {
        "format_name": "gif",
        "owner_section": "D10 T02 \u00a76",
        "exactness_class": "pixel-exact",
        "converter_provider": "giflib (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("giflib",),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t02-s6",
        "provenance_note": "Single-image GIF verified by a fresh giflib session after the adapter's own LZW length check, and independently by the pure-Python tests/converter/tiff_gif_ref.py LZW/interlace/transparency reader.",
    },
    {
        "format_name": "tiff",
        "owner_section": "D10 T02 \u00a76",
        "exactness_class": "pixel-exact",
        "converter_provider": "libtiff (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libtiff", "zlib", "libdeflate", "libjpeg-turbo", "libwebp"),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t02-s6",
        "provenance_note": "Uncompressed, PackBits, LZW and Deflate TIFF (strips, tiles, contiguous and separate planes, 8/16-bit, horizontal predictor) verified by the independent pure-Python tests/converter/tiff_gif_ref.py; JPEG-compressed (RGB and YCbCr) and WebP-lossless TIFF only by a fresh libtiff session (shared provider; WebP checked for exact samples, JPEG for closeness to the uncompressed pattern); Pillow tiff binds libtiff.",
    },
    {
        "format_name": "heif",
        "owner_section": "D10 T02 \u00a77",
        "exactness_class": "pixel-exact",
        "converter_provider": "Windows Imaging Component (system codec, not bundled)",
        "pinned_package_ids": (),
        "verification_path": "import-only-source",
        "reference_decoder_kind": "system-codec-non-independent",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t02-s7",
        "provenance_note": "Import-only through the Windows Imaging Component HEIF decoder with the system HEVC decoder (no HEVC code bundled); 8-bit opaque pictures checked against a second fresh WIC decode of the same bytes and for closeness to the encoded pattern (shared system provider, no independent decoder); orientation is applied by the system decoder; the container facts come from the converter's own ISOBMFF reader.",
    },
    {
        "format_name": "heic",
        "owner_section": "D10 T02 \u00a77",
        "exactness_class": "pixel-exact",
        "converter_provider": "Windows Imaging Component (system codec, not bundled)",
        "pinned_package_ids": (),
        "verification_path": "import-only-source",
        "reference_decoder_kind": "system-codec-non-independent",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t02-s7",
        "provenance_note": "Import-only through the Windows Imaging Component HEIF decoder with the system HEVC decoder (no HEVC code bundled); 8-bit opaque pictures checked against a second fresh WIC decode of the same bytes and for closeness to the encoded pattern (shared system provider, no independent decoder); orientation is applied by the system decoder; the container facts come from the converter's own ISOBMFF reader.",
    },
    {
        "format_name": "jpeg-2000",
        "owner_section": "D10 T02 \u00a79",
        "exactness_class": "pixel-exact",
        "converter_provider": "OpenJPEG (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("openjpeg",),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "shared-upstream-library",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "provenance-only-pending-adapter",
        "provenance_note": "Pending D10 T02 s9; Pillow openjp2 shares OpenJPEG C library.",
    },
    {
        "format_name": "qoi",
        "owner_section": "D10 T02 \u00a78",
        "exactness_class": "pixel-exact",
        "converter_provider": "qoi (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("qoi",),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t02-s8",
        "provenance_note": "Clean-room pure-Python QOI specification encoder/decoder.",
    },
    {
        "format_name": "openexr",
        "owner_section": "D10 T02 \u00a710",
        "exactness_class": "pixel-exact",
        "converter_provider": "OpenEXR (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("openexr", "imath", "openjph", "libdeflate"),
        "verification_path": "fresh-session-decode",
        "reference_decoder_kind": "independent-pure-python-uncompressed",
        "shares_c_library_with_converter": False,
        "claims_independent_decoder": True,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Clean-room pure-Python uncompressed NO_COMPRESSION=0 HALF/FLOAT parser preserving -0.0 and NaN payloads; compressed EXR uses OpenEXRCore in D10 T02 s10.",
    },
    {
        "format_name": "camera-raw",
        "owner_section": "D10 T02 \u00a711",
        "exactness_class": "pixel-exact",
        "converter_provider": "LibRaw (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libraw", "lcms2", "libjpeg-turbo", "zlib"),
        "verification_path": "import-only-source",
        "reference_decoder_kind": "deterministic-render-contract",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Import-only fixed-render contract executed in D10 T01 s4; LibRaw decoding adapter lands in D10 T02 s11.",
    },
    {
        "format_name": "dng",
        "owner_section": "D10 T02 \u00a711",
        "exactness_class": "pixel-exact",
        "converter_provider": "LibRaw (bundled, pinned by D10 T01 \u00a72)",
        "pinned_package_ids": ("libraw", "lcms2", "libjpeg-turbo", "zlib"),
        "verification_path": "import-only-source",
        "reference_decoder_kind": "deterministic-render-contract",
        "shares_c_library_with_converter": True,
        "claims_independent_decoder": False,
        "coverage_state": "executable-in-d10-t01-s4",
        "provenance_note": "Import-only fixed-render contract executed in D10 T01 s4; LibRaw DNG adapter lands in D10 T02 s11.",
    },
)

JPEG_COEFFICIENT_CONTRACT: dict[str, Any] = {
    "exactness": "coefficient-exact",
    "requires_dct_domain_equality": True,
    "checked_fields": (
        "width",
        "height",
        "progressive",
        "arithmetic_coding",
        "baseline",
        "component_count",
        "component_id",
        "h_samp_factor",
        "v_samp_factor",
        "quant_table_idx",
        "dc_table_idx",
        "ac_table_idx",
        "blocks_wide",
        "blocks_high",
        "quant_tables",
        "dct_blocks",
    ),
    "allows_pixel_only_equivalence": False,
}

OPENEXR_BIT_PATTERN_CONTRACT: dict[str, Any] = {
    "half_bits": 16,
    "float_bits": 32,
    "signed_zero_must_match": True,
    "nan_payload_must_match": True,
    "numeric_tolerance": 0.0,
}

LIBRAW_RENDERING_CONTRACT: dict[str, Any] = {
    "import_only": True,
    "allows_lossless_roundtrip_to_raw": False,
    "required_fixed_parameters": ("color_space", "white_balance", "highlight_mode", "output_bps"),
    "forbids_auto_brighten": True,
    "downstream_target_verification": "pixel-exact",
}


def validate_provenance_table() -> dict[str, dict[str, Any]]:
    """Cross-check PROVENANCE_TABLE against converter/codecs.json and converter/vendor.json."""
    codecs_doc = json.loads((ROOT / "converter" / "codecs.json").read_text(encoding="utf-8"))
    vendor_doc = json.loads((ROOT / "converter" / "vendor.json").read_text(encoding="utf-8"))

    codec_by_name = {row["format"]: row for row in codecs_doc["formats"]}
    pkg_ids = {pkg["id"] for pkg in vendor_doc["packages"]}
    sys_formats: set[str] = set()
    for srow in vendor_doc.get("system_codecs", []):
        sys_formats.update(srow["formats"])

    by_name: dict[str, dict[str, Any]] = {}
    for row in PROVENANCE_TABLE:
        name = row["format_name"]
        if name in by_name:
            raise AssertionError(f"duplicate provenance format: {name}")
        if name not in codec_by_name:
            raise AssertionError(f"provenance format missing from codecs.json: {name}")
        c_row = codec_by_name[name]
        if row["owner_section"] != c_row["owner"]:
            raise AssertionError(
                f"{name}: owner mismatch {row['owner_section']!r} vs {c_row['owner']!r}"
            )
        if row["converter_provider"] != c_row["provider"]:
            raise AssertionError(
                f"{name}: provider mismatch {row['converter_provider']!r} vs {c_row['provider']!r}"
            )
        for pid in row["pinned_package_ids"]:
            if pid not in pkg_ids:
                raise AssertionError(f"{name}: unknown pinned package {pid!r}")
        if row["shares_c_library_with_converter"] and row["claims_independent_decoder"]:
            raise AssertionError(
                f"{name}: shared C library entry cannot claim independent decoder status"
            )
        if name in sys_formats and row["pinned_package_ids"]:
            raise AssertionError(f"{name}: system codec must not claim bundled packages")
        if row["coverage_state"] not in (
            "executable-in-d10-t01-s4",
            "executable-in-d10-t02-s2",
            "executable-in-d10-t02-s3",
            "executable-in-d10-t02-s4",
            "executable-in-d10-t02-s5",
            "executable-in-d10-t02-s6",
            "executable-in-d10-t02-s7",
            "executable-in-d10-t02-s8",
            "provenance-only-pending-adapter",
        ):
            raise AssertionError(f"{name}: invalid coverage_state {row['coverage_state']!r}")
        by_name[name] = row

    if set(by_name.keys()) != set(codec_by_name.keys()):
        missing = sorted(set(codec_by_name.keys()) - set(by_name.keys()))
        raise AssertionError(f"missing provenance rows: {missing}")
    return by_name


def inspect_pillow_codec_provenance() -> dict[str, Any]:
    """Inspect Pillow runtime C bindings and declare them non-independent where shared."""
    bindings = (
        ("webp", "webp", "libwebp"),
        ("libtiff", "libtiff", "libtiff"),
        ("jpg", "jpg", "libjpeg/libjpeg-turbo"),
        ("openjp2", "jpg_2000", "openjpeg"),
        ("zlib", "zlib", "zlib/libpng"),
    )
    try:
        features = importlib.import_module("PIL.features")
    except ImportError:
        return {
            "pillow_installed": False,
            "codecs": {
                key: {
                    "available": False,
                    "pillow_feature": pfeat,
                    "classification": "shared-upstream-library",
                    "c_library_family": fam,
                    "claims_independent_decoder": False,
                    "reason": "Pillow not installed; shared C libraries cannot act as independent oracle",
                }
                for key, pfeat, fam in bindings
            },
        }

    supported_codecs = set(features.get_supported_codecs())
    codecs: dict[str, Any] = {}
    for key, pfeat, fam in bindings:
        avail = (pfeat in supported_codecs) or bool(features.check(pfeat))
        codecs[key] = {
            "available": avail,
            "pillow_feature": pfeat,
            "classification": "shared-upstream-library",
            "c_library_family": fam,
            "claims_independent_decoder": False,
            "reason": f"Pillow '{pfeat}' binds {fam} C library family and is never an independent oracle",
        }
    return {"pillow_installed": True, "codecs": codecs}


def bytes_hash_sha256(data: bytes) -> bytes:
    """Return raw 32-byte FIPS 180-4 SHA-256 digest."""
    return hashlib.sha256(data).digest()


def frames_hash_sha256(frames: list[dict[str, Any]]) -> bytes:
    """Compute canonical RFC-FRAMES-V1 SHA-256 digest matching converter/src/verify.c."""
    h = hashlib.sha256()
    h.update(b"RFC-FRAMES-V1\x00")
    h.update(struct.pack("<Q", len(frames)))
    for f in frames:
        w = int(f["width"])
        ht = int(f["height"])
        sfmt = int(f["sample_format"])
        alpha = int(f.get("alpha", ALPHA_NONE))
        bps = int(f["bits_per_sample"])
        dur = int(f.get("duration_ms", 0))
        if sfmt not in _SAMPLE_LAYOUT_INFO:
            raise ValueError(f"unknown sample_format {sfmt}")
        channels, ch_bytes, is_float = _SAMPLE_LAYOUT_INFO[sfmt]
        storage_bits = ch_bytes * 8
        if bps == 0 or bps > storage_bits:
            raise ValueError(f"invalid bits_per_sample {bps} for sample_format {sfmt}")
        bpp = channels * ch_bytes
        active_row = w * bpp
        stride = int(f.get("stride_bytes", active_row))
        if stride < active_row:
            raise ValueError("stride_bytes smaller than active row")
        data: bytes = f["data"]
        if len(data) < (ht - 1) * stride + active_row:
            raise ValueError("frame buffer shorter than stride * height")

        pad_mask = 0 if (is_float or bps >= storage_bits) else ((1 << (storage_bits - bps)) - 1)
        h.update(struct.pack("<IIIIII", w, ht, sfmt, alpha, bps, dur))
        fmt_ch = "<H" if ch_bytes == 2 else ("<I" if ch_bytes == 4 else None)
        for y in range(ht):
            off = y * stride
            row = data[off : off + active_row]
            if pad_mask != 0 and fmt_ch is not None:
                for pos in range(0, active_row, ch_bytes):
                    val = struct.unpack_from(fmt_ch, row, pos)[0]
                    if val & pad_mask:
                        raise ValueError("nonzero low padding bit in multi-byte sample")
            h.update(row)
    return h.digest()


# ---------------------------------------------------------------- BMP (24/32-bit)

def encode_bmp(width: int, height: int, pixels: bytes, channels: int = 3, topdown: bool = False) -> bytes:
    """Encode tightly packed RGB8 or RGBA8 into an uncompressed BMP."""
    if channels not in (3, 4):
        raise ValueError("BMP helper supports 3 or 4 channels")
    if len(pixels) != width * height * channels:
        raise ValueError("pixel buffer length mismatch")
    row_bytes = width * channels
    stride = (row_bytes + 3) & ~3
    img_bytes = stride * height
    off = 54
    out = bytearray(off + img_bytes)
    struct.pack_into("<2sIHHI", out, 0, b"BM", len(out), 0, 0, off)
    stored_h = -height if topdown else height
    struct.pack_into("<IiiHHIIiiII", out, 14, 40, width, stored_h, 1, channels * 8, 0, img_bytes, 3780, 3780, 0, 0)
    for y in range(height):
        src_y = y if topdown else height - 1 - y
        src = pixels[src_y * row_bytes : (src_y + 1) * row_bytes]
        dst = off + y * stride
        for x in range(width):
            i = x * channels
            out[dst + i] = src[i + 2]
            out[dst + i + 1] = src[i + 1]
            out[dst + i + 2] = src[i]
            if channels == 4:
                out[dst + i + 3] = src[i + 3]
    return bytes(out)


def decode_bmp(blob: bytes) -> tuple[int, int, int, bytes]:
    """Decode uncompressed 24-bit BGR or 32-bit BGRA BMP into RGB8 or RGBA8."""
    if len(blob) < 54 or blob[:2] != b"BM":
        raise ValueError("invalid BMP header")
    _bf_size, _r1, _r2, off = struct.unpack_from("<IHHI", blob, 2)
    bi_size, w, h_raw, planes, bpp, comp, _sz, _x, _y, _cu, _ci = struct.unpack_from("<IiiHHIIiiII", blob, 14)
    if bi_size < 40 or planes != 1 or bpp not in (24, 32) or comp != 0 or w <= 0 or h_raw == 0:
        raise ValueError("unsupported BMP layout")
    topdown = h_raw < 0
    ht = abs(h_raw)
    ch = bpp // 8
    row_bytes = w * ch
    stride = (row_bytes + 3) & ~3
    if off + stride * ht > len(blob):
        raise ValueError("truncated BMP data")
    out = bytearray(w * ht * ch)
    for file_y in range(ht):
        img_y = file_y if topdown else ht - 1 - file_y
        src = off + file_y * stride
        dst = img_y * row_bytes
        for x in range(w):
            i = x * ch
            out[dst + i] = blob[src + i + 2]
            out[dst + i + 1] = blob[src + i + 1]
            out[dst + i + 2] = blob[src + i]
            if ch == 4:
                out[dst + i + 3] = blob[src + i + 3]
    return w, ht, ch, bytes(out)


# ---------------------------------------------------------------- PNM / PAM

def _read_pnm_token(blob: bytes, pos: int) -> tuple[bytes, int]:
    n = len(blob)
    while pos < n:
        c = blob[pos]
        if c in b" \t\r\n":
            pos += 1
        elif c == ord("#"):
            while pos < n and blob[pos] != ord("\n"):
                pos += 1
        else:
            break
    start = pos
    while pos < n and blob[pos] not in b" \t\r\n#":
        pos += 1
    if pos == start:
        raise ValueError("unexpected EOF in PNM header")
    return blob[start:pos], pos


def encode_pnm(width: int, height: int, channels: int, maxval: int, data: bytes) -> bytes:
    """Encode P5 (gray) or P6 (RGB) 8/16-bit binary PNM (big-endian for 16-bit)."""
    magic = b"P5" if channels == 1 else b"P6"
    hdr = f"{magic.decode('ascii')}\n{width} {height}\n{maxval}\n".encode("ascii")
    if maxval < 256:
        return hdr + data
    out = bytearray(len(hdr) + len(data))
    out[: len(hdr)] = hdr
    for off in range(0, len(data), 2):
        val = struct.unpack_from("<H", data, off)[0]
        struct.pack_into(">H", out, len(hdr) + off, val)
    return bytes(out)


def decode_pnm(blob: bytes) -> tuple[int, int, int, int, bytes]:
    """Decode P5/P6 binary PNM returning native little-endian bytes for 16-bit."""
    magic, pos = _read_pnm_token(blob, 0)
    if magic not in (b"P5", b"P6"):
        raise ValueError(f"unsupported PNM magic {magic!r}")
    channels = 1 if magic == b"P5" else 3
    w_tok, pos = _read_pnm_token(blob, pos)
    h_tok, pos = _read_pnm_token(blob, pos)
    mv_tok, pos = _read_pnm_token(blob, pos)
    if pos >= len(blob) or blob[pos] not in b" \t\r\n":
        raise ValueError("missing PNM header whitespace separator")
    pos += 1
    w, ht, maxval = int(w_tok), int(h_tok), int(mv_tok)
    if w <= 0 or ht <= 0 or not (1 <= maxval <= 65535):
        raise ValueError("invalid PNM header values")
    bps_bytes = 2 if maxval >= 256 else 1
    need = w * ht * channels * bps_bytes
    payload = blob[pos:]
    if len(payload) != need:
        raise ValueError("PNM payload size mismatch")
    if bps_bytes == 1:
        return w, ht, channels, maxval, payload
    out = bytearray(need)
    for off in range(0, need, 2):
        val = struct.unpack_from(">H", payload, off)[0]
        struct.pack_into("<H", out, off, val)
    return w, ht, channels, maxval, bytes(out)


def encode_pam(width: int, height: int, depth: int, maxval: int, tupltype: str, data: bytes) -> bytes:
    """Encode P7 PAM (host-LE converted to big-endian when maxval >= 256)."""
    hdr = (
        f"P7\nWIDTH {width}\nHEIGHT {height}\nDEPTH {depth}\n"
        f"MAXVAL {maxval}\nTUPLTYPE {tupltype}\nENDHDR\n"
    ).encode("ascii")
    if maxval < 256:
        return hdr + data
    out = bytearray(len(hdr) + len(data))
    out[: len(hdr)] = hdr
    for off in range(0, len(data), 2):
        struct.pack_into(">H", out, len(hdr) + off, struct.unpack_from("<H", data, off)[0])
    return bytes(out)


def decode_pam(blob: bytes) -> tuple[int, int, int, int, str, bytes]:
    """Decode P7 PAM into native little-endian bytes for 16-bit samples."""
    nl = blob.find(b"ENDHDR\n")
    if nl < 0 or not blob.startswith(b"P7\n"):
        raise ValueError("invalid PAM header")
    hdr_lines = blob[3:nl].decode("ascii").splitlines()
    fields: dict[str, str] = {}
    for line in hdr_lines:
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        k, v = s.split(None, 1)
        fields[k] = v
    w = int(fields["WIDTH"])
    ht = int(fields["HEIGHT"])
    depth = int(fields["DEPTH"])
    maxval = int(fields["MAXVAL"])
    tupltype = fields["TUPLTYPE"]
    payload = blob[nl + 7 :]
    bps_bytes = 2 if maxval >= 256 else 1
    need = w * ht * depth * bps_bytes
    if len(payload) != need:
        raise ValueError("PAM payload size mismatch")
    if bps_bytes == 1:
        return w, ht, depth, maxval, tupltype, payload
    out = bytearray(need)
    for off in range(0, need, 2):
        struct.pack_into("<H", out, off, struct.unpack_from(">H", payload, off)[0])
    return w, ht, depth, maxval, tupltype, bytes(out)


# ---------------------------------------------------------------- QOI

def encode_qoi(width: int, height: int, channels: int, pixels: bytes, colorspace: int = 0) -> bytes:
    """Clean-room QOI specification encoder."""
    if channels not in (3, 4) or len(pixels) != width * height * channels:
        raise ValueError("invalid QOI input")
    out = bytearray(struct.pack(">4sIIBB", b"qoif", width, height, channels, colorspace))
    index = [(0, 0, 0, 0)] * 64
    pr, pg, pb, pa = 0, 0, 0, 255
    run = 0
    total = width * height
    for i in range(total):
        off = i * channels
        r, g, b = pixels[off], pixels[off + 1], pixels[off + 2]
        a = pixels[off + 3] if channels == 4 else 255
        if (r, g, b, a) == (pr, pg, pb, pa):
            run += 1
            if run == 62 or i == total - 1:
                out.append(0xC0 | (run - 1))
                run = 0
            continue
        if run > 0:
            out.append(0xC0 | (run - 1))
            run = 0
        idx = (r * 3 + g * 5 + b * 7 + a * 11) & 63
        if index[idx] == (r, g, b, a):
            out.append(idx)
        else:
            index[idx] = (r, g, b, a)
            if a == pa:
                vr = ((r - pr + 128) & 255) - 128
                vg = ((g - pg + 128) & 255) - 128
                vb = ((b - pb + 128) & 255) - 128
                vgr = vr - vg
                vgb = vb - vg
                if -2 <= vr <= 1 and -2 <= vg <= 1 and -2 <= vb <= 1:
                    out.append(0x40 | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2))
                elif -32 <= vg <= 31 and -8 <= vgr <= 7 and -8 <= vgb <= 7:
                    out.append(0x80 | (vg + 32))
                    out.append(((vgr + 8) << 4) | (vgb + 8))
                else:
                    out.extend((0xFE, r, g, b))
            else:
                out.extend((0xFF, r, g, b, a))
        pr, pg, pb, pa = r, g, b, a
    out.extend(b"\x00\x00\x00\x00\x00\x00\x00\x01")
    return bytes(out)


def decode_qoi_ex(blob: bytes) -> tuple[int, int, int, int, bytes]:
    """Strict clean-room QOI specification decoder returning (w, h, ch, colorspace, pixels)."""
    if len(blob) < 22 or blob[:4] != b"qoif" or blob[-8:] != b"\x00\x00\x00\x00\x00\x00\x00\x01":
        raise ValueError("invalid QOI framing")
    magic, w, ht, ch, cs = struct.unpack_from(">4sIIBB", blob, 0)
    if magic != b"qoif" or w == 0 or ht == 0 or ch not in (3, 4) or cs not in (0, 1):
        raise ValueError("invalid QOI header")
    total_px = w * ht
    if total_px >= 400_000_000:
        raise ValueError("QOI dimensions exceed QOI_PIXELS_MAX")
    end = len(blob) - 8
    # Zero-allocation chunk-coverage pre-scan before allocating pixel buffer.
    scan_pos = 14
    scan_px = 0
    scan_prev_idx = -1
    while scan_px < total_px:
        if scan_pos >= end:
            raise ValueError("truncated QOI stream")
        tag = blob[scan_pos]
        scan_pos += 1
        if tag == 0xFE:
            if end - scan_pos < 3:
                raise ValueError("truncated QOI_OP_RGB")
            scan_pos += 3
            scan_px += 1
            scan_prev_idx = -1
        elif tag == 0xFF:
            if end - scan_pos < 4:
                raise ValueError("truncated QOI_OP_RGBA")
            scan_pos += 4
            scan_px += 1
            scan_prev_idx = -1
        elif (tag & 0xC0) == 0x00:
            if tag == scan_prev_idx:
                raise ValueError("consecutive identical QOI_OP_INDEX chunks")
            scan_prev_idx = tag
            scan_px += 1
        elif (tag & 0xC0) == 0x80:
            if end - scan_pos < 1:
                raise ValueError("truncated QOI_OP_LUMA")
            scan_pos += 1
            scan_px += 1
            scan_prev_idx = -1
        elif (tag & 0xC0) == 0xC0:
            run_len = (tag & 0x3F) + 1
            if run_len > total_px - scan_px:
                raise ValueError("QOI_OP_RUN exceeds image pixel count")
            scan_px += run_len
            scan_prev_idx = -1
        else:
            scan_px += 1
            scan_prev_idx = -1
    if scan_pos != end:
        raise ValueError("trailing QOI data")
    out = bytearray(total_px * ch)
    index = [(0, 0, 0, 0)] * 64
    r, g, b, a = 0, 0, 0, 255
    pos = 14
    end = len(blob) - 8
    run = 0
    prev_index_tag = -1
    for px in range(total_px):
        if run > 0:
            run -= 1
        else:
            if pos >= end:
                raise ValueError("truncated QOI stream")
            tag = blob[pos]
            pos += 1
            if tag == 0xFE:
                if end - pos < 3:
                    raise ValueError("truncated QOI_OP_RGB")
                r, g, b = blob[pos], blob[pos + 1], blob[pos + 2]
                pos += 3
                prev_index_tag = -1
            elif tag == 0xFF:
                if end - pos < 4:
                    raise ValueError("truncated QOI_OP_RGBA")
                r, g, b, a = blob[pos], blob[pos + 1], blob[pos + 2], blob[pos + 3]
                pos += 4
                prev_index_tag = -1
            elif (tag & 0xC0) == 0x00:
                # Per QOI spec, two consecutive QOI_OP_INDEX chunks to the same slot are invalid.
                if tag == prev_index_tag:
                    raise ValueError("consecutive identical QOI_OP_INDEX chunks")
                prev_index_tag = tag
                r, g, b, a = index[tag]
            elif (tag & 0xC0) == 0x40:
                r = (r + ((tag >> 4) & 3) - 2) & 255
                g = (g + ((tag >> 2) & 3) - 2) & 255
                b = (b + (tag & 3) - 2) & 255
                prev_index_tag = -1
            elif (tag & 0xC0) == 0x80:
                if end - pos < 1:
                    raise ValueError("truncated QOI_OP_LUMA")
                b2 = blob[pos]
                pos += 1
                vg = (tag & 0x3F) - 32
                r = (r + vg - 8 + ((b2 >> 4) & 0x0F)) & 255
                g = (g + vg) & 255
                b = (b + vg - 8 + (b2 & 0x0F)) & 255
                prev_index_tag = -1
            else:
                run = tag & 0x3F
                if run + 1 > total_px - px:
                    raise ValueError("QOI_OP_RUN exceeds image pixel count")
                prev_index_tag = -1
            idx = (r * 3 + g * 5 + b * 7 + a * 11) & 63
            index[idx] = (r, g, b, a)
        dst = px * ch
        out[dst], out[dst + 1], out[dst + 2] = r, g, b
        if ch == 4:
            out[dst + 3] = a
    if run != 0 or pos != end:
        raise ValueError("trailing QOI data or unconsumed run")
    return w, ht, ch, cs, bytes(out)


def decode_qoi(blob: bytes) -> tuple[int, int, int, bytes]:
    """Clean-room QOI specification decoder."""
    w, ht, ch, _cs, pixels = decode_qoi_ex(blob)
    return w, ht, ch, pixels


# ---------------------------------------------------------------- PNG (pure-Python chunks + unfiltering)

def _png_chunk(ctype: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", crc)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def encode_png(width: int, height: int, color_type: int, bit_depth: int, pixels_native: bytes) -> bytes:
    """Clean-room PNG encoder for gray8(0,8), rgb8(2,8), rgba8(6,8), rgb16(2,16)."""
    ch_map = {0: 1, 2: 3, 6: 4}
    if color_type not in ch_map or bit_depth not in (8, 16):
        raise ValueError("unsupported PNG test pixel configuration")
    ch = ch_map[color_type]
    bps_bytes = bit_depth // 8
    row_bytes = width * ch * bps_bytes
    if len(pixels_native) != row_bytes * height:
        raise ValueError("PNG pixel buffer length mismatch")
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # Filter None
        row = pixels_native[y * row_bytes : (y + 1) * row_bytes]
        if bps_bytes == 1:
            raw.extend(row)
        else:
            for off in range(0, row_bytes, 2):
                raw.extend(struct.pack(">H", struct.unpack_from("<H", row, off)[0]))
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=6)
    return b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"IDAT", idat) + _png_chunk(b"IEND", b"")


def decode_png(blob: bytes) -> tuple[int, int, int, int, bytes]:
    """Clean-room PNG parser and 5-filter reconstructor returning little-endian 16-bit samples."""
    if not blob.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("invalid PNG signature")
    pos = 8
    width = height = bit_depth = color_type = 0
    idat_parts: list[bytes] = []
    saw_iend = False
    while pos < len(blob):
        length = struct.unpack_from(">I", blob, pos)[0]
        ctype = blob[pos + 4 : pos + 8]
        cdata = blob[pos + 8 : pos + 8 + length]
        crc_got = struct.unpack_from(">I", blob, pos + 8 + length)[0]
        if (zlib.crc32(ctype + cdata) & 0xFFFFFFFF) != crc_got:
            raise ValueError("PNG CRC mismatch")
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, comp, flt, inter = struct.unpack(">IIBBBBB", cdata)
            if comp != 0 or flt != 0 or inter != 0:
                raise ValueError("unsupported PNG header flags")
        elif ctype == b"IDAT":
            idat_parts.append(cdata)
        elif ctype == b"IEND":
            saw_iend = True
            break
    if not saw_iend or width == 0 or height == 0:
        raise ValueError("incomplete PNG chunks")
    ch = {0: 1, 2: 3, 6: 4}[color_type]
    bpp = ch * (bit_depth // 8)
    stride = width * bpp
    raw = zlib.decompress(b"".join(idat_parts))
    if len(raw) != (stride + 1) * height:
        raise ValueError("PNG decompressed byte count mismatch")

    recon = bytearray(stride * height)
    prev = bytes(stride)
    in_pos = 0
    for y in range(height):
        flt = raw[in_pos]
        in_pos += 1
        row = bytearray(raw[in_pos : in_pos + stride])
        in_pos += stride
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if flt == 1:
                row[x] = (row[x] + a) & 255
            elif flt == 2:
                row[x] = (row[x] + b) & 255
            elif flt == 3:
                row[x] = (row[x] + ((a + b) >> 1)) & 255
            elif flt == 4:
                row[x] = (row[x] + _paeth(a, b, c)) & 255
            elif flt != 0:
                raise ValueError(f"invalid PNG filter {flt}")
        dst = y * stride
        recon[dst : dst + stride] = row
        prev = bytes(row)

    if bit_depth == 8:
        return width, height, ch, bit_depth, bytes(recon)
    native = bytearray(len(recon))
    for off in range(0, len(recon), 2):
        struct.pack_into("<H", native, off, struct.unpack_from(">H", recon, off)[0])
    return width, height, ch, bit_depth, bytes(native)


# ---------------------------------------------------------------- OpenEXR (uncompressed NO_COMPRESSION=0 scanline)

def encode_openexr_uncompressed(
    width: int,
    height: int,
    channels: tuple[str, ...],
    pixel_type: int,  # 1=HALF (uint16), 2=FLOAT (uint32)
    interleaved_words: bytes,
) -> bytes:
    """Clean-room single-part uncompressed scanline OpenEXR encoder preserving raw float bits."""
    if pixel_type not in (1, 2):
        raise ValueError("pixel_type must be 1 (HALF) or 2 (FLOAT)")
    word_bytes = 2 if pixel_type == 1 else 4
    ch_names = sorted(channels)
    if len(interleaved_words) != width * height * len(channels) * word_bytes:
        raise ValueError("EXR pixel word buffer length mismatch")

    chlist = bytearray()
    for cname in ch_names:
        chlist.extend(cname.encode("ascii") + b"\x00")
        # OpenEXR chlist item: int32 pixel_type, uint8 pLinear, 3 reserved bytes, int32 xSampling, int32 ySampling (16B).
        chlist.extend(struct.pack("<IB3xii", pixel_type, 0, 1, 1))
    chlist.append(0)

    def attr(name: str, atype: str, val: bytes) -> bytes:
        return (
            name.encode("ascii")
            + b"\x00"
            + atype.encode("ascii")
            + b"\x00"
            + struct.pack("<I", len(val))
            + val
        )

    dw = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    hdr = bytearray(struct.pack("<II", 20000630, 2))
    hdr.extend(attr("channels", "chlist", bytes(chlist)))
    hdr.extend(attr("compression", "compression", bytes((0,))))
    hdr.extend(attr("dataWindow", "box2i", dw))
    hdr.extend(attr("displayWindow", "box2i", dw))
    hdr.extend(attr("lineOrder", "lineOrder", bytes((0,))))
    hdr.extend(attr("pixelAspectRatio", "float", struct.pack("<f", 1.0)))
    hdr.extend(attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0)))
    hdr.extend(attr("screenWindowWidth", "float", struct.pack("<f", 1.0)))
    hdr.append(0)

    offset_table_pos = len(hdr)
    hdr.extend(b"\x00" * (8 * height))
    out = bytearray(hdr)

    ch_order = {c: idx for idx, c in enumerate(channels)}
    num_ch = len(channels)
    scan_bytes = len(ch_names) * width * word_bytes
    for y in range(height):
        struct.pack_into("<Q", out, offset_table_pos + y * 8, len(out))
        out.extend(struct.pack("<iI", y, scan_bytes))
        row_base = y * width * num_ch * word_bytes
        for cname in ch_names:
            cidx = ch_order[cname]
            for x in range(width):
                w_off = row_base + (x * num_ch + cidx) * word_bytes
                out.extend(interleaved_words[w_off : w_off + word_bytes])
    return bytes(out)


def decode_openexr_uncompressed(blob: bytes) -> tuple[int, int, tuple[str, ...], int, bytes]:
    """Clean-room single-part uncompressed scanline OpenEXR decoder preserving exact float bits."""
    if len(blob) < 8:
        raise ValueError("truncated OpenEXR header")
    magic, version = struct.unpack_from("<II", blob, 0)
    if magic != 20000630 or (version & 0xFF) != 2:
        raise ValueError("invalid OpenEXR header")
    pos = 8
    attrs: dict[str, tuple[str, bytes]] = {}
    while True:
        if pos >= len(blob):
            raise ValueError("truncated OpenEXR attribute header")
        if blob[pos] == 0:
            break
        n_end = blob.find(0, pos)
        if n_end < 0:
            raise ValueError("unterminated OpenEXR attribute name")
        name = blob[pos:n_end].decode("ascii")
        t_start = n_end + 1
        t_end = blob.find(0, t_start)
        if t_end < 0:
            raise ValueError("unterminated OpenEXR attribute type")
        atype = blob[t_start:t_end].decode("ascii")
        if t_end + 5 > len(blob):
            raise ValueError("truncated OpenEXR attribute length")
        alen = struct.unpack_from("<I", blob, t_end + 1)[0]
        v_start = t_end + 5
        if v_start + alen > len(blob):
            raise ValueError("truncated OpenEXR attribute value")
        attrs[name] = (atype, blob[v_start : v_start + alen])
        pos = v_start + alen
    pos += 1  # end-of-header null byte

    for req in ("compression", "dataWindow", "channels"):
        if req not in attrs:
            raise ValueError(f"missing required OpenEXR header attribute {req!r}")
    if attrs["compression"][1] != b"\x00":
        raise ValueError("pure-Python EXR oracle only decodes NO_COMPRESSION=0")
    if len(attrs["dataWindow"][1]) != 16:
        raise ValueError("invalid OpenEXR dataWindow")
    x0, y0, x1, y1 = struct.unpack("<iiii", attrs["dataWindow"][1])
    width = x1 - x0 + 1
    height = y1 - y0 + 1
    if width <= 0 or height <= 0:
        raise ValueError("invalid OpenEXR dimensions")

    cb = attrs["channels"][1]
    cp = 0
    exr_channels: list[tuple[str, int]] = []
    while cp < len(cb) and cb[cp] != 0:
        ce = cb.find(0, cp)
        if ce < 0 or ce + 17 > len(cb):
            raise ValueError("truncated OpenEXR channel definition")
        cname = cb[cp:ce].decode("ascii")
        ptype = struct.unpack_from("<I", cb, ce + 1)[0]
        exr_channels.append((cname, ptype))
        cp = ce + 17
    if cp >= len(cb) or cb[cp] != 0 or not exr_channels:
        raise ValueError("invalid OpenEXR channel list")
    pixel_type = exr_channels[0][1]
    if pixel_type not in (1, 2) or any(pt != pixel_type for _, pt in exr_channels):
        raise ValueError("unsupported OpenEXR pixel type")
    word_bytes = 2 if pixel_type == 1 else 4

    table_bytes = height * 8
    if pos + table_bytes > len(blob):
        raise ValueError("truncated OpenEXR scanline offset table")
    offsets = struct.unpack_from(f"<{height}Q", blob, pos)
    table_end = pos + table_bytes

    num_ch = len(exr_channels)
    expected = width * num_ch * word_bytes
    out = bytearray(width * height * num_ch * word_bytes)
    for y_idx, off in enumerate(offsets):
        if off < table_end or off + 8 > len(blob):
            raise ValueError("truncated OpenEXR scanline header")
        y_coord, dsize = struct.unpack_from("<iI", blob, off)
        if y_coord - y0 != y_idx or dsize != expected:
            raise ValueError("EXR scanline mismatch")
        s_off = off + 8
        if s_off + expected > len(blob):
            raise ValueError("truncated OpenEXR scanline payload")
        for cidx in range(num_ch):
            for x in range(width):
                w = blob[s_off : s_off + word_bytes]
                s_off += word_bytes
                dst = ((y_idx * width + x) * num_ch + cidx) * word_bytes
                out[dst : dst + word_bytes] = w
    return width, height, tuple(c for c, _ in exr_channels), pixel_type, bytes(out)


def _is_strict_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def verify_jpeg_coefficients_py(expected: dict[str, Any], actual: dict[str, Any]) -> tuple[bool, str]:
    """Verify exact JPEG DCT-domain equivalence with strict evidence completeness checks."""
    ew = expected.get("width")
    eh = expected.get("height")
    aw = actual.get("width")
    ah = actual.get("height")
    if not _is_strict_int(ew) or not _is_strict_int(eh) or ew <= 0 or eh <= 0:
        return False, "jpeg-invalid-evidence"
    if not _is_strict_int(aw) or not _is_strict_int(ah) or aw <= 0 or ah <= 0:
        return False, "jpeg-invalid-evidence"
    if (ew, eh) != (aw, ah):
        return False, "jpeg-dimensions"

    for k in ("progressive", "arithmetic_coding", "baseline", "component_count"):
        if k not in expected or k not in actual:
            return False, "jpeg-missing-evidence"
        if expected[k] != actual[k]:
            return False, "jpeg-coding-params"

    comp_count = expected["component_count"]
    if not _is_strict_int(comp_count) or comp_count <= 0 or comp_count > 4:
        return False, "jpeg-invalid-evidence"

    exp_comps = expected.get("components")
    act_comps = actual.get("components")
    if (
        not isinstance(exp_comps, (list, tuple))
        or not isinstance(act_comps, (list, tuple))
        or len(exp_comps) != comp_count
        or len(act_comps) != comp_count
    ):
        return False, "jpeg-missing-evidence"

    exp_qts = expected.get("quant_tables")
    act_qts = actual.get("quant_tables")
    if not isinstance(exp_qts, (list, tuple)) or not isinstance(act_qts, (list, tuple)):
        return False, "jpeg-missing-evidence"

    for idx in range(comp_count):
        sc = exp_comps[idx]
        dc = act_comps[idx]
        if not isinstance(sc, dict) or not isinstance(dc, dict):
            return False, "jpeg-invalid-evidence"
        for ck in (
            "component_id",
            "h_samp_factor",
            "v_samp_factor",
            "quant_table_idx",
            "dc_table_idx",
            "ac_table_idx",
            "blocks_wide",
            "blocks_high",
        ):
            if ck not in sc or ck not in dc:
                return False, "jpeg-missing-evidence"
            if sc[ck] != dc[ck]:
                return False, "jpeg-component-spec"

        bw = sc["blocks_wide"]
        bh = sc["blocks_high"]
        hs = sc["h_samp_factor"]
        vs = sc["v_samp_factor"]
        qt = sc["quant_table_idx"]
        if (
            not _is_strict_int(bw) or bw <= 0
            or not _is_strict_int(bh) or bh <= 0
            or not _is_strict_int(hs) or hs <= 0
            or not _is_strict_int(vs) or vs <= 0
        ):
            return False, "jpeg-invalid-evidence"
        if not _is_strict_int(qt) or qt < 0 or qt >= 4:
            return False, "jpeg-invalid-quant-table"
        if qt >= len(exp_qts) or qt >= len(act_qts):
            return False, "jpeg-missing-evidence"
        eqt = exp_qts[qt]
        aqt = act_qts[qt]
        if (
            not isinstance(eqt, (list, tuple))
            or not isinstance(aqt, (list, tuple))
            or len(eqt) != 64
            or len(aqt) != 64
        ):
            return False, "jpeg-invalid-quant-table"
        if list(eqt) != list(aqt):
            return False, "jpeg-quant-table"

        expected_coeffs = bw * bh * 64
        sdct = sc.get("dct_blocks")
        ddct = dc.get("dct_blocks")
        if (
            not isinstance(sdct, (list, tuple))
            or not isinstance(ddct, (list, tuple))
            or len(sdct) != expected_coeffs
            or len(ddct) != expected_coeffs
        ):
            return False, "jpeg-missing-evidence"
        if list(sdct) != list(ddct):
            return False, "jpeg-dct-coeff"
    return True, "none"


def verify_openexr_bits_py(expected_words: list[int], actual_words: list[int]) -> tuple[bool, str]:
    """Enforce exact raw IEEE-754 integer bit-pattern identity (-0.0 vs +0.0, NaN payloads)."""
    if len(expected_words) != len(actual_words):
        return False, "dimensions"
    for ew, aw in zip(expected_words, actual_words):
        if ew != aw:
            return False, "float-bit-pattern"
    return True, "none"


def evaluate_raw_conversion_contract(
    source_format: str,
    target_format: str,
    render_params: dict[str, Any],
    rendered_source_frames: list[dict[str, Any]],
    target_decoded_frames: list[dict[str, Any]],
    claims_roundtrip_to_raw: bool = False,
) -> tuple[bool, str]:
    """Enforce deterministic import-only LibRaw conversion contract."""
    if source_format not in ("camera-raw", "dng"):
        return False, "not-a-raw-source"
    if target_format in ("camera-raw", "dng") or claims_roundtrip_to_raw:
        return False, "raw-is-import-only"
    for key in LIBRAW_RENDERING_CONTRACT["required_fixed_parameters"]:
        if key not in render_params:
            return False, f"missing-fixed-param:{key}"
    if render_params.get("auto_brighten", False):
        return False, "auto-brighten-forbidden"
    if frames_hash_sha256(rendered_source_frames) != frames_hash_sha256(target_decoded_frames):
        return False, "sample-value"
    return True, "ok"


class _WindowsProcessJob:
    """Win32 Job Object with KILL_ON_JOB_CLOSE so child trees die together."""

    def __init__(self) -> None:
        import ctypes
        from ctypes import wintypes

        class _BasicLimitInfo(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class _IoCounters(ctypes.Structure):
            _fields_ = [("ReadOperationCount", ctypes.c_uint64),
                        ("WriteOperationCount", ctypes.c_uint64),
                        ("OtherOperationCount", ctypes.c_uint64),
                        ("ReadTransferCount", ctypes.c_uint64),
                        ("WriteTransferCount", ctypes.c_uint64),
                        ("OtherTransferCount", ctypes.c_uint64)]

        class _ExtendedLimitInfo(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", _BasicLimitInfo),
                ("IoInfo", _IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        self.ctypes = ctypes
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        self.kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD
        ]
        self.kernel32.SetInformationJobObject.restype = wintypes.BOOL
        self.kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        self.kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.kernel32.TerminateJobObject.restype = wintypes.BOOL
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.ntdll = ctypes.WinDLL("ntdll")
        self.ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
        self.ntdll.NtResumeProcess.restype = ctypes.c_long
        self.handle = self.kernel32.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "CreateJobObjectW failed")
        info = _ExtendedLimitInfo()
        info.BasicLimitInformation.LimitFlags = 0x00002000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel32.SetInformationJobObject(
            self.handle, 9, ctypes.byref(info), ctypes.sizeof(info)
        ):
            err = ctypes.get_last_error()
            self.kernel32.CloseHandle(self.handle)
            self.handle = None
            raise OSError(err, "SetInformationJobObject failed")

    def adopt_and_resume(self, proc: subprocess.Popen[Any]) -> None:
        handle = int(getattr(proc, "_handle"))
        if not self.kernel32.AssignProcessToJobObject(self.handle, handle):
            raise OSError(self.ctypes.get_last_error(), "AssignProcessToJobObject failed")
        status = self.ntdll.NtResumeProcess(handle)
        if status < 0:
            raise OSError(status, "NtResumeProcess failed")

    def end(self) -> None:
        if self.handle:
            self.kernel32.TerminateJobObject(self.handle, 1)
            self.kernel32.CloseHandle(self.handle)
            self.handle = None


def _posix_ancestors(start_pid: int) -> set[int]:
    ancestors: set[int] = set()
    cur = start_pid
    while cur > 1 and cur not in ancestors:
        ancestors.add(cur)
        try:
            stat_txt = Path(f"/proc/{cur}/stat").read_text(encoding="ascii", errors="replace")
        except OSError:
            # Ancestor exited or denied /proc access mid-walk; stop climbing.
            break
        rp = stat_txt.rfind(")")
        if rp < 0:
            break
        fields = stat_txt[rp + 2 :].split()
        if len(fields) < 2:
            break
        try:
            cur = int(fields[1])
        except ValueError:
            break
    return ancestors


def _collect_posix_subtree(root_pid: int, tree_token: str | None) -> tuple[set[int], set[int]]:
    """Collect UID-owned PIDs and PGIDs descending from root_pid or carrying tree_token."""
    my_pid = os.getpid()
    my_uid = os.getuid()
    my_pgid = os.getpgrp()
    ancestors = _posix_ancestors(my_pid)
    token_bytes = tree_token.encode("ascii") if tree_token else None

    proc_dir = Path("/proc")
    if not proc_dir.is_dir():
        pgids = {root_pid} if root_pid > 1 and root_pid != my_pgid else set()
        pids = {root_pid} if root_pid > 1 and root_pid != my_pid else set()
        return pids, pgids

    children_by_ppid: dict[int, list[int]] = {}
    pgid_by_pid: dict[int, int] = {}
    tagged_pids: set[int] = set()

    for entry in proc_dir.iterdir():
        name = entry.name
        if not name.isdigit():
            continue
        pid = int(name)
        if pid <= 1 or pid == my_pid or pid in ancestors:
            continue
        try:
            st = entry.stat()
            if st.st_uid != my_uid:
                continue
            stat_txt = (entry / "stat").read_text(encoding="ascii", errors="replace")
        except OSError:
            # Process exited mid-/proc scan or belongs to another UID/unreadable entry.
            continue
        rp = stat_txt.rfind(")")
        if rp < 0:
            continue
        fields = stat_txt[rp + 2 :].split()
        if len(fields) < 3:
            continue
        try:
            ppid = int(fields[1])
            pgid = int(fields[2])
        except ValueError:
            continue
        children_by_ppid.setdefault(ppid, []).append(pid)
        pgid_by_pid[pid] = pgid
        if token_bytes is not None:
            try:
                raw_env = (entry / "environ").read_bytes()
            except OSError:
                # Process may exit or restrict environ reads (e.g. non-dumpable); PPID link is recorded above.
                continue
            for item in raw_env.split(b"\x00"):
                if item.startswith(b"RFC_PROCESS_TREE_TAGS="):
                    vals = item[len(b"RFC_PROCESS_TREE_TAGS="):].split(b",")
                    if token_bytes in vals:
                        tagged_pids.add(pid)
                    break

    seeds: list[int] = []
    if root_pid > 1 and root_pid != my_pid and root_pid not in ancestors:
        seeds.append(root_pid)
    for pid in sorted(tagged_pids):
        if pid not in seeds:
            seeds.append(pid)

    found_pids: set[int] = set()
    queue = list(seeds)
    while queue:
        cur = queue.pop()
        if cur in found_pids or cur <= 1 or cur == my_pid or cur in ancestors:
            continue
        found_pids.add(cur)
        for ch in children_by_ppid.get(cur, ()):
            if ch not in found_pids:
                queue.append(ch)

    found_pgids: set[int] = set()
    for pid in found_pids:
        pgid = pgid_by_pid.get(pid, pid)
        if pgid > 1 and pgid != my_pgid and pgid != my_pid:
            found_pgids.add(pgid)
    if root_pid > 1 and root_pid != my_pgid and root_pid != my_pid:
        found_pgids.add(root_pid)
    return found_pids, found_pgids


def _kill_posix_tree(root_pid: int, tree_token: str | None) -> None:
    stopped: set[int] = set()
    all_pids: set[int] = set()
    all_pgids: set[int] = set()
    # Freeze newly discovered members first so a dying parent cannot outrun teardown via fork.
    for _ in range(5):
        pids, pgids = _collect_posix_subtree(root_pid, tree_token)
        all_pids.update(pids)
        all_pgids.update(pgids)
        new_pids = pids - stopped
        if not new_pids:
            break
        for pid in sorted(new_pids):
            try:
                os.kill(pid, signal.SIGSTOP)
            except OSError:
                # Target may have exited between /proc discovery and signaling.
                pass
            stopped.add(pid)

    pids, pgids = _collect_posix_subtree(root_pid, tree_token)
    all_pids.update(pids)
    all_pgids.update(pgids)

    for pgid in sorted(all_pgids, reverse=True):
        try:
            os.killpg(pgid, signal.SIGKILL)
        except OSError:
            # Group may already be empty or owned outside UID after exec/exit.
            pass
    for pid in sorted(all_pids, reverse=True):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            # PID may already have terminated when its PGID was killed above.
            pass


def _kill_process_tree(
    proc: subprocess.Popen[Any],
    job: _WindowsProcessJob | None = None,
    tree_token: str | None = None,
) -> None:
    if job is not None:
        job.end()
        return
    if os.name == "nt":
        if proc.poll() is None:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                timeout=15,
                check=False,
            )
    else:
        _kill_posix_tree(proc.pid, tree_token)


def run_process_tree(
    cmd: list[str] | tuple[str, ...],
    *,
    cwd: str | Path | None = None,
    env: dict[str, str] | None = None,
    timeout: float = 60.0,
    capture_output: bool = False,
    text: bool = True,
) -> subprocess.CompletedProcess[Any]:
    """Run a child command and terminate its entire descendant tree on completion or timeout."""
    argv = [str(part) for part in cmd]
    pipes = subprocess.PIPE if capture_output else None
    tree_token = f"{os.getpid()}-{os.urandom(8).hex()}"
    child_env = dict(os.environ if env is None else env)
    prev_tags = child_env.get("RFC_PROCESS_TREE_TAGS", "")
    child_env["RFC_PROCESS_TREE_TAGS"] = f"{prev_tags},{tree_token}" if prev_tags else tree_token
    job: _WindowsProcessJob | None = None
    proc: subprocess.Popen[Any] | None = None
    try:
        if os.name == "nt":
            job = _WindowsProcessJob()
            flags = 0x00000004 | getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
            proc = subprocess.Popen(
                argv,
                cwd=cwd,
                env=child_env,
                stdin=subprocess.DEVNULL,
                stdout=pipes,
                stderr=pipes,
                text=text,
                creationflags=flags,
            )
            try:
                job.adopt_and_resume(proc)
            except OSError:
                proc.kill()
                proc.wait()
                job.end()
                job = None
                fb_flags = (
                    getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x00000200)
                    | getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
                )
                proc = subprocess.Popen(
                    argv,
                    cwd=cwd,
                    env=child_env,
                    stdin=subprocess.DEVNULL,
                    stdout=pipes,
                    stderr=pipes,
                    text=text,
                    creationflags=fb_flags,
                )
        else:
            proc = subprocess.Popen(
                argv,
                cwd=cwd,
                env=child_env,
                stdin=subprocess.DEVNULL,
                stdout=pipes,
                stderr=pipes,
                text=text,
                start_new_session=True,
            )
        try:
            out, err = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            _kill_process_tree(proc, job, tree_token)
            out_tail, err_tail = proc.communicate()
            raise subprocess.TimeoutExpired(argv, timeout, output=out_tail, stderr=err_tail) from exc
        return subprocess.CompletedProcess(argv, proc.returncode, out, err)
    finally:
        if proc is not None:
            _kill_process_tree(proc, job, tree_token)
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        elif job is not None:
            job.end()


def _probe_ffmpeg_decoder(
    decoder_names: tuple[str, ...],
    *,
    label: str,
    independent: bool,
    reason_found: str,
    reason_missing: str,
) -> dict[str, Any]:
    candidates: list[str] = []
    for name in ("ffmpeg", "ffmpeg.exe"):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    if os.name != "nt":
        win_winget = Path(
            "/mnt/c/Users/DerickPayne/AppData/Local/Microsoft/WinGet/Links/ffmpeg.exe"
        )
        if win_winget.is_file() and str(win_winget) not in candidates:
            candidates.append(str(win_winget))
    for exe in candidates:
        try:
            res = run_process_tree(
                [exe, "-hide_banner", "-decoders"],
                timeout=10.0,
                capture_output=True,
                text=True,
            )
        except Exception:
            continue
        if res.returncode == 0 and res.stdout:
            in_table = False
            for line in res.stdout.splitlines():
                if line.strip() == "------":
                    in_table = True
                    continue
                if not in_table:
                    continue
                parts = line.split()
                if (
                    len(parts) >= 2
                    and len(parts[0]) == 6
                    and parts[0][0] in ("V", "A", "S")
                    and parts[1] in decoder_names
                ):
                    return {
                        "available": True,
                        "executable": exe,
                        "decoder": label,
                        "codec_name": parts[1],
                        "independent": independent,
                        "reason": reason_found,
                    }
    return {
        "available": False,
        "executable": "",
        "decoder": "none",
        "codec_name": "",
        "independent": False,
        "reason": reason_missing,
    }


def _decode_via_ffmpeg(
    info: dict[str, Any],
    path: str | os.PathLike[str],
    pix_fmt: str,
    label: str,
) -> bytes | None:
    if not info["available"]:
        return None
    exe = info["executable"]
    target = os.fspath(path)
    if os.name != "nt" and exe.lower().endswith(".exe"):
        wp = run_process_tree(
            ["wslpath", "-w", target],
            timeout=5.0,
            capture_output=True,
            text=True,
        )
        if wp.returncode != 0 or not wp.stdout.strip():
            return None
        target = wp.stdout.strip()
    res = run_process_tree(
        [exe, "-v", "error", "-i", target, "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"],
        timeout=30.0,
        capture_output=True,
        text=False,
    )
    if res.returncode != 0:
        raise RuntimeError(
            f"ffmpeg {label} decode failed ({res.returncode}): {res.stderr.decode(errors='replace')}"
        )
    return res.stdout


def probe_ffmpeg_webp_decoder() -> dict[str, Any]:
    """Locate FFmpeg with clean-room libavcodec native webp decoder if present."""
    return _probe_ffmpeg_decoder(
        ("webp",),
        label="libavcodec-webp",
        independent=True,
        reason_found="FFmpeg built-in libavcodec clean-room WebP decoder",
        reason_missing="no ffmpeg with clean-room libavcodec webp decoder found on host",
    )


def decode_webp_ffmpeg(path: str | os.PathLike[str], pix_fmt: str = "rgba") -> bytes | None:
    """Decode a still WebP image to tightly packed raw rgb24/rgba via FFmpeg."""
    return _decode_via_ffmpeg(probe_ffmpeg_webp_decoder(), path, pix_fmt, "webp")


def probe_ffmpeg_jxl_decoder() -> dict[str, Any]:
    """Locate FFmpeg with libjxl-backed jpegxl decoder if present."""
    return _probe_ffmpeg_decoder(
        ("libjxl", "jpegxl"),
        label="ffmpeg-libjxl",
        independent=False,
        reason_found="FFmpeg libjxl decoder (shared upstream library family)",
        reason_missing="no ffmpeg with libjxl decoder found on host",
    )


def decode_jxl_ffmpeg(path: str | os.PathLike[str], pix_fmt: str = "rgb24") -> bytes | None:
    """Decode a still JPEG XL image to raw pixels via FFmpeg libjxl."""
    return _decode_via_ffmpeg(probe_ffmpeg_jxl_decoder(), path, pix_fmt, "jxl")


def probe_ffmpeg_avif_decoder() -> dict[str, Any]:
    """Locate FFmpeg with clean-room VideoLAN libdav1d AV1 decoder if present."""
    return _probe_ffmpeg_decoder(
        ("libdav1d",),
        label="ffmpeg-libdav1d",
        independent=True,
        reason_found="FFmpeg libavformat + VideoLAN libdav1d clean-room AV1 decoder",
        reason_missing="no ffmpeg with clean-room libdav1d decoder found on host",
    )

# Linux LD_PRELOAD heap high-water-mark interposer (malloc/calloc/realloc/
# posix_memalign/free with malloc_usable_size accounting); the process writes
# its peak live heap bytes to $RFC_HWM_OUT at exit.
HWM_INTERPOSER_C = """#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void (*real_free)(void *) = NULL;
static int (*real_posix_memalign)(void **, size_t, size_t) = NULL;

static char boot_buf[32768];
static size_t boot_used = 0;
static volatile int resolving = 0;
static __thread int in_dlsym = 0;
static uint64_t live_bytes = 0;
static uint64_t hwm_bytes = 0;

static int is_boot_ptr(const void *p) {
    uintptr_t a = (uintptr_t)p;
    uintptr_t lo = (uintptr_t)boot_buf;
    return a >= lo && a < lo + sizeof(boot_buf);
}

static void update_hwm(uint64_t cur) {
    uint64_t prev = __atomic_load_n(&hwm_bytes, __ATOMIC_RELAXED);
    while (cur > prev &&
           !__atomic_compare_exchange_n(&hwm_bytes, &prev, cur, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void add_live(size_t sz) {
    uint64_t cur = __atomic_add_fetch(&live_bytes, (uint64_t)sz, __ATOMIC_RELAXED);
    update_hwm(cur);
}

static void sub_live(size_t sz) {
    uint64_t prev = __atomic_load_n(&live_bytes, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t next = (prev >= (uint64_t)sz) ? (prev - (uint64_t)sz) : 0u;
        if (__atomic_compare_exchange_n(&live_bytes, &prev, next, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }
}

static void ensure_init(void) {
    if (__atomic_load_n(&real_free, __ATOMIC_ACQUIRE) != NULL) return;
    if (in_dlsym) return;
    while (__atomic_exchange_n(&resolving, 1, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&real_free, __ATOMIC_ACQUIRE) != NULL) return;
    }
    if (__atomic_load_n(&real_free, __ATOMIC_RELAXED) == NULL) {
        in_dlsym = 1;
        void *(*m)(size_t) = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
        void *(*c)(size_t, size_t) = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
        void *(*r)(void *, size_t) = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
        int (*pm)(void **, size_t, size_t) =
            (int (*)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
        void (*f)(void *) = (void (*)(void *))dlsym(RTLD_NEXT, "free");
        in_dlsym = 0;
        real_malloc = m;
        real_calloc = c;
        real_realloc = r;
        real_posix_memalign = pm;
        __atomic_store_n(&real_free, f, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&resolving, 0, __ATOMIC_RELEASE);
}

void *malloc(size_t sz) {
    ensure_init();
    if (!real_malloc || in_dlsym) {
        size_t a = (sz + 15u) & ~(size_t)15u;
        size_t off = __atomic_fetch_add(&boot_used, a ? a : 16u, __ATOMIC_RELAXED);
        return (off + a <= sizeof(boot_buf)) ? (boot_buf + off) : NULL;
    }
    void *p = real_malloc(sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
}

void *calloc(size_t n, size_t sz) {
    ensure_init();
    if (!real_calloc || in_dlsym) {
        size_t tot = n * sz;
        size_t a = (tot + 15u) & ~(size_t)15u;
        size_t off = __atomic_fetch_add(&boot_used, a ? a : 16u, __ATOMIC_RELAXED);
        if (off + a <= sizeof(boot_buf)) {
            memset(boot_buf + off, 0, tot);
            return boot_buf + off;
        }
        return NULL;
    }
    void *p = real_calloc(n, sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
}

void *realloc(void *p, size_t sz) {
    ensure_init();
    if (!real_realloc || is_boot_ptr(p)) {
        void *np = malloc(sz);
        if (np && p && is_boot_ptr(p)) memcpy(np, p, sz < 1024u ? sz : 1024u);
        return np;
    }
    size_t old_sz = p ? malloc_usable_size(p) : 0u;
    void *np = real_realloc(p, sz);
    if (np) {
        if (old_sz) sub_live(old_sz);
        add_live(malloc_usable_size(np));
    } else if (sz == 0u && old_sz) {
        sub_live(old_sz);
    }
    return np;
}

int posix_memalign(void **memptr, size_t align, size_t sz) {
    ensure_init();
    if (!real_posix_memalign) return 12;
    int rc = real_posix_memalign(memptr, align, sz);
    if (rc == 0 && *memptr) add_live(malloc_usable_size(*memptr));
    return rc;
}

void free(void *p) {
    if (!p || is_boot_ptr(p)) return;
    ensure_init();
    if (!real_free) return;
    size_t old_sz = malloc_usable_size(p);
    real_free(p);
    if (old_sz) sub_live(old_sz);
}

__attribute__((destructor)) static void dump_hwm(void) {
    const char *path = getenv("RFC_HWM_OUT");
    if (!path || !*path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%llu\\n",
                     (unsigned long long)__atomic_load_n(&hwm_bytes, __ATOMIC_RELAXED));
    if (n > 0) (void)write(fd, buf, (size_t)n);
    close(fd);
}
"""


def build_hwm_interposer(work_dir: Path) -> Path:
    """Compile HWM_INTERPOSER_C into work_dir and return the shared object."""
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        raise AssertionError("Linux C compiler unavailable for LD_PRELOAD HWM interposer")
    so_c = work_dir / "rfc_hwm_interpose.c"
    so_path = work_dir / "librfc_hwm_interpose.so"
    so_c.write_text(HWM_INTERPOSER_C, encoding="utf-8")
    res = run_process_tree(
        [cc, "-shared", "-fPIC", "-O2", str(so_c), "-o", str(so_path), "-ldl", "-pthread"],
        cwd=work_dir,
        capture_output=True,
        text=True,
        timeout=60.0,
    )
    if res.returncode != 0:
        raise AssertionError(f"compile interposer failed:\n{res.stderr}")
    return so_path
