"""Installer boundary tests with tiny local archives, never network downloads."""
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

spec=importlib.util.spec_from_file_location('bootstrap',Path(__file__).resolve().parents[1]/'tools/bootstrap.py')
bootstrap=importlib.util.module_from_spec(spec);spec.loader.exec_module(bootstrap)


def _tar_bytes(items):
    buf=io.BytesIO()
    with tarfile.open(fileobj=buf,mode='w:gz') as tf:
        for name,kind,data in items:
            ti=tarfile.TarInfo(name)
            if kind=='sym':
                ti.type=tarfile.SYMTYPE;ti.linkname=data
            else:
                raw=data.encode('utf-8') if isinstance(data,str) else data
                ti.size=len(raw);ti.mode=0o644
            tf.addfile(ti,None if kind=='sym' else io.BytesIO(raw))
    return buf.getvalue()


@unittest.skipIf(os.name=='nt','Linux-host installer fixtures')
class Toolchain(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.tools=Path(self.temp.name)
        self.archive=self.tools/'downloads/tool.zip';self.archive.parent.mkdir()

    def tearDown(self):self.temp.cleanup()

    def component(self, entries=None):
        with zipfile.ZipFile(self.archive,'w') as z:
            item=zipfile.ZipInfo('bin/tool');item.external_attr=(stat.S_IFREG|0o755)<<16
            z.writestr(item,'#!/bin/sh\necho fixture-1\n')
            for name,data in entries or []:z.writestr(name,data)
        return dict(dir='fixture',url='https://example.invalid/tool.zip',bytes=self.archive.stat().st_size,
                    sha256=hashlib.sha256(self.archive.read_bytes()).hexdigest(),probe='tool',versionArgs=['--version'],expectVersion='fixture-1')

    def test_install_verify_idempotence_and_tampering(self):
        c=self.component();bootstrap.install(c,self.tools);bootstrap.install(c,self.tools)
        bootstrap.verify(c,self.tools)
        (self.tools/'fixture/tool').write_text('#!/bin/sh\necho fixture-1\n# tamper\n')
        with self.assertRaisesRegex(ValueError,'probe changed'):bootstrap.verify(c,self.tools)

    def test_checksum_rejected_before_extraction(self):
        c=self.component();c['sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'checksum'):bootstrap.install(c,self.tools)
        self.assertFalse((self.tools/'fixture').exists())

    def test_archive_escape_rejected(self):
        c=self.component([('../escaped','unsafe')])
        with self.assertRaisesRegex(ValueError,'escapes'):bootstrap.install(c,self.tools)
        self.assertFalse((self.tools/'escaped').exists())

    def test_receipt_cannot_drop_probes(self):
        c=self.component();bootstrap.install(c,self.tools)
        p=self.tools/'fixture/PINNED.json';d=json.loads(p.read_text());d['probes']={};p.write_text(json.dumps(d))
        with self.assertRaisesRegex(ValueError,'probes'):bootstrap.verify(c,self.tools)

    def test_tar_traversal_and_symlink_escape_rejected(self):
        dl=self.tools/'downloads';dl.mkdir(parents=True,exist_ok=True)
        bad_trav=dl/'trav.tar.gz';bad_trav.write_bytes(_tar_bytes([('../escaped','reg','unsafe')]))
        c1=dict(dir='t1',url='https://example.invalid/trav.tar.gz',archive='trav.tar.gz',bytes=bad_trav.stat().st_size,
                sha256=hashlib.sha256(bad_trav.read_bytes()).hexdigest(),probe='tool',versionArgs=['--version'],expectVersion='1')
        with self.assertRaisesRegex(ValueError,'escapes'):bootstrap.install(c1,self.tools)
        bad_sym=dl/'sym.tar.gz';bad_sym.write_bytes(_tar_bytes([('sub/link','sym','../../outside')]))
        c2=dict(dir='t2',url='https://example.invalid/sym.tar.gz',archive='sym.tar.gz',bytes=bad_sym.stat().st_size,
                sha256=hashlib.sha256(bad_sym.read_bytes()).hexdigest(),probe='tool',versionArgs=['--version'],expectVersion='1')
        with self.assertRaisesRegex(ValueError,'escapes'):bootstrap.install(c2,self.tools)

    def test_vendor_moving_ref_and_branch_rejected(self):
        base=dict(id='libx',version='1.0.0',repo='https://example.invalid/x.git',tag='v1.0.0',
                  commit='a'*40,url='https://example.invalid/x.tar.gz',archive='x.tar.gz',
                  bytes=123,sha256='b'*64,bundled=True,license='MIT',license_file='converter/licenses/zlib.txt')
        for field,val,msg in [
            ('branch','main','branch'),
            ('tag','master','moving'),
            ('url','https://github.com/org/repo/archive/main.tar.gz','moving'),
            ('commit','v1.0.0','40-char'),
        ]:
            pkg=dict(base);pkg[field]=val
            with self.assertRaisesRegex(ValueError,msg):bootstrap.validate_vendor_package(pkg)

    def test_vendor_install_patch_verify_and_tampering(self):
        dl=self.tools/'downloads';dl.mkdir(parents=True,exist_ok=True)
        arc=dl/'libfoo-1.0.tar.gz'
        arc.write_bytes(_tar_bytes([('libfoo-1.0/foo.h','reg','line1\nline2\n')]))
        patch_raw=(
            '--- a/foo.h\t2026-01-01\n'
            '+++ b/foo.h\t2026-01-02\n'
            '@@ -1,2 +1,3 @@\n'
            ' line1\n'
            '+line1_patched\n'
            ' line2\n'
        ).encode('utf-8')
        pfile=dl/'libfoo-patch.gz';pfile.write_bytes(gzip.compress(patch_raw))
        pkg=dict(
            id='libfoo',version='1.0',repo='https://example.invalid/foo.git',tag='v1.0',
            commit='c'*40,url='https://example.invalid/libfoo-1.0.tar.gz',archive='libfoo-1.0.tar.gz',
            bytes=arc.stat().st_size,sha256=hashlib.sha256(arc.read_bytes()).hexdigest(),
            bundled=True,license='MIT',license_file='converter/licenses/zlib.txt',
            patches=[dict(name='apng-test',url='https://example.invalid/libfoo-patch.gz',archive='libfoo-patch.gz',
                          bytes=pfile.stat().st_size,sha256=hashlib.sha256(pfile.read_bytes()).hexdigest(),strip=1)],
        )
        csrc=self.tools/'converter-src'
        bootstrap.install_vendor_package(pkg,self.tools,csrc)
        bootstrap.install_vendor_package(pkg,self.tools,csrc)
        bootstrap.verify_vendor_package(pkg,csrc)
        self.assertEqual((csrc/'libfoo/foo.h').read_text(),'line1\nline1_patched\nline2\n')
        bad_pkg=dict(pkg,sha256='0'*64)
        with self.assertRaisesRegex(ValueError,'checksum|disagrees'):bootstrap.install_vendor_package(bad_pkg,self.tools,self.tools/'csrc2')
        (csrc/'libfoo/foo.h').write_text('tampered\n')
        with self.assertRaisesRegex(ValueError,'tree changed'):bootstrap.verify_vendor_package(pkg,csrc)

    def test_vendor_symlink_modification_and_escape_rejected(self):
        dl=self.tools/'downloads';dl.mkdir(parents=True,exist_ok=True)
        arc=dl/'libsym-1.0.tar.gz'
        arc.write_bytes(_tar_bytes([
            ('libsym-1.0/real.txt','reg','real-content\n'),
            ('libsym-1.0/other.txt','reg','other-content\n'),
            ('libsym-1.0/sub/link.txt','sym','../real.txt'),
        ]))
        pkg=dict(
            id='libsym',version='1.0',repo='https://example.invalid/sym.git',tag='v1.0',
            commit='d'*40,url='https://example.invalid/libsym-1.0.tar.gz',archive='libsym-1.0.tar.gz',
            bytes=arc.stat().st_size,sha256=hashlib.sha256(arc.read_bytes()).hexdigest(),
            bundled=True,license='MIT',license_file='converter/licenses/zlib.txt',
        )
        csrc=self.tools/'converter-src'
        bootstrap.install_vendor_package(pkg,self.tools,csrc)
        bootstrap.verify_vendor_package(pkg,csrc)

        link_path=csrc/'libsym/sub/link.txt'
        link_path.unlink();link_path.symlink_to('../other.txt')
        with self.assertRaisesRegex(ValueError,'tree changed'):bootstrap.verify_vendor_package(pkg,csrc)

        link_path.unlink();link_path.symlink_to('../../outside')
        with self.assertRaisesRegex(ValueError,'escapes root'):bootstrap.verify_vendor_package(pkg,csrc)

        link_path.unlink();link_path.symlink_to('../real.txt')
        bootstrap.verify_vendor_package(pkg,csrc)
        rcpt_path=csrc/'libsym/PINNED.json'
        rcpt_copy=self.tools/'rcpt.json'
        rcpt_copy.write_text(rcpt_path.read_text(encoding='utf-8'),encoding='utf-8')
        rcpt_path.unlink();rcpt_path.symlink_to(rcpt_copy)
        with self.assertRaisesRegex(ValueError,'PINNED.json'):bootstrap.verify_vendor_package(pkg,csrc)

    def test_scope_converter_filtering_and_tampering_behavior(self):
        repo_root=Path(__file__).resolve().parents[1]
        real_manifest=json.loads((repo_root/'toolchain.json').read_text(encoding='utf-8'))
        for group in (real_manifest['components'],real_manifest['hosts']['linux-x86_64']['components']):
            non_conv=[c['name'] for c in group if c.get('scope')!='converter']
            conv=[c['name'] for c in group if c.get('scope')=='converter']
            self.assertGreaterEqual(len(non_conv),2)
            self.assertTrue(any(n.startswith('cmake') for n in conv))
            self.assertTrue(any(n.startswith('nasm') for n in conv))
            self.assertTrue(all(not n.startswith(('cmake','nasm')) for n in non_conv))

        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            (root/'tools/githooks').mkdir(parents=True)
            (root/'scripts').mkdir()
            (root/'converter/licenses').mkdir(parents=True)
            shutil.copy2(repo_root/'tools/bootstrap.py',root/'tools/bootstrap.py')
            shutil.copy2(repo_root/'tools/provision.py',root/'tools/provision.py')
            for hook in ('pre-commit','commit-msg'):
                hp=root/'tools/githooks'/hook
                hp.write_text('#!/bin/sh\nexit 0\n',encoding='utf-8')
                hp.chmod(0o755)
            (root/'scripts/gen-compdb.py').write_text('import sys; sys.exit(0)\n',encoding='utf-8')
            subprocess.run(['git','init','-q'],cwd=root,check=True)
            subprocess.run(['git','config','core.hooksPath','tools/githooks'],cwd=root,check=True)

            dl=root/'tools/downloads';dl.mkdir()
            core_zip=dl/'core.zip'
            with zipfile.ZipFile(core_zip,'w') as z:
                it1=zipfile.ZipInfo('pkg/bin/tool');it1.external_attr=(stat.S_IFREG|0o755)<<16
                z.writestr(it1,'#!/bin/sh\necho core-1.0\n')
                it2=zipfile.ZipInfo('pkg/bin/helper');it2.external_attr=(stat.S_IFREG|0o755)<<16
                z.writestr(it2,'helper-v1\n')
            cmake_zip=dl/'cmake.zip'
            with zipfile.ZipFile(cmake_zip,'w') as z:
                it=zipfile.ZipInfo('pkg/bin/cmake');it.external_attr=(stat.S_IFREG|0o755)<<16
                z.writestr(it,'#!/bin/sh\necho "cmake version 4.5.0"\n')
            nasm_zip=dl/'nasm.zip'
            with zipfile.ZipFile(nasm_zip,'w') as z:
                it=zipfile.ZipInfo('pkg/bin/nasm');it.external_attr=(stat.S_IFREG|0o755)<<16
                z.writestr(it,'#!/bin/sh\necho "NASM version 3.02"\n')

            def mk_comp(name, folder, zpath, probe, probes, args, ver, scope=None):
                c=dict(name=name,dir=folder,url=f'https://example.invalid/{zpath.name}',
                       archive=zpath.name,bytes=zpath.stat().st_size,
                       sha256=hashlib.sha256(zpath.read_bytes()).hexdigest(),
                       probe=probe,probes=probes,versionArgs=args,expectVersion=ver)
                if scope: c['scope']=scope
                return c

            tc={
                'components': [],
                'hosts': {'linux-x86_64': {'components': [
                    mk_comp('coretool','coretool',core_zip,'bin/tool',['bin/tool','bin/helper'],['--version'],'core-1.0'),
                    mk_comp('cmake-linux','cmake-linux',cmake_zip,'bin/cmake',['bin/cmake'],['--version'],'4.5.0','converter'),
                    mk_comp('nasm-linux','nasm-linux',nasm_zip,'bin/nasm',['bin/nasm'],['-v'],'3.02','converter'),
                ]}},
            }
            (root/'toolchain.json').write_text(json.dumps(tc),encoding='utf-8')

            src_arc=dl/'tiny-1.0.tar.gz'
            src_arc.write_bytes(_tar_bytes([('tiny-1.0/tiny.h','reg','int tiny=1;\n')]))
            p_raw=('--- a/tiny.h\t2026-01-01\n+++ b/tiny.h\t2026-01-02\n'
                   '@@ -1 +1 @@\n-int tiny=1;\n+int tiny=2;\n').encode('utf-8')
            p_arc=dl/'tiny.patch.gz';p_arc.write_bytes(gzip.compress(p_raw))
            real_vendor=json.loads((repo_root/'converter/vendor.json').read_text(encoding='utf-8'))
            vendor={
                'schema': 1,
                'target': 'windows-x64',
                'system_codecs': [],
                'packages': [dict(
                    id='tiny',name='tiny',version='1.0',role='direct',formats=['png'],owner='D10 T01',
                    bundled=True,repo='https://example.invalid/tiny.git',tag='v1.0',commit='e'*40,
                    url='https://example.invalid/tiny-1.0.tar.gz',archive='tiny-1.0.tar.gz',
                    bytes=src_arc.stat().st_size,sha256=hashlib.sha256(src_arc.read_bytes()).hexdigest(),
                    license='MIT',license_file='converter/licenses/zlib.txt',
                    patches=[dict(name='tiny.patch.gz',url='https://example.invalid/tiny.patch.gz',
                                  archive='tiny.patch.gz',bytes=p_arc.stat().st_size,
                                  sha256=hashlib.sha256(p_arc.read_bytes()).hexdigest(),strip=1)],
                )],
                'runtime_libraries': real_vendor['runtime_libraries'],
            }
            (root/'converter/vendor.json').write_text(json.dumps(vendor),encoding='utf-8')

            # Core scope installs/verifies without converter tools or sources.
            subprocess.run([sys.executable,str(root/'tools/bootstrap.py')],cwd=root,check=True,capture_output=True)
            self.assertFalse((root/'tools/cmake-linux').exists())
            subprocess.run([sys.executable,str(root/'tools/provision.py'),'--verify'],cwd=root,check=True,capture_output=True)

            bs_conv_fail=subprocess.run([sys.executable,str(root/'tools/bootstrap.py'),'--converter','--verify'],
                                        cwd=root,capture_output=True,text=True)
            self.assertNotEqual(bs_conv_fail.returncode,0)
            pr_conv_fail=subprocess.run([sys.executable,str(root/'tools/provision.py'),'--converter','--verify'],
                                        cwd=root,capture_output=True,text=True)
            self.assertNotEqual(pr_conv_fail.returncode,0)

            # Full --converter installs cmake-linux, nasm-linux, and patched tiny source.
            subprocess.run([sys.executable,str(root/'tools/bootstrap.py'),'--converter'],cwd=root,check=True,capture_output=True)
            subprocess.run([sys.executable,str(root/'tools/provision.py'),'--converter','--verify'],cwd=root,check=True,capture_output=True)

            # Tamper secondary probe in coretool (version probe stays valid) -> fails default verify.
            (root/'tools/coretool/bin/helper').write_text('helper-tampered\n',encoding='utf-8')
            core_fail=subprocess.run([sys.executable,str(root/'tools/provision.py'),'--verify'],
                                     cwd=root,capture_output=True,text=True)
            self.assertNotEqual(core_fail.returncode,0)
            (root/'tools/coretool/bin/helper').write_text('helper-v1\n',encoding='utf-8')
            subprocess.run([sys.executable,str(root/'tools/provision.py'),'--verify'],cwd=root,check=True,capture_output=True)

            # Tamper cmake probe preserving --version output -> default still passes, --converter fails.
            cmake_bin=root/'tools/cmake-linux/bin/cmake'
            cmake_bin.write_text('#!/bin/sh\necho "cmake version 4.5.0"\n# tampered\n',encoding='utf-8')
            cmake_bin.chmod(0o755)
            subprocess.run([sys.executable,str(root/'tools/provision.py'),'--verify'],cwd=root,check=True,capture_output=True)
            conv_tamper=subprocess.run([sys.executable,str(root/'tools/provision.py'),'--converter','--verify'],
                                       cwd=root,capture_output=True,text=True)
            self.assertNotEqual(conv_tamper.returncode,0)
            self.assertIn('probe changed',conv_tamper.stdout+conv_tamper.stderr)

            # Restore cmake-linux, then verify changing manifest patch sha256 invalidates staged receipt.
            cmake_bin.write_text('#!/bin/sh\necho "cmake version 4.5.0"\n',encoding='utf-8')
            cmake_bin.chmod(0o755)
            vendor_bad_patch=json.loads((root/'converter/vendor.json').read_text(encoding='utf-8'))
            vendor_bad_patch['packages'][0]['patches'][0]['sha256']='f'*64
            (root/'converter/vendor.json').write_text(json.dumps(vendor_bad_patch),encoding='utf-8')
            bs_patch_fail=subprocess.run([sys.executable,str(root/'tools/bootstrap.py'),'--converter','--verify'],
                                         cwd=root,capture_output=True,text=True)
            self.assertNotEqual(bs_patch_fail.returncode,0)
            self.assertIn('receipt disagrees with manifest',bs_patch_fail.stdout+bs_patch_fail.stderr)

    def test_powershell_scope_receipt_patch_and_symlink_behavior(self):
        if os.name!='nt' and not shutil.which('powershell.exe'):
            self.skipTest('powershell.exe unavailable')
        repo_root=Path(__file__).resolve().parents[1]
        bs_ps1=repo_root/'tools/bootstrap.ps1'
        bs_ps1_win=(str(bs_ps1) if os.name=='nt'
                    else subprocess.check_output(['wslpath','-w',str(bs_ps1)],text=True).strip())

        with tempfile.TemporaryDirectory() as wsl_td:
            # 1) Over WSL share: exercise Test-VendorReceipt patches mismatch + real symlink tracking/escape.
            wsl_pkg_dir=Path(wsl_td)/'pkg'
            (wsl_pkg_dir/'sub').mkdir(parents=True)
            (wsl_pkg_dir/'real.txt').write_text('real\n',encoding='utf-8')
            (wsl_pkg_dir/'other.txt').write_text('other\n',encoding='utf-8')
            (wsl_pkg_dir/'sub/link.txt').symlink_to('../real.txt')
            tree_sha=bootstrap.tree_digest(wsl_pkg_dir)
            rcpt={
                'id':'pkg','version':'1.0','commit':'a'*40,'url':'https://example.invalid/pkg.tar.gz',
                'sha256':'b'*64,'bytes':100,'patches':[{'name':'p1','sha256':'c'*64}],'tree_sha256':tree_sha,
            }
            (wsl_pkg_dir/'PINNED.json').write_text(json.dumps(rcpt),encoding='utf-8')
            wsl_pkg_win=(str(wsl_pkg_dir) if os.name=='nt'
                         else subprocess.check_output(['wslpath','-w',str(wsl_pkg_dir)],text=True).strip())

            ps_cmd=(
                "$ErrorActionPreference='Stop'; "
                ". $env:RF_BS_PS1 -HelpersOnly; "
                "$pkgGood = [pscustomobject]@{ id='pkg'; version='1.0'; commit=('a'*40); "
                "url='https://example.invalid/pkg.tar.gz'; sha256=('b'*64); bytes=100; "
                "patches=@([pscustomobject]@{ name='p1'; sha256=('c'*64) }) }; "
                "$pkgBadPatch = [pscustomobject]@{ id='pkg'; version='1.0'; commit=('a'*40); "
                "url='https://example.invalid/pkg.tar.gz'; sha256=('b'*64); bytes=100; "
                "patches=@([pscustomobject]@{ name='p1'; sha256=('d'*64) }) }; "
                "if (-not (Test-VendorReceipt $pkgGood $env:RF_WSL_PKG)) { throw 'expected Test-VendorReceipt True' }; "
                "if (Test-VendorReceipt $pkgBadPatch $env:RF_WSL_PKG) { throw 'expected Test-VendorReceipt False on patch change' }; "
                "$tmpWin = Join-Path ([IO.Path]::GetTempPath()) ('rf_pstest_' + [guid]::NewGuid().ToString('N')); "
                "New-Item -ItemType Directory -Path $tmpWin | Out-Null; "
                "try { "
                "Set-Location $tmpWin; "
                "$tdir = Join-Path $tmpWin 'tools\\coretool'; "
                "New-Item -ItemType Directory -Path $tdir | Out-Null; "
                "'@echo fixture 1.0' | Set-Content -Encoding Ascii (Join-Path $tdir 'tool.cmd'); "
                "'helper' | Set-Content -Encoding Ascii (Join-Path $tdir 'helper.txt'); "
                "$hTool = Get-Sha256 (Join-Path $tdir 'tool.cmd'); "
                "$hHelp = Get-Sha256 (Join-Path $tdir 'helper.txt'); "
                "$pin = @{ url='u'; sha256=('e'*64); probes=@{ 'tool.cmd'=$hTool; 'helper.txt'=$hHelp } } | ConvertTo-Json -Depth 3; "
                "Write-Utf8NoBom (Join-Path $tdir 'PINNED.json') ($pin + \"`n\"); "
                "$cCore = [pscustomobject]@{ name='coretool'; dir='coretool'; url='u'; sha256=('e'*64); "
                "probe='tool.cmd'; probes=@('tool.cmd','helper.txt'); versionArgs=@(); expectVersion='fixture 1.0' }; "
                "$cConv = [pscustomobject]@{ name='cmake'; dir='cmake'; scope='converter'; url='u'; sha256=('f'*64); "
                "probe='cmake.exe'; probes=@('cmake.exe'); versionArgs=@(); expectVersion='4.5.0' }; "
                "$ok1 = Test-PinnedComponent $cCore (Join-Path $tmpWin 'tools'); "
                "if (-not $ok1.Ok) { throw \"expected core probe ok: $($ok1.Reason)\" }; "
                "$mList = @($cCore, $cConv); "
                "$defOk = $true; foreach ($mc in $mList) { if ($mc.PSObject.Properties['scope'] -and $mc.scope -eq 'converter') { continue }; if (-not (Test-PinnedComponent $mc (Join-Path $tmpWin 'tools')).Ok) { $defOk = $false } }; "
                "if (-not $defOk) { throw 'default scope should skip converter tool' }; "
                "$convOk = (Test-PinnedComponent $cConv (Join-Path $tmpWin 'tools')).Ok; "
                "if ($convOk) { throw 'converter scope should reject missing cmake' }; "
                "'tampered' | Set-Content -Encoding Ascii (Join-Path $tdir 'helper.txt'); "
                "$ok2 = Test-PinnedComponent $cCore (Join-Path $tmpWin 'tools'); "
                "if ($ok2.Ok -or $ok2.Reason -notlike '*probe changed*') { throw \"expected probe tamper rejection, got: $($ok2.Reason)\" }; "
                "$jRoot = Join-Path $tmpWin 'jtest'; $jSub = Join-Path $jRoot 'real'; "
                "New-Item -ItemType Directory -Path $jSub | Out-Null; "
                "& cmd.exe /c mklink /J (Join-Path $jRoot 'junc') $jSub | Out-Null; "
                "$jThrew = $false; try { $null = Get-TreeSha256 $jRoot } catch { if ($_.Exception.Message -like '*reparse point*') { $jThrew = $true } }; "
                "if (-not $jThrew) { throw 'expected Get-TreeSha256 to reject non-symlink reparse point' }; "
                "Write-Host 'PS_BEHAVIOR_OK' "
                "} finally { Set-Location $env:SystemRoot; if (Test-Path -LiteralPath $tmpWin) { Remove-Item -Recurse -Force -LiteralPath $tmpWin } }"
            )
            env=dict(os.environ)
            env['RF_BS_PS1']=bs_ps1_win
            env['RF_WSL_PKG']=wsl_pkg_win
            if os.name!='nt':
                ex=env.get('WSLENV','')
                env['WSLENV']=(ex+':' if ex else '')+'RF_BS_PS1:RF_WSL_PKG'
            res1=subprocess.run(
                ['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-Command',ps_cmd],
                env=env,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=60,
            )
            self.assertEqual(res1.returncode,0,res1.stdout+res1.stderr)
            self.assertIn('PS_BEHAVIOR_OK',res1.stdout)

            # Mutate POSIX symlink target in WSL tree -> PowerShell Test-VendorReceipt must reject.
            (wsl_pkg_dir/'sub/link.txt').unlink()
            (wsl_pkg_dir/'sub/link.txt').symlink_to('../other.txt')
            ps_chk=(
                "$ErrorActionPreference='Stop'; . $env:RF_BS_PS1 -HelpersOnly; "
                "$pkgGood = [pscustomobject]@{ id='pkg'; version='1.0'; commit=('a'*40); "
                "url='https://example.invalid/pkg.tar.gz'; sha256=('b'*64); bytes=100; "
                "patches=@([pscustomobject]@{ name='p1'; sha256=('c'*64) }) }; "
                "if (Test-VendorReceipt $pkgGood $env:RF_WSL_PKG) { throw 'expected False after symlink edit' }; "
                "Write-Host 'PS_SYMLINK_EDIT_OK'"
            )
            res2=subprocess.run(
                ['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-Command',ps_chk],
                env=env,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=60,
            )
            self.assertEqual(res2.returncode,0,res2.stdout+res2.stderr)
            self.assertIn('PS_SYMLINK_EDIT_OK',res2.stdout)

            # Escaping symlink -> PowerShell Test-VendorReceipt / Get-TreeSha256 must throw escape error.
            (wsl_pkg_dir/'sub/link.txt').unlink()
            (wsl_pkg_dir/'sub/link.txt').symlink_to('../../outside')
            ps_esc=(
                "$ErrorActionPreference='Stop'; . $env:RF_BS_PS1 -HelpersOnly; "
                "try { $null = Get-TreeSha256 $env:RF_WSL_PKG; throw 'unexpected success' } "
                "catch { if ($_.Exception.Message -notlike '*escapes root*') { throw }; Write-Host 'PS_SYMLINK_ESCAPE_OK' }"
            )
            res3=subprocess.run(
                ['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-Command',ps_esc],
                env=env,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=60,
            )
            self.assertEqual(res3.returncode,0,res3.stdout+res3.stderr)
            self.assertIn('PS_SYMLINK_ESCAPE_OK',res3.stdout)


if __name__=='__main__':unittest.main()
