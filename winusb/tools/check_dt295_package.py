"""Check experimental DT295 files, firmware integrity, architecture and identity."""
from pathlib import Path
import argparse
import hashlib
import json
import struct

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dist', type=Path, default=root / 'dist')
args = parser.parse_args()
source_fr_record = (root / 'pkg/DriverHost_PX4/firmware/dt295-source-fr.json').read_bytes()
source_fr_sha256 = json.loads(source_fr_record)['bundle']['sha256']
direct_fr_record = (root / 'pkg/DriverHost_PX4/firmware/dt295-direct-fr.json').read_bytes()
direct_fr_sha256 = json.loads(direct_fr_record)['bundle']['sha256']
inf = (args.dist / 'Driver/PIX-DT295.inf').read_text(encoding='utf-8')
if 'USB\\VID_06B8&PID_103B' not in inf or 'f5b71c91-0634-4a6b-902f-fd5756f591d3' not in inf:
    raise SystemExit('DT295 INF identity mismatch')
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
        firmware = folder / 'firmware/magicvision-dt295.bin'
        if hashlib.sha256(firmware.read_bytes()).hexdigest() != 'd9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f':
            raise SystemExit('Firmware mismatch: ' + str(firmware))
        if not (folder / 'firmware/magicvision-LICENSE-MIT.txt').is_file() or not (folder / 'firmware/magicvision-firmware-NOTICE.txt').is_file():
            raise SystemExit('Missing firmware license')
        host = (folder / 'DriverHost_PX4.ini').read_bytes()
        for name, sha256, record in (('dt295-source-fr', source_fr_sha256, source_fr_record),
                                     ('dt295-direct-fr', direct_fr_sha256, direct_fr_record)):
            source = folder / 'firmware' / (name + '.bin')
            if hashlib.sha256(source.read_bytes()).hexdigest() != sha256 or \
                    (folder / 'firmware' / (name + '.json')).read_bytes() != record:
                raise SystemExit('Source-built firmware mismatch: ' + str(source))
            # 既定のdirect-frは設定値、切り替え先のsource-frはINIのコメント内の値が同梱の版と一致する必要がある。
            if sha256.encode() not in host:
                raise SystemExit('SourceFirmwareSha256 does not match the bundle: ' + str(source))
        if b'FirmwareBackend=direct-fr' not in host:
            raise SystemExit('DT295 default backend is not direct-fr: ' + str(folder))
for name in ('LICENSE-MIT', 'LICENSE', 'ORIGIN.json', 'PIX-DT295.md'):
    if not (args.dist / 'licenses/magicvision' / name).is_file():
        raise SystemExit('Missing upstream attribution: ' + name)
print('PASS DT295 firmware and attribution. No DLL was loaded; no hardware was opened.')
