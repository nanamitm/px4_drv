# SPDX-License-Identifier: GPL-2.0-only
import re
from pathlib import Path


root = Path(__file__).resolve().parents[2]
source = (root / 'driver/px4_card.c').read_text(encoding='utf-8')
table = source.split('px4_card_guids[] = {', 1)[1].split('};', 1)[0]
entries = re.findall(r'\{\s*0x([0-9a-f]{4}),\s*0x([0-9a-f]{4}),\s*"(\{[0-9a-f-]+\})"\s*\}', table)
actual = {(int(vid, 16), int(pid, 16)): guid for vid, pid, guid in entries}
assert len(actual) == len(entries), 'Duplicate VID/PID in px4_card_guids'

# px4_card_guids は VID 0x0511 の機種と、Linux 版が扱う他社 VID の機種だけを持つため、それ以外の INF (SDR や PIX-DT295 など) は照合の対象外にする
other_vendor_ids = {(0x06b8, 0x106b)}

# INF は同じ PID をアーキテクチャごとに記載しているため、重複をまとめて照合する
expected = {}
for inf in sorted((root / 'winusb/pkg/inf').glob('*.inf')):
    text = inf.read_text(encoding='utf-8-sig')
    ids = {(int(vid, 16), int(pid, 16))
           for vid, pid in re.findall(r'USB\\VID_([0-9a-f]{4})&PID_([0-9a-f]{4})', text, re.IGNORECASE)}
    ids = {i for i in ids if i[0] == 0x0511 or i in other_vendor_ids}
    if not ids:
        continue
    guids = set(re.findall(r'DeviceInterfaceGUIDs,[^"\n]*"(\{[0-9a-f-]+\})"', text, re.IGNORECASE))
    assert len(guids) == 1, f'{inf.name}: expected one interface GUID'
    guid = guids.pop().lower()
    for i in ids:
        assert i not in expected or expected[i] == guid, f'{inf.name}: conflicting GUID for VID/PID {i[0]:04x}/{i[1]:04x}'
        expected[i] = guid

assert actual == expected, f'VID/PID/GUID mismatch: kernel-only {actual.items() - expected.items()}, INF-only {expected.items() - actual.items()}'
print(f'check_card_guids: {len(actual)} VID/PID/GUID entries match the Windows INFs')
