#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the actual signed artifact, not just source configuration."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import zipfile

IDENTITY = 'PradoIgor.ShadPS4Xbox.Diagnostics'
NS = {'p': 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'}


def validate(package, info_path):
    info = json.loads(info_path.read_text(encoding='utf-8-sig'))
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    if digest.lower() != info['package_sha256'].lower():
        raise ValueError('APPX hash does not match build-info.json')
    with zipfile.ZipFile(package) as appx:
        names = set(appx.namelist())
        for name in ['AppxManifest.xml', 'AppxSignature.p7x', 'MainPage.xaml', 'ShadPS4Xbox.exe']:
            if name not in names:
                raise ValueError(f'Missing packaged resource: {name}')
        manifest = ET.fromstring(appx.read('AppxManifest.xml'))
        identity = manifest.find('p:Identity', NS)
        if identity.attrib['Name'] != IDENTITY or identity.attrib['ProcessorArchitecture'] != 'x64':
            raise ValueError('Unexpected identity or architecture')
        if identity.attrib['Publisher'] != 'CN=PradoIgor.ShadPS4Xbox':
            raise ValueError('Unexpected publisher')
        capabilities = {x.attrib['Name'] for x in manifest.findall('p:Capabilities/*', NS)}
        if capabilities != {'codeGeneration'}:
            raise ValueError(f'Unexpected capabilities: {capabilities}')
        exe = appx.read('ShadPS4Xbox.exe')
        offset = struct.unpack_from('<I', exe, 0x3c)[0]
        if exe[offset:offset+4] != b'PE\0\0' or struct.unpack_from('<H', exe, offset+4)[0] != 0x8664:
            raise ValueError('Executable is not PE x64')
        if any(name.lower().endswith('.pfx') for name in names):
            raise ValueError('Private signing key must never be packaged')
    return {'package': package.name, 'sha256': digest, 'commit': info['commit'],
            'structure_valid': True, 'signature_present': True,
            'signature_cryptographically_verified': False, 'console_tested': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    parser.add_argument('build_info', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.package, args.build_info), indent=2))
