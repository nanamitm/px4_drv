"""Check experimental DT295 files, firmware integrity, architecture and identity."""
from pathlib import Path
import argparse
import struct

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dist', type=Path, default=root / 'dist')
args = parser.parse_args()
backends = ('direct-fr', 'source-fr')
firmware = root / 'pkg/DriverHost_PX4/firmware'
bundles = {backend: (firmware / f'dt295-{backend}.bin').read_bytes() for backend in backends}
inf = (args.dist / 'Driver/PIX-DT295.inf').read_text(encoding='utf-8')
if 'USB\\VID_06B8&PID_103B' not in inf or 'f5b71c91-0634-4a6b-902f-fd5756f591d3' not in inf:
    raise SystemExit('DT295 INF identity mismatch')
for legacy in ('DriverHost_TorneResident.ini', 'BonDriver_TorneResident.ini',
               'magicvision-dt295.bin', 'magicvision-torne-resident.bin',
               'magicvision-LICENSE-MIT.txt', 'magicvision-firmware-NOTICE.txt'):
    unexpected = next(args.dist.rglob(legacy), None)
    if unexpected is not None:
        raise SystemExit('Unexpected resident firmware asset: ' + str(unexpected))
for backend in backends:
    for suffix in ('bin', 'json'):
        name = f'dt295-{backend}.{suffix}'
        if next(args.dist.rglob(name), None) is not None:
            raise SystemExit('Unexpected external DT295 firmware asset: ' + name)
for bits, machine in ((32, 0x14c), (64, 0x8664)):
    folder = args.dist / ('BonDriver_DT295_' + str(bits) + 'bit')
    for name in ('BonDriver_DT295.dll', 'DriverHost_PX4.exe', 'WinSCard.dll', 'channel_switch_stress.exe'):
        data = (folder / name).read_bytes()
        offset = struct.unpack_from('<I', data, 0x3c)[0]
        if data[offset:offset + 4] != b'PE\0\0' or struct.unpack_from('<H', data, offset + 4)[0] != machine:
            raise SystemExit('Wrong architecture: ' + str(folder / name))
    for name in ('BonDriver_PX4-T.ChSet.txt', 'BonDriver_PX4-S.ChSet.txt', 'LICENSE', 'PIX-DT295.md'):
        if not (folder / name).is_file():
            raise SystemExit('Missing: ' + str(folder / name))
    ini = (folder / 'BonDriver_DT295.ini').read_text(encoding='ascii')
    if 'FramedStream=true' not in ini or 'System="ISDB-T,ISDB-S"' not in ini:
        raise SystemExit('DT295 stream/receiver configuration mismatch')
    host = (folder / 'DriverHost_PX4.ini').read_bytes()
    if b'Experimental=false' not in host or b'f5b71c91-0634-4a6b-902f-fd5756f591d3' not in host:
        raise SystemExit('DT295 device definition mismatch')
    print('PASS DT295 package ' + str(bits) + 'bit')
for folder in args.dist.glob('BonDriver_*'):
    if folder.is_dir() and (folder / 'DriverHost_PX4.exe').is_file():
        host = (folder / 'DriverHost_PX4.ini').read_bytes()
        exe = (folder / 'DriverHost_PX4.exe').read_bytes()
        for backend, bundle in bundles.items():
            if bundle not in exe:
                raise SystemExit(f'Embedded {backend} firmware mismatch: {folder}')
        if any(line.strip().startswith((b'SourceFirmwarePath=', b'SourceFirmwareSha256='))
               for line in host.splitlines()):
            raise SystemExit('DT295 default must use embedded direct-fr firmware: ' + str(folder))
        if any(line.strip().startswith((b'ResidentFirmware=', b'FirmwareBackend=magicvision'))
               for line in host.splitlines()):
            raise SystemExit('Unexpected resident firmware configuration: ' + str(folder))
        if b'FirmwareBackend=direct-fr' not in host:
            raise SystemExit('DT295 default backend is not direct-fr: ' + str(folder))
for name in ('LICENSE-MIT', 'LICENSE', 'ORIGIN.json', 'PIX-DT295.md'):
    if not (args.dist / 'licenses/magicvision' / name).is_file():
        raise SystemExit('Missing upstream attribution: ' + name)
print('PASS DT295 firmware and attribution. No DLL was loaded; no hardware was opened.')
