"""Exercise release rejection paths without building the application."""

import hashlib
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

import release


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'src').mkdir()
        (self.root / 'CMakeLists.txt').write_text('project(FastPDF VERSION 0.1.0 LANGUAGES CXX)')
        (self.root / 'src/app.rc').write_text(
            'FILEVERSION 0,1,0,0\nPRODUCTVERSION 0,1,0,0\nVALUE "FileVersion", "0.1.0\\0"\n')
        (self.root / 'src/app.manifest').write_text(
            '<assemblyIdentity version="0.1.0.0" processorArchitecture="amd64" name="FastPDF.Reader"/>')
        (self.root / 'CHANGELOG.md').write_text('## v0.1.0\n\n- First release.\n\n## v0.0.1\n\n- Old.\n')
        root_patch = patch.object(release, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        sha_patch = patch.object(release, 'commit_sha', return_value='a' * 40)
        sha_patch.start()
        self.addCleanup(sha_patch.stop)

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

    def stage_package(self):
        self.stage = self.root / 'FastPDF-x64'
        binary = bytearray(70)
        binary[:2] = b'MZ'
        struct.pack_into('<I', binary, 60, 64)
        binary[64:] = b'PE\0\0\x64\x86'
        for name in release.FILES:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(binary if name.endswith('.exe') else b'document')
        release.create_manifest(self.stage)

    def archive_package(self, extra=False):
        path = self.root / release.ARCHIVE
        with zipfile.ZipFile(path, 'w') as archive:
            for file in self.stage.rglob('*'):
                if file.is_file():
                    archive.write(file, file.relative_to(self.root).as_posix())
            if extra:
                archive.writestr('FastPDF-x64/unexpected.dll', b'extra')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        (self.root / 'SHA256SUMS.txt').write_text(digest + '  ' + release.ARCHIVE + '\n')

    def test_verifies_complete_archive(self):
        self.stage_package()
        self.archive_package()
        release.verify_package(self.root)

    def test_rejects_wrong_commit(self):
        self.stage_package()
        self.archive_package()
        with patch.object(release, 'commit_sha', return_value='b' * 40), self.assertRaises(ValueError):
            release.verify_package(self.root)

    def test_rejects_corrupt_checksum(self):
        self.stage_package()
        self.archive_package()
        (self.root / 'SHA256SUMS.txt').write_text('0' * 64 + '  ' + release.ARCHIVE)
        with self.assertRaises(ValueError):
            release.verify_package(self.root)

    def test_rejects_modified_payload(self):
        self.stage_package()
        (self.stage / 'README.md').write_text('tampered')
        self.archive_package()
        with self.assertRaisesRegex(ValueError, 'file checksum'):
            release.verify_package(self.root)

    def test_rejects_unexpected_files(self):
        self.stage_package()
        self.archive_package(extra=True)
        with self.assertRaisesRegex(ValueError, 'unexpected files'):
            release.verify_package(self.root)

    def test_rejects_non_x64_binary(self):
        self.stage_package()
        binary = bytearray((self.stage / 'fastpdf.exe').read_bytes())
        binary[-2:] = b'\x4c\x01'  # IMAGE_FILE_MACHINE_I386
        (self.stage / 'fastpdf.exe').write_bytes(binary)
        with self.assertRaisesRegex(ValueError, 'Windows x64'):
            release.create_manifest(self.stage)


if __name__ == '__main__':
    unittest.main()
