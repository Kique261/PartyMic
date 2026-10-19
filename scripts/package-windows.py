#!/usr/bin/env python3
"""Validate the Windows application and create the single-file NSIS installer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', required=True, type=Path)
    parser.add_argument('--makensis', type=Path, help='NSIS compiler; defaults to PATH')
    parser.add_argument('--nsis-dir', type=Path, help='Optional NSIS data directory')
    args = parser.parse_args()
    toolchain = args.toolchain.resolve()
    root = Path(__file__).resolve().parents[1]
    version = re.search(r'project\(PartyMic VERSION ([0-9.]+)',
                        (root / 'CMakeLists.txt').read_text()).group(1)
    executable = root / 'build-windows/PartyMic.exe'
    data = executable.read_bytes()
    if data[:2] != b'MZ':
        raise RuntimeError('Missing DOS executable signature')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    optional = pe + 24
    if data[pe:pe + 4] != b'PE\0\0' or (
            struct.unpack_from('<H', data, pe + 4)[0],
            struct.unpack_from('<H', data, optional)[0],
            struct.unpack_from('<H', data, optional + 68)[0]) != (0x8664, 0x20b, 2):
        raise RuntimeError('Expected AMD64 PE32+ Windows GUI executable')
    if b'<requestedExecutionLevel level="asInvoker"' not in data:
        raise RuntimeError('Missing ordinary-user manifest')
    readobj = toolchain / 'bin/llvm-readobj'
    imports = subprocess.check_output([str(readobj), '--coff-imports', str(executable)], text=True)
    dlls = re.findall(r'^  Name: (.+)$', imports, re.MULTILINE)
    system_dlls = {'ole32.dll', 'mmdevapi.dll', 'comctl32.dll', 'user32.dll',
                   'gdi32.dll', 'kernel32.dll', 'avrt.dll', 'shell32.dll',
                   'gdiplus.dll', 'msimg32.dll'}
    if not dlls or any(name.lower() not in system_dlls and
                       not name.lower().startswith('api-ms-win-crt-') for name in dlls):
        raise RuntimeError('Unexpected application DLL dependencies')
    resources = subprocess.check_output([str(readobj), '--coff-resources', str(executable)], text=True)
    if 'Type: ICON (ID 3)' not in resources or 'Type: GROUP_ICON (ID 14)' not in resources:
        raise RuntimeError('Missing application icon resources')

    compiler = str(args.makensis.resolve()) if args.makensis else shutil.which('makensis')
    if not compiler:
        raise RuntimeError('Install NSIS or pass --makensis; a Setup EXE is required')
    env = os.environ.copy()
    if args.nsis_dir:
        env['NSISDIR'] = str(args.nsis_dir.resolve())
    nsis_root = Path(env['NSISDIR']) if env.get('NSISDIR') else Path('/usr/share/nsis')
    # Fedora stores the license in /usr/share/licenses/mingw-nsis-base.
    nsis_license_candidates = [nsis_root / 'COPYING', nsis_root / 'Docs/COPYING',
        nsis_root.parent / 'licenses/mingw-nsis-base/COPYING',
        nsis_root.parent / 'doc/nsis/copyright']
    nsis_license = next((p for p in nsis_license_candidates if p.is_file()), None)
    if nsis_license is None:
        raise RuntimeError('Cannot locate the NSIS license; distribution must include it')

    output = root / 'dist'
    payload = output / 'staging/PartyMic'
    if payload.exists():
        shutil.rmtree(payload)
    payload.mkdir(parents=True)
    # Explicit public payload: private docs/local and validation evidence never enter.
    public_files = ['README.md', 'THIRD_PARTY_NOTICES.txt', 'docs/install.md',
                    'docs/usage.md', 'docs/troubleshooting.md', 'docs/assets/partymic-mixer.png']
    shutil.copy2(executable, payload / 'PartyMic.exe')
    for name in public_files:
        destination = payload / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, destination)
    licenses = payload / 'licenses'
    licenses.mkdir()
    shutil.copy2(toolchain / 'LICENSE.TXT', licenses / 'LLVM-LICENSE.txt')
    notices = toolchain / 'x86_64-w64-mingw32/share/mingw32'
    for name in ('COPYING.MinGW-w64-runtime.txt', 'COPYING.winpthreads.txt'):
        shutil.copy2(notices / name, licenses / name)
    shutil.copy2(nsis_license, licenses / 'NSIS-LICENSE.txt')

    setup = output / f'PartyMic-{version}-Setup.exe'
    subprocess.run([compiler, '-V2', f'-DAPP_VERSION={version}',
                    f'-DPAYLOAD_DIR={payload}', f'-DSETUP_FILE={setup}',
                    str(root / 'installer/PartyMic.nsi')], check=True, env=env)
    setup_data = setup.read_bytes()
    if setup_data[:2] != b'MZ':
        raise RuntimeError('Missing installer executable')
    # Private provenance is deliberately outside the installed payload.
    (output / 'build-info.json').write_text(json.dumps({
        'version': version, 'application_sha256': sha256(executable),
        'installer_sha256': sha256(setup), 'import_dlls': dlls,
        'toolchain': toolchain.name, 'payload': sorted(
            p.relative_to(payload).as_posix() for p in payload.rglob('*') if p.is_file()),
        'driver_bundled': False, 'install_scope': 'current user',
    }, ensure_ascii=False, indent=2) + '\n')
    (output / 'SHA256SUMS.txt').write_text(f'{sha256(setup)}  {setup.name}\n')
    print(f'Installer: {setup} ({len(setup_data):,} bytes)')
    print(f'SHA256: {sha256(setup)}')


if __name__ == '__main__':
    main()
