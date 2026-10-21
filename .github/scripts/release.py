"""Validate versions and portable Windows x64 packages (standard library only)."""

import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
EXECUTABLE = 'fastpdf.exe'
# RCDATA IDs in src/resource.h. Read PE resources without loading/executing the app,
# so verification works on both Windows builds and Linux release runners.
BUILD_METADATA = 201
LICENSES = {202: 'docs/FONT-DATA-LICENSE.txt', 203: 'docs/UNICODE-LICENSE.txt'}


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


def embedded_resources(data):
    validate_x64(data)

    def read(blob, offset, size):
        if offset < 0 or size < 0 or offset + size > len(blob):
            raise ValueError('Truncated PE resource or header')
        return blob[offset:offset + size]

    def unpack(fmt, blob, offset):
        return struct.unpack(fmt, read(blob, offset, struct.calcsize(fmt)))

    pe = unpack('<I', data, 60)[0]
    sections = unpack('<H', data, pe + 6)[0]
    optional_size = unpack('<H', data, pe + 20)[0]
    optional = read(data, pe + 24, optional_size)
    if unpack('<H', optional, 0)[0] != 0x20b or unpack('<I', optional, 108)[0] < 3:
        raise ValueError('Executable must contain PE32+ resource metadata')
    resource_rva, resource_size = unpack('<II', optional, 128)
    section_table = read(data, pe + 24 + optional_size, sections * 40)

    def rva_bytes(rva, size):
        for offset in range(0, len(section_table), 40):
            _, address, raw_size, raw_offset = unpack('<IIII', section_table, offset + 8)
            delta = rva - address
            if 0 <= delta and delta + size <= raw_size:
                return read(data, raw_offset + delta, size)
        raise ValueError('PE resource points outside section data')

    resources = rva_bytes(resource_rva, resource_size)

    def entries(offset):
        named, numbered = unpack('<HH', resources, offset + 12)
        table = read(resources, offset + 16, (named + numbered) * 8)
        return list(struct.iter_unpack('<II', table))

    def directory(offset, identifier):
        matches = [target for name, target in entries(offset) if name == identifier]
        if len(matches) != 1 or not matches[0] & 0x80000000:
            raise ValueError(f'Missing or invalid embedded resource directory: {identifier}')
        return matches[0] & 0x7fffffff

    rcdata = directory(0, 10)  # RT_RCDATA
    result = {}
    for identifier in (BUILD_METADATA, *LICENSES):
        languages = entries(directory(rcdata, identifier))
        if len(languages) != 1 or languages[0][1] & 0x80000000:
            raise ValueError(f'Ambiguous or invalid embedded resource: {identifier}')
        rva, size, _, _ = unpack('<IIII', resources, languages[0][1])
        result[identifier] = rva_bytes(rva, size)
    return result


def verify_package(directory):
    version = repository_version()
    data = (directory / EXECUTABLE).read_bytes()
    expected = hashlib.sha256(data).hexdigest() + '  ' + EXECUTABLE
    if (directory / 'SHA256SUMS.txt').read_text(encoding='ascii').strip() != expected:
        raise ValueError('Executable SHA-256 checksum does not match')
    if {p.name for p in directory.iterdir()} != {EXECUTABLE, 'SHA256SUMS.txt'}:
        raise ValueError('Release has unexpected files')
    resources = embedded_resources(data)
    manifest = json.loads(resources[BUILD_METADATA])
    if manifest != dict(version=version, commit=commit_sha(), platform='windows-x64'):
        raise ValueError('Executable version, commit, or platform does not match this checkout')
    for identifier, name in LICENSES.items():
        # Windows and Linux Git checkouts can use different line endings.
        if resources[identifier].replace(b'\r\n', b'\n') != (ROOT / name).read_bytes().replace(b'\r\n', b'\n'):
            raise ValueError('Embedded notice does not match: ' + name)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: release.py validate-tag TAG | notes TAG | verify DIRECTORY')
    command, argument = sys.argv[1:]
    if command == 'validate-tag':
        validate_tag(argument)
    elif command == 'notes':
        validate_tag(argument)
        print(release_notes(argument))
    elif command == 'verify':
        verify_package(Path(argument))
    else:
        raise SystemExit('Unknown command: ' + command)
