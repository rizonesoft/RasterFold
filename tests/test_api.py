"""Public ABI, ownership, concurrency, fidelity and native consumer tests."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import ctypes as C
import mmap
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

import rfi_ref
from test_correctness import KAT_FOLD, KAT_FOLD3, KAT_RGB, KAT_W, KAT_H, KAT_PAL, KAT_PAL2, KAT_PAL2_3, KAT_PAL_RGB, KAT_PAL_W, KAT_PAL_H

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'windows-x64' if os.name == 'nt' else 'linux-x64'
LIB = Path(os.environ.get('RF_TEST_LIB', ROOT / 'bin' / TARGET / ('bin/rasterfold.dll' if os.name == 'nt' else 'lib/librasterfold.so')))


class Options(C.Structure):
    _fields_ = [('size', C.c_uint32), ('threads', C.c_uint32), ('reserved', C.c_uint32 * 4)]


class Pixels(C.Structure):
    _fields_ = [('width', C.c_uint32), ('height', C.c_uint32), ('stride', C.c_size_t),
                ('layout', C.c_uint32), ('data', C.c_void_p), ('size', C.c_size_t)]


class Buffer(C.Structure):
    _fields_ = [('data', C.c_void_p), ('size', C.c_size_t)]


class Error(C.Structure):
    _fields_ = [('code', C.c_int), ('message', C.c_char_p)]


class Info(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('width', 'height', 'method', 'payload_size', 'payload_crc', 'header_crc')] + [('stream_size', C.c_uint32 * 3)]


def load(path=LIB):
    lib = C.CDLL(str(path))
    lib.rf_version.argtypes = []; lib.rf_version.restype = C.c_uint32
    lib.rf_encode_image.argtypes = [C.POINTER(Pixels), C.POINTER(Options), C.POINTER(Buffer), C.POINTER(Error)]
    lib.rf_decode_image.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Options), C.POINTER(Pixels), C.POINTER(Error)]
    lib.rf_inspect.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Info), C.POINTER(Error)]
    lib.rf_free.argtypes = [C.c_void_p]; lib.rf_free.restype = None
    return lib


LIBRARY = load()


def encode(w, h, rgb, threads=1, stride=None, prefix=False):
    raw = C.create_string_buffer(rgb)
    pix = Pixels(w, h, stride or w * 3, 1, C.addressof(raw), len(rgb))
    opt = Options(8 if prefix else C.sizeof(Options), threads)
    out, err = Buffer(), Error(99, b'stale')
    rc = LIBRARY.rf_encode_image(C.byref(pix), C.byref(opt), C.byref(out), C.byref(err))
    if rc:
        raise AssertionError((rc, err.code, err.message))
    try:
        assert err.code == 0 and err.message is None
        return C.string_at(out.data, out.size)
    finally:
        LIBRARY.rf_free(out.data)


class Api(unittest.TestCase):
    def test_00_concurrent_first_initialization(self):
        # First encoding calls race the predictor's once-only table setup.
        with ThreadPoolExecutor(max_workers=4) as pool:
            result = list(pool.map(lambda _: encode(KAT_W, KAT_H, KAT_RGB), range(4)))
        self.assertEqual(result, [KAT_FOLD3] * 4)

    def test_concurrent_encode_error_isolation(self):
        # Concurrent encodes keep per-call errors: a bad layout, an invalid
        # thread count, and a successful encode start together and must not
        # contaminate each other. Error objects are read only after every
        # worker has finished, so a shared mutable message buffer would fail.
        import threading
        barrier = threading.Barrier(3)
        retained = {}

        def attempt(kind):
            raw = C.create_string_buffer(KAT_RGB)
            pix = Pixels(KAT_W, KAT_H, KAT_W * 3, 99 if kind == 'layout' else 1,
                         C.addressof(raw), len(KAT_RGB))
            opt = Options(C.sizeof(Options), 42 if kind == 'threads' else 1)
            out, err = Buffer(), Error(99, b'stale')
            barrier.wait(timeout=30)
            rc = LIBRARY.rf_encode_image(C.byref(pix), C.byref(opt), C.byref(out), C.byref(err))
            retained[kind] = (rc, err, out)

        with ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(attempt, ('layout', 'threads', 'good')))
        try:
            layout_rc, layout_err, _ = retained['layout']
            threads_rc, threads_err, _ = retained['threads']
            good_rc, good_err, good_out = retained['good']
            self.assertEqual((layout_rc, layout_err.code), (3, 3))    # RF_ERR_UNSUPPORTED
            self.assertEqual((threads_rc, threads_err.code), (1, 1))  # RF_ERR_USAGE
            self.assertEqual((good_rc, good_err.code), (0, 0))
            self.assertIn(b'layout', layout_err.message)
            self.assertIn(b'threads', threads_err.message)
            self.assertIsNone(good_err.message)
            self.assertEqual(C.string_at(good_out.data, good_out.size), KAT_FOLD3)
        finally:
            for _, _, out in retained.values():
                if out.data:
                    LIBRARY.rf_free(out.data)

    def test_version(self):
        self.assertEqual(LIBRARY.rf_version(), 1 << 16)
        LIBRARY.rf_free(None)

    def test_threads_prefix_and_padded_rows(self):
        padded = b''.join(KAT_RGB[y * 48:(y + 1) * 48] + b'padding' for y in range(8))
        for threads in (0, 1, 3, 6, 9):
            self.assertEqual(encode(16, 8, padded, threads, 55, prefix=True), KAT_FOLD3)

    def test_decode_and_inspect(self):
        out, err, info = Pixels(), Error(), Info()
        self.assertEqual(LIBRARY.rf_decode_image(KAT_FOLD, len(KAT_FOLD), None, C.byref(out), C.byref(err)), 0)
        try:
            self.assertEqual((out.width, out.height, out.stride, out.layout, out.size), (16, 8, 48, 1, 384))
            self.assertEqual(C.string_at(out.data, out.size), KAT_RGB)
        finally:
            LIBRARY.rf_free(out.data)
        self.assertEqual(LIBRARY.rf_inspect(KAT_FOLD, len(KAT_FOLD), C.byref(info), None), 0)
        self.assertEqual((info.width, info.height, info.method, tuple(info.stream_size)), (16, 8, 1, struct.unpack_from('<III', KAT_FOLD, 32)))
        self.assertEqual(LIBRARY.rf_inspect(KAT_FOLD3, len(KAT_FOLD3), C.byref(info), None), 0)
        self.assertEqual((info.width, info.height, info.method, tuple(info.stream_size)), (16, 8, 1, struct.unpack_from('<III', KAT_FOLD3, 33)))
        # Verify S = 1 (h = 512), valid S = 2 (h = 512), S = 3 (h = 768), and valid S = 4 (h = 12 short-strip and h = 400)
        # version 3 CM stream_size[0..2] sums and multi-thread decoding.
        for mw, mh, want_s, force_s in ((8, 512, 1, None), (8, 512, 2, 2), (6, 768, 3, None), (8, 12, 4, 4), (6, 400, 4, 4)):
            mrgb = bytes(((x * (29 + c * 7) + y * (13 + c * 5) + ((x * 7 + y * 5 + c * 3) % 11)) & 255)
                         for y in range(mh) for x in range(mw) for c in range(3))
            if force_s is None:
                mfold = encode(mw, mh, mrgb, threads=1)
            else:
                mfold = rfi_ref.build_file(mw, mh, rfi_ref.METHOD_CM, rfi_ref.cm_encode(mw, mh, mrgb, nstrips=force_s))
            self.assertEqual((mfold[16], mfold[32]), (1, want_s))
            msizes = struct.unpack_from(f'<{3 * want_s}I', mfold, 33)
            exp_sums = tuple(sum(msizes[3 * s + c] for s in range(want_s)) for c in range(3))
            self.assertEqual(LIBRARY.rf_inspect(mfold, len(mfold), C.byref(info), C.byref(err)), 0)
            self.assertEqual((info.width, info.height, info.method, tuple(info.stream_size)), (mw, mh, 1, exp_sums))
            for t in (1, 3, 6, 9):
                opt = Options(C.sizeof(Options), t)
                self.assertEqual(LIBRARY.rf_decode_image(mfold, len(mfold), C.byref(opt), C.byref(out), C.byref(err)), 0)
                try:
                    self.assertEqual((out.width, out.height, out.size), (mw, mh, len(mrgb)))
                    self.assertEqual(C.string_at(out.data, out.size), mrgb)
                finally:
                    LIBRARY.rf_free(out.data)
        self.assertEqual(encode(KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), KAT_PAL2_3)
        for m_val, pal_blob in ((2, KAT_PAL), (3, KAT_PAL2), (3, KAT_PAL2_3)):
            self.assertEqual(LIBRARY.rf_decode_image(pal_blob, len(pal_blob), None, C.byref(out), C.byref(err)), 0)
            try:
                self.assertEqual((out.width, out.height, out.stride, out.layout, out.size), (16, 8, 48, 1, 384))
                self.assertEqual(C.string_at(out.data, out.size), KAT_PAL_RGB)
            finally:
                LIBRARY.rf_free(out.data)
            self.assertEqual(LIBRARY.rf_inspect(pal_blob, len(pal_blob), C.byref(info), None), 0)
            self.assertEqual((info.width, info.height, info.method, tuple(info.stream_size)), (16, 8, m_val, (0, 0, 0)))
        # Verify legacy method 2 long zero-run and long one-run fixtures through the RGB (stride == 0) library decoder.
        c0, c1, c2 = b"\x12\x34\x56", b"\xFE\xDC\xBA", b"\x44\x88\xCC"
        rgb_m2_zero = b"".join((c1 if (x & 1) else c0) if x < 450 else c1 for x in range(512))
        rgb_m2_one = b"".join(c0 if x < 520 else (c0 if (x & 1) else (c1 if (x & 2) else c2)) for x in range(600))
        for lw, lh, lrgb in ((512, 1, rgb_m2_zero), (600, 1, rgb_m2_one)):
            pay_m2 = rfi_ref.pal_encode(lw, lh, lrgb, method=rfi_ref.METHOD_PAL)
            self.assertIsNotNone(pay_m2)
            blob_m2 = rfi_ref.build_file(lw, lh, rfi_ref.METHOD_PAL, pay_m2)
            self.assertEqual(LIBRARY.rf_decode_image(blob_m2, len(blob_m2), None, C.byref(out), C.byref(err)), 0)
            try:
                self.assertEqual((out.width, out.height, out.size), (lw, lh, len(lrgb)))
                self.assertEqual(C.string_at(out.data, out.size), lrgb)
            finally:
                LIBRARY.rf_free(out.data)

    def test_corpus_matches_cli_and_oracle(self):
        count = 0
        for src in sorted((ROOT / 'tests/work').glob('*/in.bmp')):
            encoded = src.parent / 'out.fold'
            if not encoded.is_file():
                continue
            try:
                w, h, rgb = rfi_ref.read_bmp24(src.read_bytes())
            except ValueError:
                continue
            actual = encode(w, h, rgb)
            self.assertEqual(actual, encoded.read_bytes(), str(src))
            self.assertEqual(rfi_ref.decode_image(actual), (w, h, rgb))
            count += 1
        self.assertGreaterEqual(count, 10, 'run correctness before API corpus coverage')

    def test_encode_refusals_zero_output(self):
        raw = C.create_string_buffer(KAT_RGB)
        for field, value, code in [('width', 0, 4), ('width', 0xffffffff, 5), ('layout', 2, 3),
                                   ('stride', 1, 1), ('stride', C.c_size_t(-1).value, 1), ('size', 1, 1), ('data', None, 1)]:
            with self.subTest(field=field):
                pix = Pixels(16, 8, 48, 1, C.addressof(raw), len(KAT_RGB))
                setattr(pix, field, value)
                out, err = Buffer(1, 123), Error()
                self.assertEqual(LIBRARY.rf_encode_image(C.byref(pix), None, C.byref(out), C.byref(err)), code)
                self.assertEqual((out.data, out.size, err.code), (None, 0, code))
                self.assertTrue(err.message)
        self.assertEqual(LIBRARY.rf_encode_image(None, None, None, None), 1)

    def test_options_refusals(self):
        for opt, code in [(Options(4, 1), 1), (Options(9, 1), 1), (Options(28, 1), 3), (Options(24, 10), 1), (Options(24, 1, (C.c_uint32 * 4)(1)), 3)]:
            out = Pixels()
            self.assertEqual(LIBRARY.rf_decode_image(KAT_FOLD, len(KAT_FOLD), C.byref(opt), C.byref(out), None), code)
            self.assertFalse(out.data)

    def test_malformed_and_independent_errors(self):
        def reject(i):
            out, err, info = Pixels(), Error(), Info()
            data = None if i % 2 else KAT_FOLD[:-1]
            code = 1 if data is None else 4
            self.assertEqual(LIBRARY.rf_decode_image(data, len(KAT_FOLD) - 1, None, C.byref(out), C.byref(err)), code)
            self.assertEqual(err.code, code)
            self.assertFalse(out.data)
            self.assertEqual(LIBRARY.rf_inspect(data, len(KAT_FOLD) - 1, C.byref(info), C.byref(err)), code)
            self.assertEqual(bytes(info), bytes(C.sizeof(info)))
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(reject, range(40)))
        out = Pixels()
        self.assertEqual(LIBRARY.rf_decode_image(KAT_FOLD, C.c_size_t(-1).value, None, C.byref(out), None), 5)
        self.assertEqual(LIBRARY.rf_inspect(KAT_FOLD, len(KAT_FOLD), None, None), 1)

    def test_guard_page_exact_length_inputs(self):
        @contextmanager
        def guarded_slice(blob: bytes):
            page = mmap.PAGESIZE
            data_pages = max(1, (len(blob) + page - 1) // page)
            total = (data_pages + 1) * page
            if os.name == 'nt':
                kernel32 = C.windll.kernel32
                kernel32.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]
                kernel32.VirtualAlloc.restype = C.c_void_p
                kernel32.VirtualProtect.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.POINTER(C.c_uint32)]
                kernel32.VirtualProtect.restype = C.c_int
                kernel32.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32]
                kernel32.VirtualFree.restype = C.c_int
                base = kernel32.VirtualAlloc(None, total, 0x3000, 0x04)
                self.assertTrue(base)
                try:
                    old = C.c_uint32()
                    guard = base + data_pages * page
                    self.assertTrue(kernel32.VirtualProtect(C.c_void_p(guard), page, 0x01, C.byref(old)))
                    ptr = guard - len(blob)
                    C.memmove(C.c_void_p(ptr), blob, len(blob))
                    yield C.c_void_p(ptr), len(blob)
                finally:
                    kernel32.VirtualFree(C.c_void_p(base), 0, 0x8000)
            else:
                libc = C.CDLL(None)
                libc.mprotect.argtypes = [C.c_void_p, C.c_size_t, C.c_int]
                libc.mprotect.restype = C.c_int
                mm = mmap.mmap(-1, total, prot=mmap.PROT_READ | mmap.PROT_WRITE)
                try:
                    base = C.addressof(C.c_char.from_buffer(mm))
                    guard = base + data_pages * page
                    self.assertEqual(libc.mprotect(C.c_void_p(guard), page, 0), 0)
                    try:
                        ptr = guard - len(blob)
                        C.memmove(C.c_void_p(ptr), blob, len(blob))
                        yield C.c_void_p(ptr), len(blob)
                    finally:
                        libc.mprotect(C.c_void_p(guard), page, mmap.PROT_READ | mmap.PROT_WRITE)
                finally:
                    mm.close()

        def rebuild_fold(header_template: bytes, payload: bytes) -> bytes:
            hdr = bytearray(header_template[:20])
            hdr += struct.pack('<III', len(payload), zlib.crc32(payload) & 0xFFFFFFFF, 0)
            hdr[28:32] = struct.pack('<I', zlib.crc32(hdr[:28]) & 0xFFFFFFFF)
            return bytes(hdr) + payload

        large_w, large_h = 32, 16
        large_rgb = bytes(((x * 13 + y * 7 + c * 53 + ((x ^ y) & 7)) & 255)
                          for y in range(large_h) for x in range(large_w) for c in range(3))
        large_fold = encode(large_w, large_h, large_rgb, threads=1)
        raw_w, raw_h = 4, 4
        raw_rgb = bytes(((i * 197 + 53) & 255) for i in range(raw_w * raw_h * 3))
        raw_fold = encode(raw_w, raw_h, raw_rgb, threads=1)
        pal_long_w, pal_long_h = 32, 32
        pal_cols = [bytes(((i * 29) & 255, (i * 61 + 11) & 255, (i * 103 + 7) & 255)) for i in range(16)]
        pal_long_rgb = b''.join(pal_cols[((x * 3 + y * 5 + ((x ^ y) & 3)) & 15)] for y in range(pal_long_h) for x in range(pal_long_w))
        pal_long_fold = encode(pal_long_w, pal_long_h, pal_long_rgb, threads=1)
        self.assertEqual(pal_long_fold[16], 3)

        for label, blob, exp_w, exp_h, exp_rgb in [
            ('kat_short_tail', KAT_FOLD, KAT_W, KAT_H, KAT_RGB),
            ('large_long_tail', large_fold, large_w, large_h, large_rgb),
            ('raw_method0', raw_fold, raw_w, raw_h, raw_rgb),
            ('pal_m2_short_tail', KAT_PAL, KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB),
            ('pal_m3_short_tail', KAT_PAL2, KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB),
            ('pal_long_tail', pal_long_fold, pal_long_w, pal_long_h, pal_long_rgb),
        ]:
            for threads in (1, 3):
                with self.subTest(case=label, threads=threads), guarded_slice(blob) as (ptr, size):
                    opt = Options(C.sizeof(Options), threads)
                    out, err, info = Pixels(), Error(), Info()
                    self.assertEqual(LIBRARY.rf_decode_image(ptr, size, C.byref(opt), C.byref(out), C.byref(err)), 0)
                    try:
                        self.assertEqual((out.width, out.height, out.size), (exp_w, exp_h, len(exp_rgb)))
                        self.assertEqual(C.string_at(out.data, out.size), exp_rgb)
                    finally:
                        LIBRARY.rf_free(out.data)
                    self.assertEqual(LIBRARY.rf_inspect(ptr, size, C.byref(info), C.byref(err)), 0)
                    self.assertEqual((info.width, info.height), (exp_w, exp_h))

        # Checksum-valid malformed CM streams where each channel stream is replaced
        # by 5 zero bytes (forcing stream exhaustion right against the guard page).
        sz = list(struct.unpack_from('<III', large_fold, 33))
        streams = [
            large_fold[45:45 + sz[0]],
            large_fold[45 + sz[0]:45 + sz[0] + sz[1]],
            large_fold[45 + sz[0] + sz[1]:],
        ]
        for ch in range(3):
            mod = list(streams)
            mod[ch] = b'\x00' * 5
            payload = b'\x01' + struct.pack('<III', len(mod[0]), len(mod[1]), len(mod[2])) + b''.join(mod)
            bad_fold = rebuild_fold(large_fold[:32], payload)
            for threads in (1, 3):
                with self.subTest(case=f'exhaust_ch{ch}', threads=threads), guarded_slice(bad_fold) as (ptr, size):
                    opt = Options(C.sizeof(Options), threads)
                    out, err, info = Pixels(), Error(), Info()
                    self.assertEqual(LIBRARY.rf_decode_image(ptr, size, C.byref(opt), C.byref(out), C.byref(err)), 4)
                    self.assertFalse(out.data)
                    self.assertEqual(LIBRARY.rf_inspect(ptr, size, C.byref(info), C.byref(err)), 4)


if __name__ == '__main__':
    unittest.main(verbosity=2)
