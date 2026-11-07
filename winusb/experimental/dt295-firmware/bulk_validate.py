# SPDX-License-Identifier: GPL-2.0-only
"""Verify PIX-DT295 terrestrial and satellite retuning and sustained reception over one WinUSB handle."""
import argparse
from bisect import bisect_right
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import math
import mmap
from pathlib import Path
import secrets
import struct
from threading import Event
import time

from aux_probe import image, run
from boot_contract import inspect_build
from boot_probe import open_device
from protocol import OK, request, response
from satellite_tune import frequency as satellite_frequency
from terrestrial_tune import transfers
from ts_ring import RING_SIZE, capture

USB_PACKET = 512
READ_SIZE = 65536
PUMP_BULK = 4
TS_STRIDE = 192
TS_PACKET = 188
TRANSITION_SECONDS = 1
MAX_GAP_SECONDS = .250


@dataclass(frozen=True)
class Channel:
    channel: int
    tsid: int
    system: str = 'gr'

    @property
    def label(self):
        return f'{self.system.upper()}{self.channel:02d}'


def channel_arg(value):
    try:
        physical, tsid = value.lower().split(':')
        system = physical[:2] if physical[:2] in ('gr', 'bs', 'cs') else 'gr'
        channel = int(physical[2:] if physical.startswith(system) else physical)
        result = Channel(channel, int(tsid, 0), system)
        if system != 'gr':
            satellite_frequency(system, channel)
        # 地デジは700 MHz未満向けのRF設定を使う。
        if (system != 'gr' or 13 <= channel <= 50) and 0 <= result.tsid <= 0xffff:
            return result
    except ValueError:
        pass
    raise argparse.ArgumentTypeError('expected 13:31761, BS01:16400 or CS02:24608')


def pump(control, dest, seconds, start_ring):
    finished = Event()
    cancelled = Event()
    ready = Event()
    committed = 0
    start = time.monotonic()

    def receive():
        total = 0
        samples = []
        with (dest / 'bulk-ring.raw').open('wb') as stream:
            ready.set()
            while not cancelled.is_set() and (not finished.is_set() or total < committed):
                try:
                    data = control.bulk(READ_SIZE)
                except OSError as error:
                    if (getattr(error, 'winerror', None) in (121, 1460)
                            and finished.is_set() and total == committed):
                        break
                    raise
                if not data:
                    raise OSError('empty USB Bulk transfer')
                stream.write(data)
                total += len(data)
                samples.append((total, time.monotonic() - start))
        return total, samples

    cursor = sent = commands = 0
    with ThreadPoolExecutor(max_workers=1) as executor:
        incoming = executor.submit(receive)
        try:
            if not ready.wait(1):
                if incoming.done():
                    incoming.result()
                raise TimeoutError('USB reader did not start')
            start = time.monotonic()
            start_ring()
            last_progress = start
            next_report = start + 60
            while True:
                if incoming.done():
                    incoming.result()
                    raise OSError('USB reader ended while the pump was active')
                now = time.monotonic()
                if now >= next_report:
                    print(f'bulk_progress seconds={now - start:.1f} committed={sent * USB_PACKET}',
                          flush=True)
                    next_report += 60
                if now - last_progress > 1:
                    raise TimeoutError(f'firmware pump stalled after {sent} USB packets')
                if now - start >= seconds:
                    remaining = -sent % (READ_SIZE // USB_PACKET)
                    if not remaining:
                        break
                    budget = remaining
                else:
                    budget = 256
                sequence = secrets.randbits(32)
                payload = struct.pack('>2I', cursor, budget)
                control(0xc4620, request(sequence, PUMP_BULK, payload), selector=0x7e)
                reply = response(control(0xc4620, 64), sequence, PUMP_BULK)
                if reply.status != OK or len(reply.payload) != 20:
                    raise OSError(f'firmware bulk operation rejected: {reply.status}')
                next_cursor, actual, producer, fifo, result = struct.unpack('>5I', reply.payload)
                commands += 1
                if result or actual > budget or next_cursor != (cursor + USB_PACKET * actual) % RING_SIZE:
                    raise OSError(f'bulk pump result={result} sent={actual} cursor={next_cursor:#x} '
                                  f'producer={producer:#x} fifo={fifo:#x}')
                if actual:
                    last_progress = time.monotonic()
                cursor = next_cursor
                sent += actual
            committed = sent * USB_PACKET
            finished.set()
            total, samples = incoming.result(timeout=2)
            if total != committed or not samples:
                raise OSError(f'USB byte mismatch: received={total}, committed={committed}')
        finally:
            cancelled.set()
    with (dest / 'bulk-timing.csv').open('w') as log:
        log.write('bytes,seconds\n')
        log.writelines(f'{offset},{elapsed:.6f}\n' for offset, elapsed in samples)
    print(f'bulk_verified bytes={total} commands={commands} elapsed={samples[-1][1]:.3f}', flush=True)
    return samples


def crc(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


def pat_tsid(packet, adaptation):
    index = 5 + packet[4] if adaptation & 2 else 4
    if index >= TS_PACKET:
        return None
    index += 1 + packet[index]
    if index + 12 <= TS_PACKET and packet[index] == 0:
        size = 3 + ((packet[index + 1] & 15) << 8 | packet[index + 2])
        if size >= 12 and index + size <= TS_PACKET:
            section = packet[index:index + size]
            if section[5] & 1 and crc(section) == 0:
                return int.from_bytes(section[3:5], 'big')
    return None


def metrics():
    return dict(packets=0, sync_errors=0, tei=0, malformed=0,
                cc_errors=Counter(), pat_tsid=Counter())


def analyze(dest, channel, samples, seconds, tune_seconds, tune_to_capture):
    offsets, elapsed = zip(*samples)
    boundary = next((offset for offset, timestamp in samples if timestamp >= TRANSITION_SECONDS),
                    offsets[-1])
    first_pat = None
    transition, steady = metrics(), metrics()
    counters = {}
    previous_packets = {}
    with (dest / 'bulk-ring.raw').open('rb') as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        sample = data[:READ_SIZE]
        offset = max(range(TS_STRIDE),
                     key=lambda i: sample[i::TS_STRIDE].count(0x47))
        interval = transition
        for pos in range(offset, len(data) - TS_PACKET + 1, TS_STRIDE):
            if interval is transition and pos >= boundary:
                interval = steady
            packet = data[pos:pos + TS_PACKET]
            interval['packets'] += 1
            if packet[0] != 0x47:
                interval['sync_errors'] += 1
                continue
            if packet[1] & 0x80:
                interval['tei'] += 1
            pid = (packet[1] & 31) << 8 | packet[2]
            adaptation = (packet[3] >> 4) & 3
            cc = packet[3] & 15
            if not adaptation or (adaptation == 2 and packet[4] != 183) or (
                    adaptation == 3 and packet[4] > 182):
                interval['malformed'] += 1
                continue
            if pid == 0x1fff:
                continue
            if adaptation & 2 and packet[4] and packet[5] & 0x80:
                counters.pop(pid, None)
                previous_packets.pop(pid, None)
            previous = counters.get(pid)
            if previous is not None:
                expected = (previous + bool(adaptation & 1)) & 15
                if cc != expected and (cc != previous or previous_packets.get(pid) != packet):
                    interval['cc_errors'][pid] += 1
            if adaptation & 1:
                counters[pid] = cc
                previous_packets[pid] = packet
                if pid == 0 and packet[1] & 0x40:
                    tsid = pat_tsid(packet, adaptation)
                    if tsid is not None:
                        interval['pat_tsid'][tsid] += 1
                        if first_pat is None and tsid == channel.tsid:
                            sample_index = bisect_right(offsets, pos + TS_PACKET - 1)
                            first_pat = elapsed[sample_index]
    gaps = [after - before for before, after in zip(elapsed, elapsed[1:])]
    wrong_tsids = sorted((transition['pat_tsid'].keys() | steady['pat_tsid'].keys()) - {channel.tsid})
    errors = [name for name in ('sync_errors', 'tei', 'malformed', 'cc_errors') if steady[name]]
    # Last data can be in the pipe when the requested pump interval ends.
    duration_ok = elapsed[-1] + MAX_GAP_SECONDS >= seconds
    report = dict(channel=channel.channel, system=channel.system, expected_tsid=channel.tsid, bytes=offsets[-1],
                  sync_offset=offset, requested_seconds=seconds, bulk_elapsed=elapsed[-1],
                  tune_to_lock_seconds=tune_seconds, first_usb_seconds=elapsed[0],
                  first_target_pat_seconds=first_pat,
                  tune_to_first_usb_seconds=(None if tune_to_capture is None
                                             else tune_to_capture + elapsed[0]),
                  tune_to_first_target_pat_seconds=(None if tune_to_capture is None or first_pat is None
                                                    else tune_to_capture + first_pat),
                  max_gap_seconds=max(gaps, default=0),
                  gaps_over_250ms=sum(gap > MAX_GAP_SECONDS for gap in gaps),
                  transition=transition, steady=steady, wrong_tsids=wrong_tsids)
    report['passed'] = bool(steady['pat_tsid'][channel.tsid] and not wrong_tsids
                            and not errors and not report['gaps_over_250ms'] and duration_ok)
    (dest / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return report['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface')
    parser.add_argument('auxiliary_build', type=Path)
    parser.add_argument('bulk_build', type=Path)
    parser.add_argument('init_trace', type=Path)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--channels', nargs='+', type=channel_arg, required=True)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--long-seconds', type=float, default=0,
                        help='additional fixed-channel receive after the final switch (e.g. 1800)')
    args = parser.parse_args()
    if (not math.isfinite(args.seconds) or args.seconds <= TRANSITION_SECONDS
            or not math.isfinite(args.long_seconds)
            or (args.long_seconds != 0 and args.long_seconds <= TRANSITION_SECONDS)):
        parser.error('capture durations must exceed one second (zero disables the additional receive)')

    rows = [json.loads(line) for line in args.init_trace.read_text().splitlines()]
    auxiliary = image(args.auxiliary_build)
    inspect_build(args.bulk_build)
    manifest = json.loads((args.bulk_build / 'manifest.json').read_text(encoding='utf-8'))
    if 'bulk_pump.c' not in manifest.get('sources', {}):
        parser.error('bulk build must be generated with build.py --bulk')
    args.evidence.mkdir(parents=True)
    index = 0
    long_capture = None

    def receive_segment(channel, tune_seconds):
        nonlocal index, long_capture
        index += 1
        locked_at = time.monotonic()

        def segment(name, seconds, tuned):
            dest = args.evidence / name
            dest.mkdir()

            def receive(start_ring):
                nonlocal setup_seconds
                if tuned is not None:
                    setup_seconds = time.monotonic() - locked_at + tuned
                return pump(control, dest, seconds, start_ring)

            setup_seconds = None
            samples = capture(control, args.bulk_build, dest, receive)
            return dest, channel, samples, seconds, tuned, setup_seconds

        short_capture = segment(f'{index:02d}-{channel.label.lower()}', args.seconds, tune_seconds)
        if not analyze(*short_capture):
            raise OSError(f'{short_capture[0].name} failed TS validation')
        if index == len(args.channels) and args.long_seconds:
            long_capture = segment(f'long-{channel.label.lower()}', args.long_seconds, None)

    with open_device(args.interface, bulk_input=True) as control:
        run(control, auxiliary, args.evidence, 7,
            transfers(rows, args.evidence, args.channels, receive_segment))
    print('session_restore=verified', flush=True)
    if long_capture is not None and not analyze(*long_capture):
        raise OSError(f'{long_capture[0].name} failed TS validation')


if __name__ == '__main__':
    main()
