"""Compare old/new frontend TS output at several start positions of a known CU8 capture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("iq", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.iq.read_bytes()
    rate = 1_015_873
    report = []
    env = dict(os.environ, RTLSDR_PROFILE="1")
    with tempfile.TemporaryDirectory(prefix="rtlsdr-tuning-") as scratch:
        for offset_ms in (0, 500, 2000, 6000):
            start = (rate * offset_ms // 1000) * 2
            clip = source[start:start + rate * 15 * 2]
            assert len(clip) == rate * 15 * 2, "Need at least 21 seconds of CU8"
            iq = Path(scratch) / "clip.cu8"
            iq.write_bytes(clip)
            row = dict(offset_ms=offset_ms)
            for name, frontend in (("baseline", args.baseline), ("candidate", args.candidate)):
                ts = args.output / f"{offset_ms}-{name}.ts"
                subprocess.run([sys.executable, str(Path(__file__).with_name("rtlsdr_frontend_replay_test.py")),
                                str(frontend.resolve()), str(iq), str(ts)], check=True, timeout=135, env=env)
                metrics = json.loads(Path(str(ts) + ".json").read_text())
                log = Path(str(ts) + ".stderr.log").read_text()
                match = re.search(r"First TS after ([\d.]+)s, input_samples=(\d+)", log)
                assert match, "Missing first TS timing"
                row[name] = dict(sha256=hashlib.sha256(ts.read_bytes()).hexdigest(),
                                 first_ts_compute_s=float(match[1]), input_samples=int(match[2]), metrics=metrics)
            # 同じI/Qの同じ開始点では、計算を高速化しても出力を一切変えない。
            assert row["baseline"]["sha256"] == row["candidate"]["sha256"], row
            report.append(row)
            (args.output / "comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            print(json.dumps(row), flush=True)


if __name__ == "__main__":
    main()
