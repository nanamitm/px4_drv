"""Stage experimental DT295 distributions without installing or opening devices."""
from pathlib import Path
import argparse
import shutil

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dist', type=Path, default=root / 'dist')
args = parser.parse_args()
dist = args.dist.resolve()
if not dist.is_relative_to(root.resolve()):
    raise SystemExit('Distribution must stay inside winusb workspace')
for bits, platform in ((32, 'x86'), (64, 'x64')):
    folder = dist / ('BonDriver_DT295_' + str(bits) + 'bit')
    binaries = root / 'build' / platform / 'Release-static'
    sources = [(binaries / 'BonDriver_PX4.dll', 'BonDriver_DT295.dll'),
               (binaries / 'DriverHost_PX4.exe', 'DriverHost_PX4.exe'),
               (binaries / 'WinSCard.dll', 'WinSCard.dll'),
               (binaries / 'channel_switch_stress.exe', 'channel_switch_stress.exe'),
               (root / 'pkg/BonDriver_PX4/BonDriver_DT295.ini', 'BonDriver_DT295.ini'),
               (root / 'pkg/DriverHost_PX4/DriverHost_PX4.ini', 'DriverHost_PX4.ini'),
               (root / 'docs/pix-dt295.md', 'PIX-DT295.md'),
               (root.parent / 'LICENSE', 'LICENSE')]
    sources += [(root / 'pkg/BonDriver_PX4' / name, name) for name in ('BonDriver_PX4-T.ChSet.txt', 'BonDriver_PX4-S.ChSet.txt')]
    for source, _ in sources:
        if not source.is_file():
            raise SystemExit('Missing build/input file: ' + str(source))
    folder.mkdir(parents=True, exist_ok=True)
    for source, name in sources:
        shutil.copy2(source, folder / name)
    # 共通ホストの全配置先で既存PX-S1UDファームウェアを保つ契約にも合わせる。
    (folder / 'firmware').mkdir(exist_ok=True)
    shutil.copy2(root / 'pkg/DriverHost_PX4/firmware/isdbt_rio.inp', folder / 'firmware/isdbt_rio.inp')
    shutil.copytree(root / 'pkg/licenses/siano', folder / 'licenses/siano', dirs_exist_ok=True)
# Shared hosts can expose DT295 from any configured BonDriver directory.
for folder in dist.glob('BonDriver_*'):
    if folder.is_dir() and (folder / 'DriverHost_PX4.exe').is_file():
        target = folder / 'firmware'
        target.mkdir(exist_ok=True)
        for name in ('dt295-source-fr.bin', 'dt295-source-fr.json', 'dt295-direct-fr.bin', 'dt295-direct-fr.json'):
            shutil.copy2(root / 'pkg/DriverHost_PX4/firmware' / name, target / name)
target = dist / 'licenses/magicvision'
target.mkdir(parents=True, exist_ok=True)
vendor = root / 'src/DriverHost_PX4/dt295/upstream'
for name in ('LICENSE-MIT', 'LICENSE', 'ORIGIN.json'):
    shutil.copy2(vendor / name, target / name)
shutil.copy2(root / 'docs/pix-dt295.md', target / 'PIX-DT295.md')
coax = dist / 'licenses/coax'
coax.mkdir(parents=True, exist_ok=True)
for name in ('COPYING', 'NOTICE', 'upstream.json', 'LOCAL-CHANGES.md'):
    shutil.copy2(root / 'tools/torne_frontend' / name, coax / name)
print('Prepared experimental DT295 x86/x64 folders; Experimental=false remains the default.')
