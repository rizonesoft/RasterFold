"""Converter exactness, oracle provenance, and format contract tests."""
from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import reference as ref


class ExactnessReferenceTests(unittest.TestCase):
    def test_provenance_table_complete_and_honest(self) -> None:
        by_name = ref.validate_provenance_table()
        self.assertEqual(len(by_name), 19)
        for pending in (
            "apng",
            "webp",
            "jpeg-xl",
            "avif",
            "gif",
            "tiff",
            "heif",
            "heic",
            "jpeg-2000",
        ):
            self.assertEqual(by_name[pending]["coverage_state"], "provenance-only-pending-adapter")
        for executable in (
            "rasterfold",
            "bmp",
            "png",
            "pnm",
            "pam",
            "qoi",
            "openexr",
            "jpeg",
            "camera-raw",
            "dng",
        ):
            self.assertEqual(by_name[executable]["coverage_state"], "executable-in-d10-t01-s4")
        self.assertEqual(by_name["rasterfold"]["reference_decoder_kind"], "independent-oracle")
        self.assertFalse(by_name["rasterfold"]["shares_c_library_with_converter"])
        self.assertEqual(by_name["webp"]["reference_decoder_kind"], "shared-upstream-library")
        self.assertFalse(by_name["webp"]["claims_independent_decoder"])
        self.assertTrue(by_name["webp"]["shares_c_library_with_converter"])

    def test_pillow_shared_library_honesty(self) -> None:
        info = ref.inspect_pillow_codec_provenance()
        self.assertIn("codecs", info)
        for feat in ("webp", "libtiff", "jpg", "openjp2", "zlib"):
            row = info["codecs"][feat]
            self.assertEqual(row["classification"], "shared-upstream-library")
            self.assertFalse(row["claims_independent_decoder"])
            self.assertTrue(row["c_library_family"])

    def test_bmp_pnm_pam_qoi_png_independent_decoders_and_mutations(self) -> None:
        w, h = 5, 4
        rgb8 = bytes(((x * 47 + y * 29 + c * 73) & 0xFF) for y in range(h) for x in range(w) for c in range(3))
        rgba8 = bytes(
            (x * 31) & 0xFF if c < 3 else (0 if (x == 2 and y == 1) else 200)
            for y in range(h)
            for x in range(w)
            for c in range(4)
        )

        # BMP 24-bit bottom-up and 32-bit top-down
        bmp24 = ref.encode_bmp(w, h, rgb8, channels=3, topdown=False)
        self.assertEqual(ref.decode_bmp(bmp24), (w, h, 3, rgb8))
        bmp32 = ref.encode_bmp(w, h, rgba8, channels=4, topdown=True)
        self.assertEqual(ref.decode_bmp(bmp32), (w, h, 4, rgba8))
        # Row stride padding byte mutation must not alter active pixel output; active byte mutation must.
        pad_bmp = bytearray(bmp24)
        pad_bmp[-1] ^= 0x01
        self.assertEqual(ref.decode_bmp(bytes(pad_bmp))[3], rgb8)
        bad_bmp = bytearray(bmp24)
        bad_bmp[-2] ^= 0x01
        self.assertNotEqual(ref.decode_bmp(bytes(bad_bmp))[3], rgb8)

        # PNM P5 gray16 and P6 rgb16 big-endian -> host LE round-trip
        rgb16 = b"".join(struct.pack("<H", (i * 257) & 0xFFFF) for i in range(w * h * 3))
        pnm = ref.encode_pnm(w, h, 3, 65535, rgb16)
        self.assertEqual(ref.decode_pnm(pnm), (w, h, 3, 65535, rgb16))

        # PAM P7 RGB_ALPHA
        pam = ref.encode_pam(w, h, 4, 255, "RGB_ALPHA", rgba8)
        self.assertEqual(ref.decode_pam(pam), (w, h, 4, 255, "RGB_ALPHA", rgba8))

        # QOI RGB8 & RGBA8
        qoi3 = ref.encode_qoi(w, h, 3, rgb8)
        self.assertEqual(ref.decode_qoi(qoi3), (w, h, 3, rgb8))
        qoi4 = ref.encode_qoi(w, h, 4, rgba8)
        self.assertEqual(ref.decode_qoi(qoi4), (w, h, 4, rgba8))

        # Pure-Python PNG RGBA8 & RGB16
        png4 = ref.encode_png(w, h, 6, 8, rgba8)
        self.assertEqual(ref.decode_png(png4), (w, h, 4, 8, rgba8))
        png16 = ref.encode_png(w, h, 2, 16, rgb16)
        self.assertEqual(ref.decode_png(png16), (w, h, 3, 16, rgb16))

    def test_openexr_uncompressed_bit_pattern_preservation(self) -> None:
        w, h = 2, 2
        # HALF words: +0.0 (0x0000), -0.0 (0x8000), qNaN payload A (0x7e01), 1.0 (0x3c00)
        words_a = [
            0x0000, 0x3c00, 0x7e01,
            0x8000, 0x4000, 0x3555,
            0x3800, 0x8000, 0x7e02,
            0x0000, 0x0001, 0x3c00,
        ]
        raw_a = b"".join(struct.pack("<H", v) for v in words_a)
        exr = ref.encode_openexr_uncompressed(w, h, ("B", "G", "R"), 1, raw_a)
        dw, dh, dch, dpt, dec = ref.decode_openexr_uncompressed(exr)
        self.assertEqual((dw, dh, dch, dpt), (w, h, ("B", "G", "R"), 1))
        self.assertEqual(dec, raw_a)

        # Mutate -0.0 -> +0.0 and NaN payload -> OPENEXR_BIT_PATTERN_CONTRACT must reject
        words_b = list(words_a)
        words_b[3] = 0x0000
        ok, kind = ref.verify_openexr_bits_py(words_a, words_b)
        self.assertFalse(ok)
        self.assertEqual(kind, "float-bit-pattern")

        words_c = list(words_a)
        words_c[2] = 0x7e09
        ok, kind = ref.verify_openexr_bits_py(words_a, words_c)
        self.assertFalse(ok)
        self.assertEqual(kind, "float-bit-pattern")

        # Also test 32-bit FLOAT
        f_words = [0x80000000, 0x3f800000, 0x7fc00001] * (w * h)
        f_raw = b"".join(struct.pack("<I", v) for v in f_words)
        exr_f = ref.encode_openexr_uncompressed(w, h, ("B", "G", "R"), 2, f_raw)
        dw, dh, dch, dpt, dec_f = ref.decode_openexr_uncompressed(exr_f)
        self.assertEqual((dw, dh, dch, dpt), (w, h, ("B", "G", "R"), 2))
        self.assertEqual(dec_f, f_raw)

    def test_jpeg_and_libraw_contracts(self) -> None:
        comp = {
            "component_id": 1,
            "h_samp_factor": 2,
            "v_samp_factor": 2,
            "quant_table_idx": 0,
            "dc_table_idx": 0,
            "ac_table_idx": 0,
            "blocks_wide": 1,
            "blocks_high": 1,
            "dct_blocks": [0] * 64,
        }
        src_c = {
            "width": 8,
            "height": 8,
            "progressive": 0,
            "arithmetic_coding": 0,
            "baseline": 1,
            "component_count": 1,
            "components": [dict(comp)],
            "quant_tables": [[16] * 64],
        }
        dst_c = {
            "width": 8,
            "height": 8,
            "progressive": 0,
            "arithmetic_coding": 0,
            "baseline": 1,
            "component_count": 1,
            "components": [dict(comp)],
            "quant_tables": [[16] * 64],
        }
        self.assertEqual(ref.verify_jpeg_coefficients_py(src_c, dst_c), (True, "none"))
        dst_c["quant_tables"][0][0] = 17
        self.assertEqual(ref.verify_jpeg_coefficients_py(src_c, dst_c), (False, "jpeg-quant-table"))

        # LibRaw import-only fixed-parameter & target pixel-exact contract
        pix = bytes(range(12))
        frame = {
            "width": 2,
            "height": 2,
            "sample_format": ref.SAMPLE_RGB8,
            "alpha": ref.ALPHA_NONE,
            "bits_per_sample": 8,
            "duration_ms": 0,
            "data": pix,
        }
        params = {"color_space": "srgb", "white_balance": "camera", "highlight_mode": "clip", "output_bps": 16, "auto_brighten": False}
        self.assertEqual(
            ref.evaluate_raw_conversion_contract("dng", "png", params, [frame], [frame]),
            (True, "ok"),
        )
        self.assertEqual(
            ref.evaluate_raw_conversion_contract("dng", "dng", params, [frame], [frame]),
            (False, "raw-is-import-only"),
        )
        bad_params = dict(params, auto_brighten=True)
        self.assertEqual(
            ref.evaluate_raw_conversion_contract("camera-raw", "rasterfold", bad_params, [frame], [frame]),
            (False, "auto-brighten-forbidden"),
        )

    def test_fold_roundtrip_oracle_and_digest_parity(self) -> None:
        exe = os.environ.get("RFC_TEST_EXACTNESS_EXE")
        if not exe:
            if os.environ.get("RFC_REQUIRE_EXACTNESS_EXE") == "1":
                self.fail("RFC_TEST_EXACTNESS_EXE is required when RFC_REQUIRE_EXACTNESS_EXE=1")
            self.skipTest("RFC_TEST_EXACTNESS_EXE not set")

        with tempfile.TemporaryDirectory() as tmp:
            fold_path = Path(tmp) / "out.fold"
            hash_path = Path(tmp) / "out.sha256"
            res = subprocess.run(
                [exe, "--fold-roundtrip", str(fold_path), str(hash_path)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(res.returncode, 0, msg=res.stderr)
            blob = fold_path.read_bytes()
            expected_hex = hash_path.read_text(encoding="utf-8").strip()

            # Independent spec oracle tests/rfi_ref.py decodes the C-encoded .fold bytes.
            w, h, rgb = ref.rfi_ref.decode_image(blob)
            py_digest = ref.frames_hash_sha256(
                [
                    {
                        "width": w,
                        "height": h,
                        "sample_format": ref.SAMPLE_RGB8,
                        "alpha": ref.ALPHA_NONE,
                        "bits_per_sample": 8,
                        "duration_ms": 0,
                        "data": rgb,
                    }
                ]
            ).hex()
            self.assertEqual(py_digest, expected_hex)

            # Mutating 1 payload byte in .fold must be rejected by independent spec oracle.
            tampered = bytearray(blob)
            tampered[-1] ^= 0x01
            with self.assertRaises(ref.rfi_ref.FormatError):
                ref.rfi_ref.decode_image(bytes(tampered))


if __name__ == "__main__":
    unittest.main()
