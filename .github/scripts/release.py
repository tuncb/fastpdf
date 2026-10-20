"""Validate versions and portable Windows x64 packages (standard library only)."""

import hashlib
import json
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
MANIFEST = 'release-manifest.json'
ARCHIVE = 'FastPDF-x64.zip'
FILES = {
    'fastpdf.exe', 'pdfprobe.exe', 'README.md', 'CHANGELOG.md',
    'docs/ARCHITECTURE.md', 'docs/COMPATIBILITY.md', 'docs/VALIDATION.md',
    'docs/validation-corpus.json', 'docs/FONT-DATA-LICENSE.txt', 'docs/UNICODE-LICENSE.txt',
}


def repository_version():
    cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'project\(FastPDF VERSION (' + VERSION + r')\s', cmake)
    if not match:
        raise ValueError('Cannot determine CMake project version')
    version = match[1]
    rc = (ROOT / 'src/app.rc').read_text(encoding='utf-8')
    numeric = version.replace('.', ',') + ',0'
    for key in ('FILEVERSION', 'PRODUCTVERSION'):
        if not re.search(r'^' + key + r'\s+' + re.escape(numeric) + r'\s*$', rc, re.M):
            raise ValueError(f'{key} must match CMake version {version}')
    if f'VALUE "FileVersion", "{version}\\0"' not in rc:
        raise ValueError('FileVersion string must match CMake version')
    manifest = (ROOT / 'src/app.manifest').read_text(encoding='utf-8')
    if not re.search(r'<assemblyIdentity version="' + re.escape(version) + r'\.0" processorArchitecture="amd64" name="FastPDF.Reader"', manifest):
        raise ValueError('Application manifest must match CMake version and x64 architecture')
    return version


def release_notes(tag):
    text = (ROOT / 'CHANGELOG.md').read_text(encoding='utf-8')
    match = re.search(r'^## ' + re.escape(tag) + r'\s*\n(.*?)(?=^## |\Z)', text, re.M | re.S)
    if not match or not match[1].strip():
        raise ValueError(f'CHANGELOG.md needs release notes under ## {tag}')
    return match[1].strip()


def validate_tag(tag):
    if tag != 'v' + repository_version():
        raise ValueError('Release tag must match the application version: vX.Y.Z')
    release_notes(tag)


def commit_sha():
    return subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()


def validate_x64(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Package executable is not a PE binary')
    offset = struct.unpack_from('<I', data, 60)[0]
    if data[offset:offset + 6] != b'PE\0\0\x64\x86':
        raise ValueError('Package executable must target Windows x64')


def create_manifest(stage):
    version = repository_version()
    validate_tag('v' + version)
    actual = {p.relative_to(stage).as_posix() for p in stage.rglob('*') if p.is_file()}
    if actual - {MANIFEST} != FILES:
        raise ValueError('Package files differ from the required release file set')
    files = {}
    for name in sorted(FILES):
        data = (stage / name).read_bytes()
        if name.endswith('.exe'):
            validate_x64(data)
        files[name] = hashlib.sha256(data).hexdigest()
    manifest = dict(version=version, commit=commit_sha(), platform='windows-x64', files=files)
    (stage / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def verify_package(directory):
    version = repository_version()
    archive = directory / ARCHIVE
    expected = hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + ARCHIVE
    if (directory / 'SHA256SUMS.txt').read_text(encoding='ascii').strip() != expected:
        raise ValueError('Archive SHA-256 checksum does not match')
    prefix = 'FastPDF-x64/'
    with zipfile.ZipFile(archive) as package:
        entries = [entry.filename for entry in package.infolist() if not entry.is_dir()]
        if len(entries) != len(set(entries)) or set(entries) != {prefix + name for name in FILES | {MANIFEST}}:
            raise ValueError('Archive has missing, duplicate, or unexpected files')
        manifest = json.loads(package.read(prefix + MANIFEST))
        if (manifest['version'], manifest['commit'], manifest['platform']) != (version, commit_sha(), 'windows-x64'):
            raise ValueError('Archive version, commit, or platform does not match this checkout')
        if set(manifest['files']) != FILES:
            raise ValueError('Manifest file set does not match the release')
        for name, digest in manifest['files'].items():
            data = package.read(prefix + name)
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError('Archive file checksum mismatch: ' + name)
            if name.endswith('.exe'):
                validate_x64(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: release.py validate-tag TAG | notes TAG | manifest STAGE | verify DIRECTORY')
    command, argument = sys.argv[1:]
    if command == 'validate-tag':
        validate_tag(argument)
    elif command == 'notes':
        validate_tag(argument)
        print(release_notes(argument))
    elif command == 'manifest':
        create_manifest(Path(argument))
    elif command == 'verify':
        verify_package(Path(argument))
    else:
        raise SystemExit('Unknown command: ' + command)
