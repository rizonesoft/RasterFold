"""Installer boundary tests with tiny local archives, never network downloads."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
import zipfile

spec=importlib.util.spec_from_file_location('bootstrap',Path(__file__).resolve().parents[1]/'tools/bootstrap.py')
bootstrap=importlib.util.module_from_spec(spec);spec.loader.exec_module(bootstrap)


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


if __name__=='__main__':unittest.main()
