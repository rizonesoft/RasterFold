"""Installer boundary tests with tiny local archives, never network downloads."""
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
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

    def test_scope_converter_filtering_in_manifest(self):
        manifest=json.loads((Path(__file__).resolve().parents[1]/'toolchain.json').read_text())
        for group in (manifest['components'],manifest['hosts']['linux-x86_64']['components']):
            non_conv=[c['name'] for c in group if c.get('scope')!='converter']
            conv=[c['name'] for c in group if c.get('scope')=='converter']
            self.assertGreaterEqual(len(non_conv),2)
            self.assertTrue(any(n.startswith('cmake') for n in conv))
            self.assertTrue(any(n.startswith('nasm') for n in conv))
            self.assertTrue(all(not n.startswith(('cmake','nasm')) for n in non_conv))


if __name__=='__main__':unittest.main()
