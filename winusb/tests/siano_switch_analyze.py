# SPDX-License-Identifier: GPL-2.0-only
"""Check every epoch's PAT (including CRC) and independently inspect TS with TSDuck."""
from pathlib import Path
import argparse
import json
import re
import subprocess

def crc32_mpeg(data):
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04C11DB7 if value & 0x80000000 else 0)) & 0xFFFFFFFF
    return value

def pat_ids(data):
    if len(data) % 188:
        raise ValueError("TS has an incomplete packet")
    pending = bytearray()
    ids = []

    def consume():
        while pending:
            if pending[0] == 0xFF:
                pending.clear()
                return
            if len(pending) < 3:
                return
            length = 3 + ((pending[1] & 15) << 8) + pending[2]
            if length < 12 or length > 1024:
                raise ValueError("Invalid PAT section length")
            if len(pending) < length:
                return
            section = pending[:length]
            del pending[:length]
            if section[0] != 0 or not section[1] & 0x80 or crc32_mpeg(section) != 0:
                raise ValueError("Invalid PAT table/header/CRC")
            ids.append((section[3] << 8) | section[4])

    for offset in range(0, len(data), 188):
        p = data[offset:offset + 188]
        if p[0] != 0x47 or p[1] & 0x80:
            raise ValueError("TS sync or TEI error")
        if ((p[1] & 31) << 8) | p[2]:
            continue
        afc = (p[3] >> 4) & 3
        if not afc & 1:
            continue
        pos = 5 + p[4] if afc & 2 else 4
        if pos >= 188:
            raise ValueError("Invalid PAT adaptation length")
        payload = p[pos:]
        if p[1] & 0x40:
            pointer = payload[0]
            if pointer + 1 > len(payload):
                raise ValueError("Invalid PAT pointer")
            if pending:
                pending.extend(payload[1:1 + pointer])
                consume()
                if pending:
                    raise ValueError("PAT section interrupted by a new start")
            pending = bytearray(payload[1 + pointer:])
        elif pending:
            pending.extend(payload)
        consume()
    if not ids:
        raise ValueError("No complete PAT section")
    return ids

def analyze(directory, cycles, expected):
    paths = sorted(directory.glob("cycle*_ch*_epoch*.ts"))
    if len(paths) != cycles:
        raise ValueError(f"Expected {cycles} epochs, got {len(paths)}")
    reports = []
    previous_epoch = None
    for index, path in enumerate(paths):
        match = re.fullmatch(r"cycle(\d+)_ch(\d+)_epoch(\d+)\.ts", path.name)
        cycle, channel, epoch = map(int, match.groups())
        if cycle != index or channel != list(expected)[index % 2]:
            raise ValueError(f"Unexpected cycle/channel: {path.name}")
        if previous_epoch is not None and epoch != previous_epoch + 1:
            raise ValueError(f"Non-contiguous epoch: {path.name}")
        previous_epoch = epoch
        data = path.read_bytes()
        ids = pat_ids(data)
        if set(ids) != {expected[channel]}:
            raise ValueError(f"Wrong/old/mixed TSID in {path.name}: {set(ids)} != {expected[channel]}")
        result = subprocess.run(["tsanalyze", "--isdb", "--japan", "--normalized", str(path)],
                                capture_output=True, encoding="utf-8", errors="replace", check=True)
        if result.stderr.strip():
            raise ValueError(f"TSDuck reported an error: {result.stderr}")
        summary = next(line for line in result.stdout.splitlines() if line.startswith("ts:"))
        fields = dict(re.findall(r"([a-z0-9]+)=(\d+)", summary))
        for key in ("invalidsyncs", "transporterrors", "suspectignored"):
            if int(fields[key]):
                raise ValueError(f"{path.name}: {key}={fields[key]}")
        pid_errors = {k: 0 for k in ("discontinuities", "duplicated", "invalidscrambling")}
        for line in result.stdout.splitlines():
            if line.startswith("pid:"):
                for key, value in re.findall(r"(discontinuities|duplicated|invalidscrambling)=(\d+)", line):
                    pid_errors[key] += int(value)
        if any(pid_errors.values()):
            raise ValueError(f"{path.name}: PID errors: {pid_errors}")
        reports.append(dict(cycle=cycle, channel=channel, epoch=epoch, tsid=expected[channel],
                            pat_sections=len(ids), packets=len(data) // 188, bytes=len(data), **pid_errors))
        print(f"PASS cycle={cycle} channel={channel} epoch={epoch} tsid={expected[channel]} PATs={len(ids)} packets={len(data)//188}", flush=True)
    return reports

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("cycles", type=int)
    parser.add_argument("--channel-a", type=int, default=13)
    parser.add_argument("--tsid-a", type=int, required=True)
    parser.add_argument("--channel-b", type=int, default=14)
    parser.add_argument("--tsid-b", type=int, required=True)
    args = parser.parse_args()
    reports = analyze(args.directory, args.cycles, {args.channel_a: args.tsid_a, args.channel_b: args.tsid_b})
    (args.directory / "analysis.json").write_text(json.dumps(reports, indent=2), encoding="utf-8")
    print(f"PASS: {len(reports)} epochs; all PAT CRC/TSID and TSDuck checks")
