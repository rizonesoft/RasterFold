"""Public ABI, ownership, concurrency, fidelity and native consumer tests."""
from __future__ import annotations

import ctypes as C
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import rfi_ref
from test_correctness import KAT_FOLD, KAT_RGB, KAT_W, KAT_H

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
        self.assertEqual(result, [KAT_FOLD] * 4)

    def test_version(self):
        self.assertEqual(LIBRARY.rf_version(), 1 << 16)
        LIBRARY.rf_free(None)

    def test_threads_prefix_and_padded_rows(self):
        padded = b''.join(KAT_RGB[y * 48:(y + 1) * 48] + b'padding' for y in range(8))
        for threads in (0, 1, 3, 9):
            self.assertEqual(encode(16, 8, padded, threads, 55, prefix=True), KAT_FOLD)

    def test_decode_and_inspect(self):
        out, err, info = Pixels(), Error(), Info()
        self.assertEqual(LIBRARY.rf_decode_image(KAT_FOLD, len(KAT_FOLD), None, C.byref(out), C.byref(err)), 0)
        try:
            self.assertEqual((out.width, out.height, out.stride, out.layout, out.size), (16, 8, 48, 1, 384))
            self.assertEqual(C.string_at(out.data, out.size), KAT_RGB)
        finally:
            LIBRARY.rf_free(out.data)
        self.assertEqual(LIBRARY.rf_inspect(KAT_FOLD, len(KAT_FOLD), C.byref(info), None), 0)
        self.assertEqual((info.width, info.height, info.method), (16, 8, 1))

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


if __name__ == '__main__':
    unittest.main(verbosity=2)
