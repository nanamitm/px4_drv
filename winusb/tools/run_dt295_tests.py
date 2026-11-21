"""Build/run hardware-free DT295 tests. No DLL loading or USB enumeration."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform', choices=('x86', 'x64', 'both'), default='both')
args = parser.parse_args()
vendor = root / 'src/DriverHost_PX4/dt295/upstream'
manifest = json.loads((vendor / 'ORIGIN.json').read_text(encoding='utf-8'))
for name, expected in manifest['files'].items():
    if hashlib.sha256((vendor / name).read_bytes()).hexdigest() != expected:
        raise SystemExit('Imported source differs from pinned revision: ' + name)
firmware = root / 'pkg/DriverHost_PX4/firmware/magicvision-dt295.bin'
if hashlib.sha256(firmware.read_bytes()).hexdigest() != 'd9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f':
    raise SystemExit('Firmware differs from verified release')
# 収録済みのソース生成版バンドルと、初期化列の生成器を確認する。
for script, extra in ((root / 'tools/check_dt295_source_firmware.py', []),
                      (root / 'experimental/dt295-firmware/tests/test_init_trace.py', ['--firmware', str(firmware)])):
    result = subprocess.run([sys.executable, str(script), *extra], capture_output=True)
    if result.returncode:
        print(result.stdout.decode(errors='replace'))
        print(result.stderr.decode(errors='replace'))
        raise SystemExit('Check failed: ' + script.name)
    print('PASS ' + script.name, flush=True)
msbuild = shutil.which('msbuild')
if not msbuild:
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    # 試験プロジェクトは v143 を既定の toolset とするため、Visual Studio 2022 以降のうち
    # v143 を Win32 と x64 の双方へ導入済みの環境を選び、併存時は Visual Studio 2022 を優先する。
    instances = json.loads(subprocess.check_output([str(vswhere), '-all', '-version', '17.0', '-requires', 'Microsoft.Component.MSBuild', '-format', 'json', '-utf8'], encoding='utf-8') or '[]')
    instances.sort(key=lambda instance: tuple(int(part) for part in instance['installationVersion'].split('.')))
    for instance in instances:
        vs = Path(instance['installationPath'])
        candidate = vs / 'MSBuild/Current/Bin/MSBuild.exe'
        if candidate.is_file() and all((vs / 'MSBuild/Microsoft/VC/v170/Platforms' / native / 'PlatformToolsets/v143').is_dir() for native in ('Win32', 'x64')):
            msbuild = str(candidate)
            break
    if not msbuild:
        raise SystemExit('MSBuild with the v143 platform toolset (Win32 and x64) not found; install Visual Studio 2022 or later with the MSVC v143 build tools')
for platform in (('x86', 'x64') if args.platform == 'both' else (args.platform,)):
    native = 'Win32' if platform == 'x86' else 'x64'
    out = root / 'build' / platform / 'Release-static'
    out.mkdir(parents=True, exist_ok=True)
    for test in ('bootstrap', 'loader', 'session', 'usb_window', 'stream_input', 'transport', 'runtime', 'frontend_init', 'card', 'control_channel', 'stall', 'pump'):
        own_project = test in ('transport', 'runtime', 'frontend_init', 'card', 'control_channel', 'stall', 'pump')
        project = root / 'tests' / ('dt295_' + test + '_test.vcxproj' if own_project else 'dt295_upstream_test.vcxproj')
        command = [msbuild, str(project), '/nologo', '/v:minimal', '/p:Configuration=Release-static', '/p:Platform=' + native, '/p:TestName=' + test]
        result = subprocess.run(command, capture_output=True)
        (out / ('dt295_' + test + '_build.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            print(result.stdout.decode(errors='replace'))
            print(result.stderr.decode(errors='replace'))
            raise SystemExit('Build failed: ' + test + ' ' + platform)
        command = [str(out / ('dt295_' + test + '_test.exe'))]
        if test in ('transport', 'runtime'):
            command.append(str(firmware))
        result = subprocess.run(command, capture_output=True)
        (out / ('dt295_' + test + '_run.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            print(result.stdout.decode(errors='replace'))
            print(result.stderr.decode(errors='replace'))
            raise SystemExit('Test failed: ' + test + ' ' + platform)
        print('PASS DT295 ' + platform + ' ' + test, flush=True)
        if test == 'runtime':
            result = subprocess.run(command + ['torne'], capture_output=True)
            (out / 'torne_resident_runtime_run.log').write_bytes(result.stdout + result.stderr)
            if result.returncode:
                print(result.stdout.decode(errors='replace'))
                print(result.stderr.decode(errors='replace'))
                raise SystemExit('Torne resident runtime test failed: ' + platform)
            print('PASS torne resident runtime ' + platform, flush=True)
print('PASS pinned source and firmware SHA-256. No hardware was opened.')
