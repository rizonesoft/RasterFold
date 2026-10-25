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

from test_correctness import KAT_FOLD, KAT_RGB
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
                run([commands[0],source,encoded]);self.assertEqual(encoded.read_bytes(),KAT_FOLD)
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
            'int main(void) {\n'
            '    const uint32_t w = 64, h = 64;\n'
            '    const size_t raw = (size_t)w * h * 3, each = (size_t)w * h + 64, work = 12 + 3 * each;\n'
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
            '    static const int tcounts[2] = {1, 3};\n'
            '    for (int ti = 0; ti < 2; ti++) {\n'
            '        int threads = tcounts[ti];\n'
            '        rf_error err = {0, 0};\n'
            '        /* 1. Overlapping compaction: n0 + n1 > each, each n_c <= each, total <= cap. */\n'
            '        memset(guard, 0xA5, 64);\n'
            '        memset(guard + 64, 0xCC, work);\n'
            '        memset(guard + 64 + work, 0x5A, 64);\n'
            '        size_t written = 0;\n'
            '        if (rf_cm_encode(rgb1, w, h, guard + 64, raw - 1, &written, threads, &err) != RF_OK) return 3;\n'
            '        if (!check_canaries(guard, work)) return 4;\n'
            '        uint32_t n0 = (uint32_t)guard[64] | ((uint32_t)guard[65] << 8) | ((uint32_t)guard[66] << 16) | ((uint32_t)guard[67] << 24);\n'
            '        uint32_t n1 = (uint32_t)guard[68] | ((uint32_t)guard[69] << 8) | ((uint32_t)guard[70] << 16) | ((uint32_t)guard[71] << 24);\n'
            '        uint32_t n2 = (uint32_t)guard[72] | ((uint32_t)guard[73] << 8) | ((uint32_t)guard[74] << 16) | ((uint32_t)guard[75] << 24);\n'
            '        if (written == 0 || written != 12u + n0 + n1 + n2 || (size_t)n0 + n1 <= each) return 5;\n'
            '        if (ti == 0) { ref_written = written; memcpy(ref_out, guard + 64, written); }\n'
            '        else if (written != ref_written || memcmp(ref_out, guard + 64, written) != 0) return 6;\n'
            '        memset(dec, 0, raw);\n'
            '        if (rf_cm_decode(guard + 64, written, dec, 0, w, h, threads, &err) != RF_OK) return 7;\n'
            '        if (memcmp(dec, rgb1, raw) != 0) return 8;\n'
            '        /* 2. Aggregate payload overflow: each stream fits <= each, but total > cap. */\n'
            '        memset(guard, 0xA5, 64);\n'
            '        memset(guard + 64, 0xCC, work);\n'
            '        memset(guard + 64 + work, 0x5A, 64);\n'
            '        written = 999;\n'
            '        if (rf_cm_encode(rgb1, w, h, guard + 64, ref_written - 1, &written, threads, &err) != RF_OK) return 9;\n'
            '        if (written != 0 || !check_canaries(guard, work)) return 10;\n'
            '        /* 3. Individual stream overflow (stream 0 > each while total < cap) and container raw fallback. */\n'
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
            '        if (!fold || fold_sz != RF_HEADER_SIZE + raw || fold[16] != RF_METHOD_RAW || memcmp(fold + RF_HEADER_SIZE, rgb2, raw) != 0) return 14;\n'
            '        rf_image round = {0, 0, 0};\n'
            '        if (rf_decode(fold, fold_sz, &round, 0, threads, &err) != RF_OK) return 15;\n'
            '        if (round.width != w || round.height != h || memcmp(round.rgb, rgb2, raw) != 0) return 16;\n'
            '        rf_free(round.rgb);\n'
            '        rf_free(fold);\n'
            '    }\n'
            '    rf_free(ref_out); rf_free(guard); rf_free(dec); rf_free(rgb2); rf_free(rgb1);\n'
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

    @unittest.skipIf(WIN,'POSIX-only path semantics')
    def test_no_clobber_race_symlinks_and_fifo(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            temp=Path(temp);src=temp/'in.bmp';src.write_bytes(rfi_ref.write_bmp24(16,8,KAT_RGB))
            dest=temp/'out.fold';exe=OUT/'bin/rasterfold'
            def encode(_):return subprocess.run([exe,'encode',src,dest],capture_output=True,timeout=15).returncode
            with ThreadPoolExecutor(max_workers=2) as pool:codes=list(pool.map(encode,range(2)))
            self.assertEqual(sorted(codes),[0,2]);self.assertEqual(dest.read_bytes(),KAT_FOLD)
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
                    self.assertIn(message,result.stderr);self.assertEqual(dest.read_bytes(),KAT_FOLD)
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

            # 6. Interrupted publication between .profdata and .profdata.json rolls back to the previous valid pair.
            staged_new = temp_dir / 'staged_new.profdata'
            staged_new.write_bytes(second_bytes + b'\x00')
            new_meta = json.loads(second_meta)
            new_meta['profdata_sha256'] = hashlib.sha256(staged_new.read_bytes()).hexdigest()

            def interrupt_on_meta(src: Path, dst: Path) -> None:
                if dst == meta_out:
                    raise OSError('simulated interruption before metadata replace')
                src.replace(dst)

            with self.assertRaisesRegex(OSError, 'simulated interruption'):
                pgo_mod.publish_profile_pair(staged_new, prof_out, new_meta, replace_fn=interrupt_on_meta)
            self.assertEqual(prof_out.read_bytes(), second_bytes)
            self.assertEqual(meta_out.read_text(encoding='utf-8'), second_meta)
            self.assertEqual(build_mod.validate_pgo_profile(prof_out, pgo_tools, suffix), prof_out.resolve())

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


if __name__=='__main__':unittest.main(verbosity=2)
