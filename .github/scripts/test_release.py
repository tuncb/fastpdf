"""Exercise standalone release verification without building the application."""

import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import release


def pe_binary(resources):
    """Small PE32+ fixture with a real RCDATA resource tree and section mapping."""
    count = len(resources)
    languages = 40 + 8 * count
    records = languages + 24 * count
    payload = records + 16 * count
    section = bytearray(payload)
    struct.pack_into('<HII', section, 14, 1, 10, 0x80000018)
    struct.pack_into('<H', section, 38, count)
    for index, (identifier, content) in enumerate(resources.items()):
        language = languages + 24 * index
        record = records + 16 * index
        struct.pack_into('<II', section, 40 + 8 * index, identifier, language | 0x80000000)
        struct.pack_into('<HII', section, language + 14, 1, 1033, record)
        struct.pack_into('<IIII', section, record, 0x1000 + len(section), len(content), 0, 0)
        section.extend(content)
        section.extend(b'\0' * (-len(section) % 4))
    binary = bytearray(512)
    binary[:2] = b'MZ'
    struct.pack_into('<I', binary, 60, 128)
    binary[128:132] = b'PE\0\0'
    struct.pack_into('<HH', binary, 132, 0x8664, 1)
    struct.pack_into('<H', binary, 148, 240)
    struct.pack_into('<H', binary, 152, 0x20b)
    struct.pack_into('<I', binary, 260, 16)
    struct.pack_into('<II', binary, 280, 0x1000, len(section))
    binary[392:400] = b'.rsrc\0\0\0'
    struct.pack_into('<IIII', binary, 400, len(section), 0x1000, len(section), 512)
    return binary + section


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'src').mkdir()
        (self.root / 'docs').mkdir()
        self.output = self.root / 'release'
        self.output.mkdir()
        (self.root / 'CMakeLists.txt').write_text('project(FastPDF VERSION 0.1.0 LANGUAGES CXX)')
        (self.root / 'src/app.rc').write_text(
            'FILEVERSION 0,1,0,0\nPRODUCTVERSION 0,1,0,0\nVALUE "FileVersion", "0.1.0\\0"\n')
        (self.root / 'src/app.manifest').write_text(
            '<assemblyIdentity version="0.1.0.0" processorArchitecture="amd64" name="FastPDF.Reader"/>')
        (self.root / 'CHANGELOG.md').write_text('## v0.1.0\n\n- First release.\n\n## v0.0.1\n\n- Old.\n')
        self.metadata = dict(version='0.1.0', commit='a' * 40, platform='windows-x64')
        self.resources = {release.BUILD_METADATA: json.dumps(self.metadata).encode()}
        for identifier, name in release.LICENSES.items():
            content = f'Notice {identifier}\nCopyright © holder\n'.encode('utf-8')
            (self.root / name).write_bytes(content)
            self.resources[identifier] = content
        root_patch = patch.object(release, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        sha_patch = patch.object(release, 'commit_sha', return_value='a' * 40)
        sha_patch.start()
        self.addCleanup(sha_patch.stop)

    def package(self, binary=None):
        if binary is None:
            binary = pe_binary(self.resources)
        (self.output / release.EXECUTABLE).write_bytes(binary)
        digest = hashlib.sha256(binary).hexdigest()
        (self.output / 'SHA256SUMS.txt').write_text(digest + '  ' + release.EXECUTABLE + '\n')

    def test_tag_and_notes(self):
        release.validate_tag('v0.1.0')
        self.assertEqual(release.release_notes('v0.1.0'), '- First release.')
        for tag in ('0.1.0', 'v0.2.0', 'v0.1.0-rc.1', 'v00.1.0'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.validate_tag(tag)

    def test_rejects_inconsistent_version_fields(self):
        for name in ('src/app.rc', 'src/app.manifest'):
            path = self.root / name
            original = path.read_text()
            path.write_text(original.replace('0.1.0', '0.2.0').replace('0,1,0', '0,2,0'))
            with self.subTest(name=name), self.assertRaises(ValueError):
                release.validate_tag('v0.1.0')
            path.write_text(original)

    def test_rejects_missing_notes(self):
        (self.root / 'CHANGELOG.md').write_text('## v0.1.0\n\n')
        with self.assertRaises(ValueError):
            release.validate_tag('v0.1.0')

    def test_verifies_standalone_executable(self):
        self.package()
        release.verify_package(self.output)

    def test_verifies_windows_notices_on_linux(self):
        for identifier in release.LICENSES:
            self.resources[identifier] = self.resources[identifier].replace(b'\n', b'\r\n')
        self.package()
        release.verify_package(self.output)

    def test_rejects_wrong_build_metadata(self):
        for key, value in [('version', '0.2.0'), ('commit', 'b' * 40), ('platform', 'windows-arm64')]:
            self.resources[release.BUILD_METADATA] = json.dumps({**self.metadata, key: value}).encode()
            self.package()
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'version, commit, or platform'):
                release.verify_package(self.output)

    def test_rejects_corrupt_checksum(self):
        self.package()
        (self.output / 'SHA256SUMS.txt').write_text('0' * 64 + '  ' + release.EXECUTABLE)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            release.verify_package(self.output)

    def test_rejects_modified_payload(self):
        self.package()
        with (self.output / release.EXECUTABLE).open('ab') as executable:
            executable.write(b'tampered')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            release.verify_package(self.output)

    def test_rejects_unexpected_files(self):
        self.package()
        (self.output / 'pdfprobe.exe').write_bytes(b'old tool')
        with self.assertRaisesRegex(ValueError, 'unexpected files'):
            release.verify_package(self.output)

    def test_rejects_non_x64_binary(self):
        binary = pe_binary(self.resources)
        binary[132:134] = b'\x4c\x01'
        self.package(binary)
        with self.assertRaisesRegex(ValueError, 'Windows x64'):
            release.verify_package(self.output)

    def test_rejects_missing_resources(self):
        for identifier in self.resources:
            resources = {key: value for key, value in self.resources.items() if key != identifier}
            self.package(pe_binary(resources))
            with self.subTest(identifier=identifier), self.assertRaisesRegex(ValueError, 'resource directory'):
                release.verify_package(self.output)

    def test_rejects_modified_notices(self):
        for identifier in release.LICENSES:
            resources = {**self.resources, identifier: b'incomplete notice'}
            self.package(pe_binary(resources))
            with self.subTest(identifier=identifier), self.assertRaisesRegex(ValueError, 'Embedded notice'):
                release.verify_package(self.output)

    def test_rejects_truncated_headers_and_resources(self):
        binary = pe_binary(self.resources)
        for length in (0, 63, 135, 200, 410, len(binary) - 1):
            self.package(binary[:length])
            with self.subTest(length=length), self.assertRaises(ValueError):
                release.verify_package(self.output)

    def test_rejects_resource_outside_section(self):
        binary = pe_binary(self.resources)
        struct.pack_into('<I', binary, 512 + 136, 0xfffffff0)
        self.package(binary)
        with self.assertRaisesRegex(ValueError, 'outside section'):
            release.verify_package(self.output)

    def test_rejects_resource_directory_outside_section(self):
        binary = pe_binary(self.resources)
        struct.pack_into('<I', binary, 512 + 20, 0xfffffff0)
        self.package(binary)
        with self.assertRaisesRegex(ValueError, 'Truncated'):
            release.verify_package(self.output)


if __name__ == '__main__':
    unittest.main()
