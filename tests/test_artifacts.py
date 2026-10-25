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


if __name__=='__main__':unittest.main(verbosity=2)
