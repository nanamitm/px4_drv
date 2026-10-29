"""Replay a known CU8 capture through the frontend IPC and validate generated PATs."""
import argparse
import json
import struct
import subprocess
import threading
from pathlib import Path


def crc(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


def read_exact(pipe, size):
    out = bytearray()
    while len(out) < size:
        part = pipe.read(size - len(out))
        if not part:
            if not out:
                return None
            raise AssertionError("Truncated helper frame")
        out.extend(part)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("frontend", type=Path)
    parser.add_argument("iq", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = dict(ready=False, frames=0, ts_packets=0, pat=0, pmt=0, rs_accepted=0, rs_rejected=0)
    with args.output.open("xb") as output, Path(str(args.output) + ".stderr.log").open("x") as error:
        process = subprocess.Popen([str(args.frontend.resolve()), "--replay", str(args.iq.resolve())],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error)
        timer = threading.Timer(120, process.kill)
        timer.start()
        try:
            while True:
                header = read_exact(process.stdout, 24)
                if header is None:
                    break
                magic, kind, size, quality, accepted, rejected = struct.unpack("<6I", header)
                assert magic == 0x31545352 and 1 <= kind <= 4 and quality <= 10000
                assert (kind == 2 and 0 < size <= 188 * 2048 and size % 188 == 0) or (kind in (1, 3) and size == 0) or (kind == 4 and 0 < size <= 1024)
                payload = read_exact(process.stdout, size) if size else b""
                assert payload is not None
                if kind == 4:
                    raise AssertionError(payload.decode("utf-8", errors="replace"))
                if kind == 1:
                    report["ready"] = True
                if kind == 2:
                    assert report["ready"]
                    report["frames"] += 1
                    report["rs_accepted"], report["rs_rejected"] = accepted, rejected
                    for pos in range(0, size, 188):
                        p = payload[pos:pos+188]
                        assert p[0] == 0x47 and not p[1] & 0x80
                        pid = ((p[1] & 31) << 8) | p[2]
                        report["ts_packets"] += 1
                        if pid == 0x1fc8:
                            report["pmt"] += 1
                        if pid == 0:
                            assert p[1] & 0x40 and ((p[3] >> 4) & 3) == 1
                            offset = 5 + p[4]
                            length = 3 + ((p[offset + 1] & 15) << 8) + p[offset + 2]
                            assert offset + length <= 188 and crc(p[offset:offset+length]) == 0
                            report["pat"] += 1
                    output.write(payload)
            assert process.wait(timeout=10) == 0
            assert report["ts_packets"] > 0 and report["pat"] > 0 and report["pmt"] > 0
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            process.stdin.close()
            process.stdout.close()
    Path(str(args.output) + ".json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
