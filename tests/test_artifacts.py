"""Verify public exports, target metadata, library consumers and file safety."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_correctness import KAT_FOLD, KAT_FOLD3, KAT_RGB
import rfi_ref

ROOT=Path(__file__).resolve().parents[1]
WIN=os.name=='nt'
TARGET='windows-x64' if WIN else 'linux-x64'
TOOLS=ROOT/('tools/llvm-mingw/bin' if WIN else 'tools/llvm-mingw-linux/bin')
OUT=ROOT/'bin'/TARGET
EXPORTS={'rf_version':1,'rf_encode_image':2,'rf_decode_image':3,'rf_inspect':4,'rf_free':5}


def tool(name):return str(TOOLS/(name+('.exe' if WIN else '')))


def run(args, **kw):
    try:
        return subprocess.check_output(list(map(str,args)),stderr=subprocess.STDOUT,**kw)
    except subprocess.CalledProcessError as exc:
        raise AssertionError(exc.output.decode(errors='replace')) from exc


class Artifacts(unittest.TestCase):
    def test_public_exports(self):
        source=dict((name,int(ordinal)) for name,ordinal in re.findall(r'(rf_\w+)\s+@(\d+)',(ROOT/'src/rasterfold.def').read_text()))
        self.assertEqual(source,EXPORTS)
        for target in ('windows-x64','linux-x64','android-arm64','android-x64'):
            if WIN and target != 'windows-x64':
                continue  # WSL ELF symlinks are not normal Windows filesystem links.
            directory=ROOT/'bin'/target
            if not directory.is_dir():continue
            if target=='windows-x64':
                text=run([tool('llvm-readobj'),'--coff-exports',directory/'bin/rasterfold.dll']).decode()
                actual={name:int(n) for n,name in re.findall(r'Ordinal: (\d+)\s+Name: (rf_\w+)',text)}
                self.assertEqual(actual,EXPORTS)
                imports=run([tool('llvm-objdump'),'-p',directory/'bin/rasterfold.dll']).decode()
                self.assertEqual(set(re.findall(r'DLL Name: (\S+)',imports)),{'KERNEL32.dll'})
                imports=run([tool('llvm-objdump'),'-p',directory/'bin/rasterfold.exe']).decode()
                self.assertEqual(set(re.findall(r'DLL Name: (\S+)',imports)),{'KERNEL32.dll','SHELL32.dll'})
                for binary,pdb in (('rasterfold.exe','rasterfold.pdb'),('rasterfold.dll','rasterfold-dll.pdb')):
                    debug=run([tool('llvm-readobj'),'--coff-debug-directory',directory/'bin'/binary]).decode()
                    summary=run([tool('llvm-pdbutil'),'dump','-summary',directory/'symbols'/pdb]).decode()
                    binary_id=re.search(r'PDBGUID: (\{[^}]+\})',debug)
                    symbol_id=re.search(r'GUID: (\{[^}]+\})',summary)
                    self.assertIsNotNone(binary_id);self.assertIsNotNone(symbol_id)
                    self.assertEqual(binary_id[1],symbol_id[1])
                    self.assertIn('Has Debug Info: true',summary)
            else:
                text=run([tool('llvm-nm'),'--dynamic','--defined-only',directory/'lib/librasterfold.so']).decode()
                self.assertEqual({line.split()[-1] for line in text.splitlines() if line.strip()},set(EXPORTS))
                dynamic=run([tool('llvm-readelf'),'-d',directory/'lib/librasterfold.so']).decode()
                soname='librasterfold.so.1' if target=='linux-x64' else 'librasterfold.so'
                self.assertRegex(dynamic,r'\(SONAME\).*\['+re.escape(soname)+r'\]')
            symbols=run([tool('llvm-nm'),'--defined-only','--extern-only',directory/'lib/librasterfold.a']).decode()
            names=re.findall(r'^[0-9a-fA-F]+\s+[A-Za-z]\s+(\S+)$',symbols,re.M)
            self.assertTrue(names)
            self.assertTrue(all(name.startswith('rf_') for name in names),str(names))
            manifest=json.loads((directory/'build.json').read_text())
            for name,digest in manifest['artifacts'].items():
                self.assertEqual(hashlib.sha256((directory/name).read_bytes()).hexdigest(),digest)

    def test_c_cpp_static_and_dynamic_consumers(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp)
            triple='x86_64-w64-mingw32' if WIN else 'x86_64-linux-gnu'
            flags=[f'--target={triple}','-Wall','-Wextra','-Werror','-I'+str(OUT/'include'),'-fuse-ld=lld']
            header=temp/'consumer.cpp';header.write_text('#include "rasterfold.h"\nint main(){ return rf_version() == RF_API_VERSION ? 0 : 1; }\n')
            for shared in (False,True):
                library=OUT/'lib'/('librasterfold.dll.a' if WIN and shared else 'librasterfold.so' if shared else 'librasterfold.a')
                link=[str(library),*(['-lkernel32',*(['-DRF_SHARED'] if shared else [])] if WIN else ['-pthread','-Wl,-rpath,'+str(OUT/'lib')])]
                if WIN:shutil.copy2(OUT/'bin/rasterfold.dll',temp/'rasterfold.dll')
                if WIN and shared:
                    # The MSVC-format import library must also link the same API.
                    run([tool('clang++'),*flags,header,OUT/'lib/rasterfold.lib','-o',temp/'msvc-import.exe'])
                    run([temp/'msvc-import.exe'])
                exe=temp/('cpp'+('.exe' if WIN else ''))
                run([tool('clang++'),*flags,header,*link,'-o',exe]);run([exe])
                commands=[]
                for name in ('encode','decode'):
                    exe=temp/(name+('.exe' if WIN else ''))
                    run([tool('clang'),'-std=c11',*flags,ROOT/'examples'/f'{name}.c',*link,'-o',exe]);commands.append(exe)
                source=temp/'in.ppm';source.write_bytes(b'P6\n16 8\n255\n'+KAT_RGB)
                encoded=temp/'out.fold';decoded=temp/'out.ppm'
                encoded.unlink(missing_ok=True);decoded.unlink(missing_ok=True)
                run([commands[0],source,encoded]);self.assertEqual(encoded.read_bytes(),KAT_FOLD3)
                run([commands[1],encoded,decoded]);self.assertEqual(decoded.read_bytes(),source.read_bytes())

    def test_rf_read_file_eight_byte_zero_padding(self):
        harness = (
            '#include <stdio.h>\n'
            '#include <string.h>\n'
            '#include "rf.h"\n'
            'int main(int argc, char **argv) {\n'
            '    if (argc < 2) return 2;\n'
            '    const char *dir = argv[1];\n'
            '    static const size_t lengths[] = {1, 7, 8, 63, 64, 109};\n'
            '    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {\n'
            '        size_t want = lengths[i];\n'
            '        char path_utf8[512];\n'
            '        snprintf(path_utf8, sizeof path_utf8, "%s/pad_%zu.bin", dir, want);\n'
            '        FILE *f = fopen(path_utf8, "wb");\n'
            '        if (!f) return 3;\n'
            '        for (size_t k = 0; k < want; k++) {\n'
            '            unsigned char b = (unsigned char)((0xA5u ^ (unsigned)k) | 1u);\n'
            '            if (fputc(b, f) == EOF) { fclose(f); return 4; }\n'
            '        }\n'
            '        fclose(f);\n'
            '        rf_path_char path[512];\n'
            '        for (size_t k = 0; ; k++) { path[k] = (rf_path_char)(unsigned char)path_utf8[k]; if (!path_utf8[k]) break; }\n'
            '        uint8_t *data = 0;\n'
            '        size_t got = 0;\n'
            '        rf_error err = {0, 0};\n'
            '        if (rf_read_file(path, &data, &got, &err) != RF_OK) return 5;\n'
            '        if (got != want || !data) { rf_free(data); return 6; }\n'
            '#ifdef _WIN32\n'
            '        size_t alloc_sz = ((size_t *)data)[-2];\n'
            '#else\n'
            '        size_t alloc_sz = ((size_t *)data)[-1];\n'
            '#endif\n'
            '        if (alloc_sz != want + 8) { rf_free(data); return 7; }\n'
            '        for (size_t k = 0; k < want; k++) {\n'
            '            uint8_t exp = (uint8_t)((0xA5u ^ (unsigned)k) | 1u);\n'
            '            if (data[k] != exp) { rf_free(data); return 8; }\n'
            '        }\n'
            '        for (size_t k = 0; k < 8; k++) {\n'
            '            if (data[want + k] != 0) { rf_free(data); return 9; }\n'
            '        }\n'
            '        rf_free(data);\n'
            '    }\n'
            '    return 0;\n'
            '}\n'
        )
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp)
            src=temp/'pad_test.c';src.write_text(harness)
            triple='x86_64-w64-mingw32' if WIN else 'x86_64-linux-gnu'
            exe=temp/('pad_test'+('.exe' if WIN else ''))
            flags=[f'--target={triple}','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),'-I'+str(OUT/'include'),'-fuse-ld=lld']
            link=[str(OUT/'lib/librasterfold.a'),*(['-lkernel32'] if WIN else ['-pthread'])]
            run([tool('clang'),*flags,src,*link,'-o',exe])
            run([exe,temp.relative_to(ROOT).as_posix()],cwd=ROOT,stdin=subprocess.DEVNULL)

    def test_rf_cm_encode_in_place_bounds_and_overflow(self):
        harness = (
            '#include <stdlib.h>\n'
            '#include <string.h>\n'
            '#include "rf.h"\n'
            'static int check_canaries(const uint8_t *guard, size_t work) {\n'
            '    for (size_t i = 0; i < 64; i++) {\n'
            '        if (guard[i] != 0xA5 || guard[64 + work + i] != 0x5A) return 0;\n'
            '    }\n'
            '    return 1;\n'
            '}\n'
            'static uint32_t rd32_le(const uint8_t *p) {\n'
            '    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);\n'
            '}\n'
            'static int test_one_geom(uint32_t w, uint32_t h, int want_s) {\n'
            '    const size_t raw = (size_t)w * h * 3;\n'
            '    const size_t work = 1u + 12u * (size_t)want_s + raw + 3u * (size_t)want_s * (64u / (size_t)want_s);\n'
            '    if (work > raw + 226u) return 1;\n'
            '    uint8_t *rgb1 = (uint8_t *)rf_alloc(raw);\n'
            '    uint8_t *rgb2 = (uint8_t *)rf_alloc(raw);\n'
            '    uint8_t *dec = (uint8_t *)rf_alloc(raw);\n'
            '    uint8_t *guard = (uint8_t *)rf_alloc(64 + work + 64);\n'
            '    uint8_t *ref_out = (uint8_t *)rf_alloc(work);\n'
            '    if (!rgb1 || !rgb2 || !dec || !guard || !ref_out) return 2;\n'
            '    uint32_t s = 0x12345678u;\n'
            '    for (size_t i = 0; i < (size_t)w * h; i++) {\n'
            '        if ((i & 1) == 0) {\n'
            '            rgb1[i * 3] = rgb1[i * 3 + 1] = rgb1[i * 3 + 2] = 0;\n'
            '        } else {\n'
            '            s ^= s << 13; s ^= s >> 17; s ^= s << 5;\n'
            '            uint8_t g = (uint8_t)(s & 0xFF);\n'
            '            s ^= s << 13; s ^= s >> 17; s ^= s << 5;\n'
            '            uint8_t r = (uint8_t)(s & 0xFF);\n'
            '            rgb1[i * 3] = r; rgb1[i * 3 + 1] = g; rgb1[i * 3 + 2] = g;\n'
            '        }\n'
            '    }\n'
            '    s = 0xCAFEBABEu;\n'
            '    for (size_t i = 0; i < (size_t)w * h; i++) {\n'
            '        s ^= s << 13; s ^= s >> 17; s ^= s << 5;\n'
            '        uint8_t g = (uint8_t)(s & 0xFF);\n'
            '        rgb2[i * 3] = g; rgb2[i * 3 + 1] = g; rgb2[i * 3 + 2] = g;\n'
            '    }\n'
            '    size_t ref_written = 0;\n'
            '    static const int tcounts[4] = {1, 3, 6, 9};\n'
            '    const size_t hdr = 1u + 12u * (size_t)want_s;\n'
            '    for (int ti = 0; ti < 4; ti++) {\n'
            '        int threads = tcounts[ti];\n'
            '        rf_error err = {0, 0};\n'
            '        /* 1. Overlapping compaction within exact worst-case work buffer. */\n'
            '        memset(guard, 0xA5, 64);\n'
            '        memset(guard + 64, 0xCC, work);\n'
            '        memset(guard + 64 + work, 0x5A, 64);\n'
            '        size_t written = 0;\n'
            '        if (rf_cm_encode(rgb1, w, h, guard + 64, raw - 1, &written, threads, &err) != RF_OK) return 3;\n'
            '        if (!check_canaries(guard, work)) return 4;\n'
            '        if (guard[64] != (uint8_t)want_s) return 5;\n'
            '        size_t sum_sz = 0;\n'
            '        for (int k = 0; k < 3 * want_s; k++) sum_sz += rd32_le(guard + 65 + 4 * k);\n'
            '        if (written == 0 || written != hdr + sum_sz) return 5;\n'
            '        if (want_s == 1) {\n'
            '            size_t each = (size_t)w * h + 64u;\n'
            '            if ((size_t)rd32_le(guard + 65) + rd32_le(guard + 69) <= each) return 5;\n'
            '        }\n'
            '        if (ti == 0) { ref_written = written; memcpy(ref_out, guard + 64, written); }\n'
            '        else if (written != ref_written || memcmp(ref_out, guard + 64, written) != 0) return 6;\n'
            '        memset(dec, 0, raw);\n'
            '        if (rf_cm_decode(guard + 64, written, dec, 0, w, h, threads, 3, &err) != RF_OK) return 7;\n'
            '        if (memcmp(dec, rgb1, raw) != 0) return 8;\n'
            '        /* 2. Aggregate payload overflow: each stream fits <= each_s, but total > cap. */\n'
            '        memset(guard, 0xA5, 64);\n'
            '        memset(guard + 64, 0xCC, work);\n'
            '        memset(guard + 64 + work, 0x5A, 64);\n'
            '        written = 999;\n'
            '        if (rf_cm_encode(rgb1, w, h, guard + 64, ref_written - 1, &written, threads, &err) != RF_OK) return 9;\n'
            '        if (written != 0 || !check_canaries(guard, work)) return 10;\n'
            '        /* 3. Individual stream overflow: palette fallback (<=256 colours) and raw fallback (>256 colours). */\n'
            '        memset(guard, 0xA5, 64);\n'
            '        memset(guard + 64, 0xCC, work);\n'
            '        memset(guard + 64 + work, 0x5A, 64);\n'
            '        written = 999;\n'
            '        if (rf_cm_encode(rgb2, w, h, guard + 64, raw - 1, &written, threads, &err) != RF_OK) return 11;\n'
            '        if (written != 0 || !check_canaries(guard, work)) return 12;\n'
            '        rf_image img = {w, h, rgb2};\n'
            '        uint8_t *fold = 0;\n'
            '        size_t fold_sz = 0;\n'
            '        if (rf_encode(&img, &fold, &fold_sz, threads, &err) != RF_OK) return 13;\n'
            '        if (!fold || fold_sz >= RF_HEADER_SIZE + raw || fold[16] != RF_METHOD_PAL2) return 14;\n'
            '        rf_image round = {0, 0, 0};\n'
            '        if (rf_decode(fold, fold_sz, &round, 0, threads, &err) != RF_OK) return 15;\n'
            '        if (round.width != w || round.height != h || memcmp(round.rgb, rgb2, raw) != 0) return 16;\n'
            '        rf_free(round.rgb);\n'
            '        rf_free(fold);\n'
            '        rgb2[0] ^= 1u;\n'
            '        memset(guard, 0xA5, 64);\n'
            '        memset(guard + 64, 0xCC, work);\n'
            '        memset(guard + 64 + work, 0x5A, 64);\n'
            '        written = 999;\n'
            '        if (rf_cm_encode(rgb2, w, h, guard + 64, raw - 1, &written, threads, &err) != RF_OK) return 17;\n'
            '        if (written != 0 || !check_canaries(guard, work)) return 18;\n'
            '        fold = 0; fold_sz = 0;\n'
            '        if (rf_encode(&img, &fold, &fold_sz, threads, &err) != RF_OK) return 19;\n'
            '        if (!fold || fold_sz != RF_HEADER_SIZE + raw || fold[16] != RF_METHOD_RAW || memcmp(fold + RF_HEADER_SIZE, rgb2, raw) != 0) return 20;\n'
            '        memset(&round, 0, sizeof(round));\n'
            '        if (rf_decode(fold, fold_sz, &round, 0, threads, &err) != RF_OK) return 21;\n'
            '        if (round.width != w || round.height != h || memcmp(round.rgb, rgb2, raw) != 0) return 22;\n'
            '        rf_free(round.rgb);\n'
            '        rf_free(fold);\n'
            '        rgb2[0] ^= 1u;\n'
            '    }\n'
            '    rf_free(ref_out); rf_free(guard); rf_free(dec); rf_free(rgb2); rf_free(rgb1);\n'
            '    return 0;\n'
            '}\n'
            'int main(void) {\n'
            '    int rc;\n'
            '    if ((rc = test_one_geom(64, 64, 1)) != 0) return 10 + rc;\n'
            '    if ((rc = test_one_geom(16, 512, 2)) != 0) return 40 + rc;\n'
            '    if ((rc = test_one_geom(12, 768, 3)) != 0) return 70 + rc;\n'
            '    return 0;\n'
            '}\n'
        )
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp)
            src=temp/'cm_enc_test.c';src.write_text(harness)
            triple='x86_64-w64-mingw32' if WIN else 'x86_64-linux-gnu'
            exe=temp/('cm_enc_test'+('.exe' if WIN else ''))
            flags=[f'--target={triple}','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),'-I'+str(OUT/'include'),'-fuse-ld=lld']
            link=[str(OUT/'lib/librasterfold.a'),*(['-lkernel32'] if WIN else ['-pthread'])]
            run([tool('clang'),*flags,src,*link,'-o',exe])
            run([exe],cwd=ROOT,stdin=subprocess.DEVNULL)

    def test_cm_thread_failure_injection_and_portable_simd(self):
        harness = (
            '#include <stdio.h>\n'
            '#include <stdint.h>\n'
            '#include <stdlib.h>\n'
            '#include <string.h>\n'
            '#include "rf.h"\n'
            '#include "simd.h"\n'
            'uint32_t g_test_thread_fail_mask = 0;\n'
            'int g_test_thread_calls = 0;\n'
            'int g_test_thread_injected = 0;\n'
            'int g_test_thread_live = 0;\n'
            'static int64_t g_test_live_alloc = 0;\n'
            'uint64_t rf_internal_alloc_peak = 0;\n'
            'int rf_fail(rf_error *err, int code, const char *msg) {\n'
            '    if (err && !err->code) { err->code = code; err->message = msg; }\n'
            '    return code;\n'
            '}\n'
            'void rf_account_add(size_t size) { __atomic_add_fetch(&g_test_live_alloc, (int64_t)size, __ATOMIC_SEQ_CST); }\n'
            'void rf_account_remove(size_t size) { __atomic_sub_fetch(&g_test_live_alloc, (int64_t)size, __ATOMIC_SEQ_CST); }\n'
            'static int check_simd_counter_all(void) {\n'
            '    for (int n = 0; n <= 60; n++) {\n'
            '        uint16_t r = (uint16_t)((65536u * 2u) / (uint32_t)(2 * n + 3));\n'
            '        __m128i rv = _mm_set1_epi16((int16_t)r);\n'
            '        for (int bit = 0; bit <= 1; bit++) {\n'
            '            __m128i mask = _mm_set1_epi16((int16_t)-bit);\n'
            '            __m128i not_mask = _mm_set1_epi16((int16_t)(bit - 1));\n'
            '            int64_t target = bit ? 65535 : 0;\n'
            '            for (uint32_t base = 0; base < 65536u; base += 8u) {\n'
            '                __m128i cv = _mm_setr_epi16(\n'
            '                    (int16_t)(base + 0), (int16_t)(base + 1), (int16_t)(base + 2), (int16_t)(base + 3),\n'
            '                    (int16_t)(base + 4), (int16_t)(base + 5), (int16_t)(base + 6), (int16_t)(base + 7));\n'
            '                __m128i xv = _mm_xor_si128(cv, mask);\n'
            '                __m128i hi = _mm_mulhi_epu16(xv, rv);\n'
            '                __m128i adj = _mm_and_si128(_mm_cmpeq_epi16(_mm_mullo_epi16(xv, rv), _mm_setzero_si128()), not_mask);\n'
            '                __m128i uv = _mm_sub_epi16(_mm_add_epi16(cv, _mm_xor_si128(hi, not_mask)), adj);\n'
            '                uint16_t got[8];\n'
            '                got[0] = (uint16_t)_mm_extract_epi16(uv, 0);\n'
            '                got[1] = (uint16_t)_mm_extract_epi16(uv, 1);\n'
            '                got[2] = (uint16_t)_mm_extract_epi16(uv, 2);\n'
            '                got[3] = (uint16_t)_mm_extract_epi16(uv, 3);\n'
            '                got[4] = (uint16_t)_mm_extract_epi16(uv, 4);\n'
            '                got[5] = (uint16_t)_mm_extract_epi16(uv, 5);\n'
            '                got[6] = (uint16_t)_mm_extract_epi16(uv, 6);\n'
            '                got[7] = (uint16_t)_mm_extract_epi16(uv, 7);\n'
            '                for (uint32_t j = 0; j < 8u; j++) {\n'
            '                    uint32_t v = base + j;\n'
            '                    uint16_t exp = (uint16_t)(v + (int32_t)(((target - (int64_t)v) * (int64_t)r) >> 16));\n'
            '                    if (got[j] != exp) return 0;\n'
            '                }\n'
            '            }\n'
            '        }\n'
            '    }\n'
            '    return 1;\n'
            '}\n'
            'static int test_geom(uint32_t w, uint32_t h, FILE *dump_f) {\n'
            '    size_t raw = (size_t)w * h * 3;\n'
            '    size_t cap = raw + 256;\n'
            '    uint8_t *rgb = (uint8_t *)rf_alloc(raw);\n'
            '    uint8_t *ref_buf = (uint8_t *)rf_alloc(cap);\n'
            '    uint8_t *out_buf = (uint8_t *)rf_alloc(cap);\n'
            '    uint8_t *dec_buf = (uint8_t *)rf_alloc(raw);\n'
            '    if (!rgb || !ref_buf || !out_buf || !dec_buf) return 2;\n'
            '    int64_t base_alloc = g_test_live_alloc;\n'
            '    uint32_t rng = 0x9E3779B9u ^ (w * 1000u + h);\n'
            '    for (size_t i = 0; i < raw; i++) {\n'
            '        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;\n'
            '        rgb[i] = (uint8_t)(((i * 19u) + (rng & 31u)) & 0xFFu);\n'
            '    }\n'
            '    rf_error err = {0, 0};\n'
            '    size_t ref_sz = 0;\n'
            '    g_test_thread_fail_mask = 0; g_test_thread_calls = 0; g_test_thread_injected = 0;\n'
            '    if (rf_cm_encode(rgb, w, h, ref_buf, cap, &ref_sz, 1, &err) != RF_OK || ref_sz == 0) return 3;\n'
            '    if (g_test_thread_live != 0 || g_test_live_alloc != base_alloc) return 4;\n'
            '    if (dump_f && fwrite(ref_buf, 1, ref_sz, dump_f) != ref_sz) return 4;\n'
            '    static const int tcounts[4] = {2, 3, 6, 9};\n'
            '    static const uint32_t multi_masks[5] = {0x3u, 0x5u, 0x6u, 0x7u, 0xFFu};\n'
            '    for (int ti = 0; ti < 4; ti++) {\n'
            '        int threads = tcounts[ti];\n'
            '        g_test_thread_fail_mask = 0; g_test_thread_calls = 0; g_test_thread_injected = 0;\n'
            '        size_t sz = 0;\n'
            '        if (rf_cm_encode(rgb, w, h, out_buf, cap, &sz, threads, &err) != RF_OK) return 5;\n'
            '        int total_calls = g_test_thread_calls;\n'
            '        if (total_calls <= 0 || g_test_thread_live != 0 || g_test_live_alloc != base_alloc || sz != ref_sz || memcmp(out_buf, ref_buf, ref_sz) != 0) return 6;\n'
            '        for (int fail_idx = 0; fail_idx < total_calls; fail_idx++) {\n'
            '            g_test_thread_fail_mask = 1u << fail_idx;\n'
            '            g_test_thread_calls = 0; g_test_thread_injected = 0;\n'
            '            sz = 0;\n'
            '            if (rf_cm_encode(rgb, w, h, out_buf, cap, &sz, threads, &err) != RF_OK) return 7;\n'
            '            if (g_test_thread_injected == 0 || g_test_thread_live != 0 || g_test_live_alloc != base_alloc || sz != ref_sz || memcmp(out_buf, ref_buf, ref_sz) != 0) return 8;\n'
            '            g_test_thread_fail_mask = 1u << fail_idx;\n'
            '            g_test_thread_calls = 0; g_test_thread_injected = 0;\n'
            '            memset(dec_buf, 0, raw);\n'
            '            if (rf_cm_decode(ref_buf, ref_sz, dec_buf, 0, w, h, threads, 3, &err) != RF_OK) return 9;\n'
            '            if (g_test_thread_injected == 0 || g_test_thread_live != 0 || g_test_live_alloc != base_alloc || memcmp(dec_buf, rgb, raw) != 0) return 10;\n'
            '        }\n'
            '        for (int mi = 0; mi < 5; mi++) {\n'
            '            g_test_thread_fail_mask = multi_masks[mi];\n'
            '            g_test_thread_calls = 0; g_test_thread_injected = 0;\n'
            '            sz = 0;\n'
            '            if (rf_cm_encode(rgb, w, h, out_buf, cap, &sz, threads, &err) != RF_OK) return 11;\n'
            '            if (g_test_thread_injected == 0 || g_test_thread_live != 0 || g_test_live_alloc != base_alloc || sz != ref_sz || memcmp(out_buf, ref_buf, ref_sz) != 0) return 12;\n'
            '            g_test_thread_fail_mask = multi_masks[mi];\n'
            '            g_test_thread_calls = 0; g_test_thread_injected = 0;\n'
            '            memset(dec_buf, 0, raw);\n'
            '            if (rf_cm_decode(ref_buf, ref_sz, dec_buf, 0, w, h, threads, 3, &err) != RF_OK) return 13;\n'
            '            if (g_test_thread_injected == 0 || g_test_thread_live != 0 || g_test_live_alloc != base_alloc || memcmp(dec_buf, rgb, raw) != 0) return 14;\n'
            '        }\n'
            '    }\n'
            '    rf_free(dec_buf); rf_free(out_buf); rf_free(ref_buf); rf_free(rgb);\n'
            '    if (g_test_live_alloc != 0) return 15;\n'
            '    return 0;\n'
            '}\n'
            'int main(int argc, char **argv) {\n'
            '    if (!check_simd_counter_all()) return 1;\n'
            '    FILE *f = (argc > 1) ? fopen(argv[1], "wb") : 0;\n'
            '    int rc;\n'
            '    if ((rc = test_geom(16, 16, f)) != 0) { if (f) fclose(f); return 20 + rc; }\n'
            '    if ((rc = test_geom(4, 512, f)) != 0) { if (f) fclose(f); return 40 + rc; }\n'
            '    if ((rc = test_geom(4, 768, f)) != 0) { if (f) fclose(f); return 60 + rc; }\n'
            '    if (f) fclose(f);\n'
            '    return 0;\n'
            '}\n'
        )
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp)
            src=temp/'cm_fail_simd_test.c';src.write_text(harness)
            triple='x86_64-w64-mingw32' if WIN else 'x86_64-linux-gnu'
            sys_srcs=[ROOT/'src/cm.c',*(
                [ROOT/'src/sys.c',ROOT/'src/mem.c'] if WIN else [ROOT/'src/sys_posix.c']
            )]
            base_flags=[f'--target={triple}','-std=c11','-O2','-march=x86-64-v2','-Wall','-Wextra','-Werror','-DRF_TEST_THREAD_FAIL','-I'+str(ROOT/'src'),'-I'+str(OUT/'include'),'-fuse-ld=lld']
            link=['-lkernel32'] if WIN else ['-pthread']
            dumps={}
            for tag, extra in (('native', []), ('portable', ['-DRF_PORTABLE_SIMD'])):
                exe=temp/(f'cm_fail_{tag}'+('.exe' if WIN else ''))
                dump_path=temp/f'dump_{tag}.bin'
                run([tool('clang'),*base_flags,*extra,src,*sys_srcs,*link,'-o',exe])
                run([exe,str(dump_path)],cwd=ROOT,stdin=subprocess.DEVNULL)
                dumps[tag]=dump_path.read_bytes()
            self.assertGreater(len(dumps['native']), 0)
            self.assertEqual(dumps['native'], dumps['portable'])

    @unittest.skipIf(WIN,'POSIX-only path semantics')
    def test_no_clobber_race_symlinks_and_fifo(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp);src=temp/'in.bmp';src.write_bytes(rfi_ref.write_bmp24(16,8,KAT_RGB))
            dest=temp/'out.fold';exe=OUT/'bin/rasterfold'
            def encode(_):return subprocess.run([exe,'encode',src,dest],capture_output=True,timeout=15).returncode
            with ThreadPoolExecutor(max_workers=2) as pool:codes=list(pool.map(encode,range(2)))
            self.assertEqual(sorted(codes),[0,2]);self.assertEqual(dest.read_bytes(),KAT_FOLD3)
            self.assertFalse(Path(str(dest)+'.partial').exists())
            dest.unlink();dest.symlink_to(temp/'missing')
            self.assertEqual(encode(0),2);self.assertTrue(dest.is_symlink());dest.unlink()
            partial=Path(str(dest)+'.partial');partial.write_bytes(b'other writer')
            self.assertEqual(encode(0),2);self.assertEqual(partial.read_bytes(),b'other writer');self.assertFalse(dest.exists())
            fifo=temp/'pipe';os.mkfifo(fifo)
            result=subprocess.run([exe,'inspect',fifo],capture_output=True,timeout=3)
            self.assertEqual(result.returncode,2)

    @unittest.skipIf(WIN,'POSIX failure injection')
    def test_publish_fallback_and_post_publish_errors(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp);src=temp/'in.bmp';src.write_bytes(rfi_ref.write_bmp24(16,8,KAT_RGB))
            variants={
                'fallback':('long syscall(long n, ...) { (void)n; errno=ENOSYS; return -1; }',0,b''),
                'flush':('int fsync(int fd) { struct stat s; if(!fstat(fd,&s) && S_ISDIR(s.st_mode)){errno=EIO;return -1;}return 0;}',2,b'output published but directory flush failed'),
                'unsupported_flush':('int fsync(int fd) { struct stat s; if(!fstat(fd,&s) && S_ISDIR(s.st_mode)){errno=EINVAL;return -1;}return 0;}',0,b''),
                'cleanup':('long syscall(long n, ...) { (void)n;errno=ENOSYS;return -1;} int unlinkat(int d, const char *p, int f){(void)d;(void)p;(void)f;errno=EPERM;return -1;}',2,b'output published but temporary cleanup failed'),
            }
            for name,(body,code,message) in variants.items():
                with self.subTest(name=name):
                    source=temp/(name+'.c');lib=temp/(name+'.so');dest=temp/(name+'.fold')
                    source.write_text('#include <errno.h>\n#include <sys/stat.h>\n#include <unistd.h>\n'+body)
                    run([tool('clang'),'--target=x86_64-linux-gnu','-shared','-fPIC',source,'-o',lib])
                    env=dict(os.environ,LD_PRELOAD=str(lib))
                    result=subprocess.run([OUT/'bin/rasterfold','encode',src,dest],env=env,capture_output=True,timeout=15)
                    self.assertEqual(result.returncode,code,result.stderr)
                    self.assertIn(message,result.stderr);self.assertEqual(dest.read_bytes(),KAT_FOLD3)
            for mode,err_code in (('renameat2','EEXIST'),('linkat','ENOSYS')):
                with self.subTest(name=f'parent_retarget_{mode}'):
                    dir_a=temp/f'dir_a_{mode}';dir_b=temp/f'dir_b_{mode}';dir_a.mkdir();dir_b.mkdir()
                    (dir_b/'out.fold').write_bytes(b'keep-b-fold')
                    (dir_b/'out.fold.partial').write_bytes(b'keep-b-partial')
                    parent_link=temp/f'parent_link_{mode}';parent_link.symlink_to(dir_a)
                    retarget_src=temp/f'retarget_{mode}.c';retarget_so=temp/f'retarget_{mode}.so'
                    retarget_src.write_text(
                        '#include <errno.h>\n#include <fcntl.h>\n#include <stdlib.h>\n#include <unistd.h>\n'
                        'long syscall(long n, ...) {\n'
                        '    (void)n;\n'
                        '    const char *link_path = getenv("RF_TEST_PARENT_LINK");\n'
                        '    const char *dir_b = getenv("RF_TEST_DIR_B");\n'
                        '    const char *collide_a = getenv("RF_TEST_COLLIDE_A");\n'
                        '    if (collide_a) { int cfd = open(collide_a, O_WRONLY | O_CREAT | O_EXCL, 0666); if (cfd >= 0) close(cfd); }\n'
                        '    if (link_path && dir_b) { unlink(link_path); symlink(dir_b, link_path); }\n'
                        f'    errno = {err_code};\n'
                        '    return -1;\n'
                        '}\n'
                    )
                    run([tool('clang'),'--target=x86_64-linux-gnu','-shared','-fPIC',retarget_src,'-o',retarget_so])
                    env=dict(
                        os.environ,
                        LD_PRELOAD=str(retarget_so),
                        RF_TEST_PARENT_LINK=str(parent_link),
                        RF_TEST_DIR_B=str(dir_b),
                        RF_TEST_COLLIDE_A=str(dir_a/'out.fold'),
                    )
                    result=subprocess.run([OUT/'bin/rasterfold','encode',src,parent_link/'out.fold'],env=env,capture_output=True,timeout=15)
                    self.assertEqual(result.returncode,2,result.stderr)
                    self.assertEqual((dir_b/'out.fold').read_bytes(),b'keep-b-fold')
                    self.assertEqual((dir_b/'out.fold.partial').read_bytes(),b'keep-b-partial')
                    self.assertFalse((dir_a/'out.fold.partial').exists())

    def test_pgo_lifecycle_and_validation(self):
        import json
        import stat
        import sys
        import time
        for extra_dir in (ROOT / 'scripts', ROOT / 'research'):
            if str(extra_dir) not in sys.path:
                sys.path.insert(0, str(extra_dir))
        import build as build_mod  # noqa: E402
        import pgo as pgo_mod  # noqa: E402
        import vsjxl as vsjxl_mod  # noqa: E402

        pgo_tools = ROOT / ('tools/llvm-mingw/bin' if WIN else 'tools/llvm-mingw-linux/bin')
        suffix = '.exe' if WIN else ''
        clang_bin = pgo_tools / ('clang' + suffix)

        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temp:
            temp_dir = Path(temp)
            # 1. Synthetic fallback training BMPs cover 10 regimes and decode cleanly via rfi_ref.
            syn_dir = temp_dir / 'syn'
            syn_dir.mkdir()
            bmps = pgo_mod.synthetic_training_bmps(syn_dir)
            self.assertEqual(len(bmps), 10)
            w0, h0, rgb0 = rfi_ref.read_bmp24(bmps[0].read_bytes())
            self.assertEqual((w0, h0), (256, 192))
            self.assertEqual(len(rgb0), 256 * 192 * 3)

            # 2. Build pgo-gen and verify executable provenance metadata is emitted and validated.
            build_mod.build_pgo_gen(2)
            self.assertTrue(pgo_mod.PGO_EXE.is_file())
            build_mod.validate_pgo_exe_provenance(pgo_mod.PGO_EXE, clang_bin)

            if WIN:
                train_exe = pgo_mod.PGO_EXE
            else:
                train_exe = temp_dir / 'host_pgo_runner'
                train_exe.write_text(
                    f'#!{sys.executable}\n'
                    'import os, subprocess, sys\n'
                    'from pathlib import Path\n'
                    f'rc = subprocess.call([{str(OUT / "bin" / "rasterfold")!r}, *sys.argv[1:]])\n'
                    'if rc == 0 and os.environ.get("LLVM_PROFILE_FILE"):\n'
                    '    Path(os.environ["LLVM_PROFILE_FILE"]).write_text(\n'
                    '        "__rf_pgo_synthetic_probe\\n# Func Hash:\\n0\\n# Num Counters:\\n1\\n# Counter Values:\\n1\\n\\n",\n'
                    '        encoding="utf-8",\n'
                    '    )\n'
                    'raise SystemExit(rc)\n',
                    encoding='utf-8',
                )
                train_exe.chmod(train_exe.stat().st_mode | stat.S_IXUSR)
                build_mod.write_pgo_exe_provenance(train_exe, clang_bin)

            # 3. Repeated profile generation (max_images=1) produces valid .profdata and .profdata.json.
            prof_out = temp_dir / 'test.profdata'
            meta_out = Path(str(prof_out) + '.json')
            pgo_mod.generate_profile(prof_out, build_instrumented=False, pgo_exe=train_exe, max_images=1)
            self.assertTrue(prof_out.is_file() and prof_out.stat().st_size > 0)
            self.assertTrue(meta_out.is_file())
            pgo_mod.generate_profile(prof_out, build_instrumented=False, pgo_exe=train_exe, max_images=1)
            second_bytes = prof_out.read_bytes()
            second_meta = meta_out.read_text(encoding='utf-8')
            self.assertGreater(len(second_bytes), 0)
            self.assertEqual(build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix), prof_out.resolve())

            # 4. Stale executable provenance (--no-build after source/header change) is rejected before training.
            exe_meta_path = Path(str(train_exe.resolve()) + '.json')
            saved_exe_meta = exe_meta_path.read_text(encoding='utf-8')
            for tracked_key in ('src/cm.c', 'include/rasterfold.h'):
                stale_exe_meta = json.loads(saved_exe_meta)
                stale_exe_meta['sources'][tracked_key] = '0' * 64
                exe_meta_path.write_text(json.dumps(stale_exe_meta), encoding='utf-8')
                try:
                    with self.assertRaisesRegex(ValueError, f'stale or mismatched PGO executable provenance for {tracked_key}'):
                        pgo_mod.generate_profile(prof_out, build_instrumented=False, pgo_exe=train_exe, max_images=1)
                finally:
                    exe_meta_path.write_text(saved_exe_meta, encoding='utf-8')
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)

            # 5. Failed training run preserves both existing .profdata and .profdata.json unchanged.
            fail_exe = temp_dir / ('fail_pgo.bat' if WIN else 'fail_pgo')
            if WIN:
                fail_exe.write_text('@exit /b 2\r\n', encoding='utf-8')
            else:
                fail_exe.write_text(f'#!{sys.executable}\nraise SystemExit(2)\n', encoding='utf-8')
                fail_exe.chmod(fail_exe.stat().st_mode | stat.S_IXUSR)
            build_mod.write_pgo_exe_provenance(fail_exe, clang_bin)
            with self.assertRaises(RuntimeError):
                pgo_mod.generate_profile(prof_out, build_instrumented=False, pgo_exe=fail_exe, max_images=1)
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)

            # 6. Interrupted backup creation or live publication preserves the valid profile pair.
            staged_new = temp_dir / 'staged_new.profdata'
            staged_new.write_bytes(second_bytes + b'\x00')
            new_meta = json.loads(second_meta)
            new_meta['profdata_sha256'] = hashlib.sha256(staged_new.read_bytes()).hexdigest()
            bak_prof = Path(str(prof_out) + '.bak')
            bak_meta = Path(str(meta_out) + '.bak')

            def interrupt_on_bak_meta(src: Path, dst: Path) -> None:
                if dst == bak_meta:
                    dst.write_text('{truncated', encoding='utf-8')
                    raise OSError('simulated interruption during backup metadata creation')
                src.replace(dst)

            with self.assertRaisesRegex(OSError, 'simulated interruption during backup'):
                pgo_mod.publish_profile_pair(staged_new, prof_out, new_meta, replace_fn=interrupt_on_bak_meta)
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)
            self.assertEqual(build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix), prof_out.resolve())

            # Simulate an uncleaned truncated backup left by SIGKILL while the live pair is valid.
            bak_prof.write_bytes(second_bytes)
            bak_meta.write_text('{truncated', encoding='utf-8')
            self.assertEqual(build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix), prof_out.resolve())
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)
            self.assertFalse(bak_prof.exists())
            self.assertFalse(bak_meta.exists())

            def interrupt_on_meta(src: Path, dst: Path) -> None:
                if dst == meta_out:
                    raise OSError('simulated interruption before metadata replace')
                src.replace(dst)

            with self.assertRaisesRegex(OSError, 'simulated interruption before metadata'):
                pgo_mod.publish_profile_pair(staged_new, prof_out, new_meta, replace_fn=interrupt_on_meta)
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)
            self.assertEqual(build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix), prof_out.resolve())

            # Simulate a crash after live .profdata replacement where a complete backup pair exists on disk.
            bak_prof.write_bytes(second_bytes)
            bak_meta.write_text(second_meta, encoding='utf-8')
            prof_out.write_bytes(staged_new.read_bytes())
            self.assertEqual(build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix), prof_out.resolve())
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)
            self.assertFalse(bak_prof.exists())
            self.assertFalse(bak_meta.exists())

            # 7. Stale source or header provenance in .profdata.json is rejected by validate_pgo_profile.
            for tracked_key in ('src/cm.c', 'include/rasterfold.h'):
                stale_meta = json.loads(second_meta)
                stale_meta['sources'][tracked_key] = '0' * 64
                meta_out.write_text(json.dumps(stale_meta), encoding='utf-8')
                with self.assertRaisesRegex(ValueError, f'stale or mismatched PGO profile provenance for {tracked_key}'):
                    build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix)
            meta_out.write_text(second_meta, encoding='utf-8')

            # 7. Corrupt .profdata is refused by scripts/build.py --pgo-use.
            corrupt = temp_dir / 'corrupt.profdata'
            corrupt.write_bytes(b'not-a-valid-llvm-profdata')
            proc = subprocess.run(
                [sys.executable, str(ROOT / 'scripts' / 'build.py'), '--target', TARGET, '--pgo-use', str(corrupt)],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn('corrupt or incompatible PGO profile', proc.stderr)

            # 8. Corpus timing parser rejects MISMATCH rows and missing images.
            with self.assertRaisesRegex(RuntimeError, 'bench round-trip failure'):
                vsjxl_mod.parse_bench_rows(
                    'kodim01.ppm       1000  bpp=1.0000  enc=   10.0ms dec=   10.0ms  (100/100 cyc/sample) MISMATCH\n',
                    ('kodim01',),
                )

            # 9. Implicit Ninja dependency on .profdata triggers object rebuild when profile timestamp advances.
            manifest = ROOT / 'build' / 'windows-x64' / 'build.ninja'
            try:
                build_mod.build('windows-x64', 2, False, pgo_profile=prof_out)
                self.assertIn(f'| {build_mod.ninja_path(prof_out.resolve())}', manifest.read_text(encoding='utf-8'))
                future = time.time() + 10.0
                os.utime(prof_out, (future, future))
                ninja_bin = ROOT / ('tools/ninja/ninja.exe' if WIN else 'tools/ninja-linux/ninja')
                dry = subprocess.run([str(ninja_bin), '-f', str(manifest), '-n'], cwd=ROOT, capture_output=True, text=True, check=True)
                self.assertIn('CC cm', dry.stdout)
            finally:
                build_mod.build('windows-x64', 2, False, pgo_profile=None)

    def test_lto_build_imports_and_consumers(self):
        import sys
        if str(ROOT / 'scripts') not in sys.path:
            sys.path.insert(0, str(ROOT / 'scripts'))
        import build as build_mod  # noqa: E402

        targets = ['windows-x64'] if WIN else [t for t in build_mod.TARGETS if (ROOT / 'bin' / t).is_dir() or t in (TARGET, 'windows-x64')]
        try:
            for t in targets:
                build_mod.build(t, 2, False, lto=True)
            manifest = (ROOT / 'build' / 'windows-x64' / 'build.ninja').read_text(encoding='utf-8')
            self.assertIn('rule cc_cm_lto', manifest)
            self.assertIn(' -flto ', manifest)
            exe_pe = run([tool('llvm-objdump'), '-p', ROOT / 'bin/windows-x64/bin/rasterfold.exe']).decode()
            self.assertEqual(set(re.findall(r'DLL Name:\s*(\S+)', exe_pe)), {'KERNEL32.dll', 'SHELL32.dll'})
            dll_pe = run([tool('llvm-objdump'), '-p', ROOT / 'bin/windows-x64/bin/rasterfold.dll']).decode()
            self.assertEqual(set(re.findall(r'DLL Name:\s*(\S+)', dll_pe)), {'KERNEL32.dll'})
            self.test_public_exports()
            self.test_c_cpp_static_and_dynamic_consumers()
            for case in ('known-answer', 'odd3', 'corrupt', 'truncated-abort'):
                run([sys.executable, ROOT / 'tests/test_correctness.py', '-k', case], cwd=ROOT)
            if not WIN:
                self.test_no_clobber_race_symlinks_and_fifo()
        finally:
            for t in targets:
                build_mod.build(t, 2, False, lto=False)


if __name__=='__main__':unittest.main(verbosity=2)
