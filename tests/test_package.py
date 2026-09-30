"""Development packages must contain every required, hash-verified artifact."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec=importlib.util.spec_from_file_location('package',Path(__file__).resolve().parents[1]/'scripts/package.py')
package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)


class Packages(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.patch=patch.object(package,'ROOT',self.root)
        self.patch.start();self.addCleanup(self.patch.stop)
        for name in ('LICENSE','docs/building.md','docs/library.md'):
            path=self.root/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(name)

    def fixture(self,target):
        names=['include/rasterfold.h','lib/librasterfold.a']
        if target=='windows-x64':
            names+=['bin/rasterfold.exe','bin/rasterfold.dll','lib/librasterfold.dll.a',
                    'lib/rasterfold.lib','symbols/rasterfold.pdb','symbols/rasterfold-dll.pdb']
        else:
            names+=['bin/rasterfold','lib/librasterfold.so']
            if target=='linux-x64':names+=['lib/librasterfold.so.1']
        directory=self.root/'bin'/target
        artifacts={}
        for name in names:
            path=directory/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(name.encode())
            artifacts[name]=hashlib.sha256(path.read_bytes()).hexdigest()
        manifest={'schema':2,'target':target,'artifacts':artifacts}
        self.write_manifest(target,manifest)
        return manifest

    def write_manifest(self,target,manifest):
        (self.root/'bin'/target/'build.json').write_text(json.dumps(manifest))

    def test_complete_packages_for_every_target(self):
        for target in package.TARGETS:
            with self.subTest(target=target):
                manifest=self.fixture(target)
                with contextlib.redirect_stdout(io.StringIO()):package.package(target)
                dest=self.root/'build/packages'/f'rasterfold-{target}-dev.zip'
                with zipfile.ZipFile(dest) as archive:
                    prefix=f'rasterfold-{target}/'
                    expected=set(manifest['artifacts'])|{'build.json','LICENSE','docs/building.md','docs/library.md'}
                    self.assertEqual(set(archive.namelist()),{prefix+name for name in expected})
                    for name,digest in manifest['artifacts'].items():
                        self.assertEqual(hashlib.sha256(archive.read(prefix+name)).hexdigest(),digest)
                self.assertEqual(dest.with_suffix('.zip.sha256').read_text(),
                                 f'{hashlib.sha256(dest.read_bytes()).hexdigest()}  {dest.name}\n')

    def test_missing_required_manifest_entries_refused(self):
        for target in package.TARGETS:
            original=self.fixture(target)
            for name in original['artifacts']:
                with self.subTest(target=target,missing=name):
                    manifest={**original,'artifacts':dict(original['artifacts'])}
                    del manifest['artifacts'][name]
                    self.write_manifest(target,manifest)
                    with self.assertRaisesRegex(ValueError,'incomplete build manifest'):package.package(target)
        self.assertFalse((self.root/'build/packages').exists())

    def test_missing_or_empty_artifacts_refused(self):
        manifest=self.fixture('linux-x64')
        for artifacts in ({},None,[]):
            manifest['artifacts']=artifacts;self.write_manifest('linux-x64',manifest)
            with self.assertRaisesRegex(ValueError,'incomplete build manifest'):package.package('linux-x64')
        del manifest['artifacts'];self.write_manifest('linux-x64',manifest)
        with self.assertRaisesRegex(ValueError,'incomplete build manifest'):package.package('linux-x64')

    def test_tampered_artifact_refused_before_zip_write(self):
        self.fixture('windows-x64')
        (self.root/'bin/windows-x64/include/rasterfold.h').write_text('changed')
        with self.assertRaisesRegex(ValueError,'digest mismatch'):package.package('windows-x64')
        self.assertFalse((self.root/'build/packages').exists())

    def test_unsafe_manifest_path_refused(self):
        for name in ('../outside','/absolute','lib/../outside','lib\\outside','C:/outside'):
            with self.subTest(name=name):
                manifest=self.fixture('linux-x64');manifest['artifacts'][name]='0'*64
                self.write_manifest('linux-x64',manifest)
                with self.assertRaisesRegex(ValueError,'invalid artifact path'):package.package('linux-x64')


if __name__=='__main__':unittest.main(verbosity=2)
