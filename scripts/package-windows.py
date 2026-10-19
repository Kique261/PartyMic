#!/usr/bin/env python3
"""Check the cross-built PE and package the executable, documentation and notices."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
from zipfile import ZipFile, ZIP_DEFLATED


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', required=True, type=Path)
    args = parser.parse_args()
    toolchain = args.toolchain.resolve()
    root = Path(__file__).resolve().parents[1]
    version_match = re.search(r'project\(PartyMic VERSION ([0-9.]+)', (root / 'CMakeLists.txt').read_text())
    if not version_match:
        raise RuntimeError('Missing project version')
    version = version_match.group(1)
    executable = root / 'build-windows' / 'PartyMic.exe'
    data = executable.read_bytes()
    if data[:2] != b'MZ':
        raise RuntimeError('Missing DOS executable signature')
    pe_offset = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe_offset:pe_offset + 4] != b'PE\0\0':
        raise RuntimeError('Missing PE signature')
    optional_offset = pe_offset + 24
    machine = struct.unpack_from('<H', data, pe_offset + 4)[0]
    magic = struct.unpack_from('<H', data, optional_offset)[0]
    subsystem = struct.unpack_from('<H', data, optional_offset + 68)[0]
    if (machine, magic, subsystem) != (0x8664, 0x20b, 2):
        raise RuntimeError('Expected AMD64 PE32+ Windows GUI executable')
    if b'<requestedExecutionLevel level="asInvoker"' not in data:
        raise RuntimeError('Missing ordinary-user manifest')
    imports = subprocess.check_output([
        str(toolchain / 'bin' / 'llvm-readobj'), '--coff-imports', str(executable)
    ], text=True)
    dlls = re.findall(r'^  Name: (.+)$', imports, re.MULTILINE)
    system_dlls = {'ole32.dll', 'mmdevapi.dll', 'comctl32.dll', 'user32.dll', 'gdi32.dll', 'kernel32.dll', 'avrt.dll', 'shell32.dll', 'gdiplus.dll', 'msimg32.dll'}
    unexpected = [name for name in dlls if name.lower() not in system_dlls
                  and not name.lower().startswith('api-ms-win-crt-')]
    if not dlls or unexpected:
        raise RuntimeError(f'Unexpected DLL dependencies: {unexpected}')

    resources = subprocess.check_output([str(toolchain / 'bin' / 'llvm-readobj'), '--coff-resources', str(executable)], text=True)
    if 'Type: ICON (ID 3)' not in resources or 'Type: GROUP_ICON (ID 14)' not in resources:
        raise RuntimeError('Missing embedded application icon resources')
    output = root / 'dist'
    package = output / 'PartyMic-win11-x64'
    if package.exists():
        shutil.rmtree(package)
    package.mkdir(parents=True, exist_ok=True)
    shutil.copy2(executable, output / 'PartyMic.exe')
    shutil.copy2(executable, package / 'PartyMic.exe')
    shutil.copy2(root / 'README.md', package / 'README.md')
    shutil.copy2(root / 'CHANGELOG.md', package / 'CHANGELOG.md')
    quick_start = (root / 'docs' / 'quick-start-windows.txt').read_text(encoding='utf-8')
    (package / '使用说明.txt').write_bytes(b'\xef\xbb\xbf' + quick_start.replace('\n', '\r\n').encode('utf-8'))
    docs = package / 'docs'
    shutil.copytree(root / 'docs', docs, ignore=shutil.ignore_patterns('__pycache__'))
    runtime_record = root / 'docs' / 'test-results' / 'windows-runtime.json'
    runtime_evidence = json.loads(runtime_record.read_text()) if runtime_record.exists() else {}
    runtime_verified = (runtime_evidence.get('version') == version
                        and runtime_evidence.get('ui', {}).get('visual_passed') is True
                        and runtime_evidence.get('ui', {}).get('screen_sequence_passed') is True
                        and runtime_evidence.get('exe_sha256') == sha256(executable)
                        and runtime_evidence.get('gui_passed') is True
                        and runtime_evidence.get('core_tests_passed') is True
                        and runtime_evidence.get('audio_smoke_passed') is True
                        and runtime_evidence.get('audio_fidelity_passed') is True
                        and runtime_evidence.get('audio_soak_passed') is True
                        and runtime_evidence.get('audio_test_exe_sha256') ==
                            sha256(root / 'build-windows' / 'windows_audio_smoke.exe'))
    if runtime_record.exists():
        (docs / 'test-results').mkdir(exist_ok=True)
        shutil.copy2(runtime_record, docs / 'test-results' / runtime_record.name)
        for history in runtime_record.parent.glob('*.json'):
            shutil.copy2(history, docs / 'test-results' / history.name)
    licenses = package / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(toolchain / 'LICENSE.TXT', licenses / 'LLVM-LICENSE.txt')
    mingw_notices = toolchain / 'x86_64-w64-mingw32' / 'share' / 'mingw32'
    for name in ('COPYING.MinGW-w64-runtime.txt', 'COPYING.winpthreads.txt'):
        shutil.copy2(mingw_notices / name, licenses / name)
    build_info = {
        'version': version,
        'target': 'Windows 11 x64',
        'toolchain': toolchain.name,
        'compiler': subprocess.check_output([
            str(toolchain / 'bin' / 'x86_64-w64-mingw32-clang++'), '--version'
        ], text=True).splitlines()[0],
        'sha256': sha256(executable),
        'size_bytes': len(data),
        'format': 'PE32+ AMD64 Windows GUI',
        'execution_level': 'asInvoker',
        'import_dlls': dlls,
        'windows_runtime_tested': runtime_verified,
        'windows_runtime_scope': ('GUI, preview, synthetic Smoke/Fidelity/90-second Soak; '
                                  'not actual player, voice call or game/ACE validation') if runtime_verified else None,
        'windows_runtime_evidence': 'docs/test-results/windows-runtime.json' if runtime_verified else None,
        'ace_compatibility_tested': False,
    }
    (package / 'build-info.json').write_text(
        json.dumps(build_info, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    binary_zip = output / 'PartyMic-win11-x64.zip'
    with ZipFile(binary_zip, 'w', ZIP_DEFLATED) as archive:
        for path in sorted(package.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(output).as_posix())
    source_files = [root / name for name in ('CMakeLists.txt', 'README.md', 'CHANGELOG.md', '.gitignore', '.clang-format')]
    for folder in ('src', 'tests', 'docs', 'cmake', 'scripts', '.github/workflows'):
        source_files.extend(path for path in (root / folder).rglob('*')
                            if path.is_file() and '__pycache__' not in path.parts)
    source_zip = output / f'PartyMic-{version}-source.zip'
    with ZipFile(source_zip, 'w', ZIP_DEFLATED) as archive:
        for path in sorted(source_files):
            archive.write(path, 'PartyMic/' + path.relative_to(root).as_posix())
    for path in (binary_zip, source_zip):
        with ZipFile(path) as archive:
            if archive.testzip() is not None:
                raise RuntimeError(f'Corrupt archive: {path}')
    sums = ''.join(f'{sha256(path)}  {path.name}\n'
                   for path in (output / 'PartyMic.exe', binary_zip, source_zip))
    (output / 'SHA256SUMS.txt').write_text(sums, encoding='utf-8')
    print(f'Checked: {build_info["format"]}; only Windows system DLL imports; asInvoker manifest')
    print(f'EXE: {output / "PartyMic.exe"} ({len(data):,} bytes)')
    print(f'Package: {binary_zip}')
    print(f'SHA256: {build_info["sha256"]}')


if __name__ == '__main__':
    main()
