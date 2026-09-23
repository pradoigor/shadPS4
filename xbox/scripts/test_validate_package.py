# SPDX-License-Identifier: GPL-2.0-or-later
"""Negative tests for the deploy-artifact guard; these are not console tests."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from validate_package import validate


class PackageValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        exe = bytearray(128)
        struct.pack_into('<I', exe, 0x3c, 64)
        exe[64:68] = b'PE\0\0'
        struct.pack_into('<H', exe, 68, 0x8664)
        self.files = {
            'AppxManifest.xml': b'''<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">
              <Identity Name="PradoIgor.ShadPS4Xbox.Diagnostics" ProcessorArchitecture="x64" Publisher="CN=PradoIgor.ShadPS4Xbox"/>
              <Capabilities><Capability Name="codeGeneration"/></Capabilities></Package>''',
            'AppxSignature.p7x': b'fixture-only-not-a-real-signature',
            'MainPage.xaml': b'<Grid/>', 'ShadPS4Xbox.exe': bytes(exe),
        }
        angle_binaries = {'libEGL.dll': b'egl-fixture', 'libGLESv2.dll': b'gles-fixture'}
        self.files.update(angle_binaries)
        for name in ['Fonts/NotoSans-Regular.ttf', 'Fonts/NotoSansCJK-Regular.ttc',
                     'Licenses/Noto-OFL.txt', 'Licenses/FreeType-GPLv2.txt', 'Licenses/font-notices.md']:
            self.files[name] = b'fixture'
        self.files['Licenses/ANGLE.txt'] = b'license-fixture'
        self.files['Licenses/ANGLE-build-info.json'] = json.dumps({
            'target_os': 'winuwp', 'target_cpu': 'x64',
            'binaries': {name: hashlib.sha256(payload).hexdigest()
                         for name, payload in angle_binaries.items()},
        }).encode()

    def write(self):
        package = self.path / 'test.appx'
        with zipfile.ZipFile(package, 'w') as output:
            for name, value in self.files.items():
                output.writestr(name, value)
        info = self.path / 'build-info.json'
        info.write_text(json.dumps({'package_sha256': hashlib.sha256(package.read_bytes()).hexdigest(), 'commit': 'fixture'}))
        return package, info

    def test_structure_does_not_claim_signature_or_console_validation(self):
        result = validate(*self.write())
        self.assertTrue(result['structure_valid'])
        self.assertFalse(result['signature_cryptographically_verified'])
        self.assertFalse(result['console_tested'])

    def test_rejects_corrupted_download(self):
        package, info = self.write()
        package.write_bytes(package.read_bytes() + b'corruption')
        with self.assertRaisesRegex(ValueError, 'hash'):
            validate(package, info)

    def test_rejects_different_application(self):
        self.files['AppxManifest.xml'] = self.files['AppxManifest.xml'].replace(b'PradoIgor.ShadPS4Xbox.Diagnostics', b'Other.App')
        with self.assertRaisesRegex(ValueError, 'identity'):
            validate(*self.write())

    def test_rejects_x86_executable(self):
        exe = bytearray(self.files['ShadPS4Xbox.exe'])
        struct.pack_into('<H', exe, 68, 0x14c)
        self.files['ShadPS4Xbox.exe'] = bytes(exe)
        with self.assertRaisesRegex(ValueError, 'PE x64'):
            validate(*self.write())

    def test_rejects_private_key(self):
        self.files['signing.pfx'] = b'secret-fixture'
        with self.assertRaisesRegex(ValueError, 'Private signing key'):
            validate(*self.write())

    def test_rejects_added_capabilities(self):
        self.files['AppxManifest.xml'] = self.files['AppxManifest.xml'].replace(b'</Capabilities>', b'<Capability Name="internetClient"/></Capabilities>')
        with self.assertRaisesRegex(ValueError, 'capabilities'):
            validate(*self.write())

    def test_rejects_modified_angle_binary(self):
        self.files['libEGL.dll'] = b'modified'
        with self.assertRaisesRegex(ValueError, 'ANGLE binary mismatch'):
            validate(*self.write())

    def test_rejects_missing_runtime_font(self):
        del self.files['Fonts/NotoSans-Regular.ttf']
        with self.assertRaisesRegex(ValueError, 'runtime font'):
            validate(*self.write())


if __name__ == '__main__':
    unittest.main()
