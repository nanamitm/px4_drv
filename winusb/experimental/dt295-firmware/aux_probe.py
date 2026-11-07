# SPDX-License-Identifier: GPL-2.0-only
"""Run the source-built PAF1 auxiliary CPU diagnostic on a cold PIX-DT295."""
import argparse
from contextlib import contextmanager, nullcontext
from hashlib import sha256
from itertools import chain
import json
from pathlib import Path
import struct
import time

from boot_contract import BOOT_BYTES, validate_boot
from boot_probe import open_device, read, replace

RAM = 0x017a0000
RAM_SIZE = 0x4000
MAILBOX = RAM + 0x3900
CLUSTER = 0x01700000
VECTOR = 0x017ffffc
STAGING = 0x000c4620
PAF1 = 0x50414631


@contextmanager
def bank(control, address, writing=False):
    register = 0xea3d8 if writing else 0xe9ebc
    stock = bytes.fromhex('00f00000')
    if control(register, 4) != stock:
        raise ValueError(f'unexpected memory window at {register:#x}')
    try:
        replace(control, register, (address & ~0x1ffff).to_bytes(4, 'big'))
        yield 0x80000 + (address & 0x1ffff)
    finally:
        replace(control, register, stock)


def mapped_read(control, address, size):
    with bank(control, address) as alias:
        return read(control, alias, size)


def mapped_write(control, address, data):
    with bank(control, address, writing=True) as alias:
        for offset in range(0, len(data), 64):
            control(alias + offset, data[offset:offset + 64])


def word(control, address, value):
    mapped_write(control, address, struct.pack('>I', value))


def image(build):
    manifest = json.loads((build / 'manifest.json').read_text(encoding='utf-8'))
    data = (build / 'auxiliary.bin').read_bytes()
    if (manifest.get('target') != 'PIX-DT295 auxiliary FR' or manifest.get('diagnostic') != 'paf1'
            or manifest.get('deployable') is not False or manifest.get('features') != []
            or manifest.get('entry') != '0x000a0000' or manifest.get('shared_ram') != '0x017a0000'
            or not 0 < len(data) <= 0x2000
            or manifest.get('binary') != dict(file='auxiliary.bin', bytes=len(data),
                                              sha256=sha256(data).hexdigest())):
        raise ValueError('expected a source-built PIX-DT295 PAF1 image')
    return data.ljust((len(data) + 3) & ~3, b'\0')


def poll_mailbox(control, condition):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        packet = struct.unpack('>16I', mapped_read(control, MAILBOX, 64))
        if packet[:2] == (PAF1, 1) and condition(packet):
            return packet
        time.sleep(0.01)
    raise TimeoutError(f'PAF1 mailbox timeout: heartbeat={packet[2]} '
                       f'sequence={packet[4]} checkpoint={packet[15]:#x}')


@contextmanager
def demod_reset(control):
    direction = control(0xf10014, 2)
    output = control(0xf10018, 2)
    if direction != bytes(2) or output != bytes(2):
        raise ValueError(f'unrecognized cold GPIO state: {direction.hex()}/{output.hex()}')
    try:
        replace(control, 0xf10014, b'\x00\x02')
        replace(control, 0xf10018, b'\x00\x00')
        time.sleep(0.020)
        replace(control, 0xf10018, b'\x00\x02')
        time.sleep(0.005)
        print('demod_gpio=enabled', flush=True)
        yield
    finally:
        try:
            replace(control, 0xf10018, output)
        finally:
            replace(control, 0xf10014, direction)
        print('demod_gpio=restored', flush=True)


def run(control, data, evidence, operation=4, transfers=()):
    if control(0x82008, 2) != bytes(2):
        raise ValueError('execution requires the verified boot state 0000')
    boot = read(control, 0xe9000, BOOT_BYTES)
    validate_boot(boot)
    cluster = mapped_read(control, CLUSTER, 8)
    vector = mapped_read(control, VECTOR, 4)
    if cluster != bytes.fromhex('000200ff0d8f7ff3') or vector != bytes(4):
        raise ValueError(f'unrecognized cold auxiliary state: {cluster.hex()}/{vector.hex()}')
    ram = mapped_read(control, RAM, RAM_SIZE)
    staging = control(STAGING, 64)
    for name, value in [('boot', boot), ('aux-ram', ram), ('cluster', cluster),
                        ('vector', vector), ('staging', staging)]:
        (evidence / f'{name}-before.bin').write_bytes(value)
    gpio = demod_reset(control) if operation in (6, 7) else nullcontext()
    with gpio:
        run_auxiliary(control, data, evidence, operation, boot, cluster, vector, ram, staging, transfers)


def run_auxiliary(control, data, evidence, operation, boot, cluster, vector, ram, staging, transfers):
    start_attempted = False
    try:
        mapped_write(control, RAM, data)
        if mapped_read(control, RAM, len(data)) != data:
            raise OSError('auxiliary code readback mismatch')
        mapped_write(control, MAILBOX, bytes(64))
        word(control, VECTOR, 0xa0000)
        vector_read = mapped_read(control, VECTOR, 4)
        if vector_read not in (vector, bytes.fromhex('000a0000')):
            raise OSError(f'unexpected auxiliary vector readback: {vector_read.hex()}')
        # The vector command may not echo. Only a fresh PAF1 heartbeat proves execution.
        print(f'auxiliary_vector_command=000a0000 readback={vector_read.hex()}', flush=True)
        start_attempted = True
        word(control, CLUSTER + 4, 0xffffffff)
        word(control, CLUSTER, 0xffffffff)
        word(control, CLUSTER + 4, 0x00800000)
        word(control, CLUSTER + 4, 0)
        word(control, CLUSTER, 0xfffcffff)
        first = poll_mailbox(control, lambda packet: True)
        second = poll_mailbox(control, lambda packet: packet[2] != first[2])
        print(f'PAF1 heartbeat={first[2]}->{second[2]}', flush=True)
        diagnostics = ((7, transfer) for transfer in transfers) if operation == 7 else ((operation, None),)
        requests = chain(((1, None), (2, None)), diagnostics)
        for sequence, (command, transfer) in enumerate(requests, 1):
            if transfer is not None:
                address, tx, rx_size = transfer
                payload = bytes((address, len(tx), rx_size, 0)) + tx.ljust(8, b'\0') + bytes(8)
                mapped_write(control, MAILBOX + 64, payload)
            word(control, MAILBOX + 20, command)
            word(control, MAILBOX + 12, sequence)
            reply = poll_mailbox(control, lambda packet: packet[4] == sequence)
            (evidence / f'mailbox-{sequence}-{command}.bin').write_bytes(struct.pack('>16I', *reply))
            if reply[5:8] != (command, 0, 0):
                raise ValueError(f'unexpected PAF1 result for command {command}')
            print(f'PAF1 command={command} response=verified', flush=True)
            if command in (4, 5):
                print(f'I2C state={reply[8]:08x} control={reply[9]:08x} '
                      f'irq_pending={reply[10]:08x} irq_mask={reply[11]:08x} '
                      f'system_gate={reply[12]:04x} system_config={reply[13]:04x} '
                      f'gate_reacquired={reply[14]} config_check={reply[15]}', flush=True)
            if command == 6:
                print(f'demod stage={reply[15]:#x} state={reply[8]:#x} control={reply[9]:#x} '
                      f'irq_pending={reply[10]:#x} '
                      f'irq_mask_or_rx={reply[11]:#x} '
                      f'gate={reply[12]:#x}', flush=True)
                if reply[15] == 2 and (reply[8] & 0x10) and (reply[10] & 1):
                    raise OSError('demodulator write address NACK (7-bit 0x10)')
                if (reply[14], reply[15]) != (1, 6):
                    raise OSError('fixed demod read or restoration unverified')
            if command == 5 and (reply[14], reply[15]) != (1, 1):
                raise OSError('system configuration change or restoration unverified')
            if command == 7:
                if (reply[14], reply[15]) != (1, 6):
                    raise OSError(f'I2C address {address:#04x} failed: stage={reply[15]:#x} '
                                  f'state={reply[8]:#x} control={reply[9]:#x} irq={reply[10]:#x}')
                rx = mapped_read(control, MAILBOX + 76, 8)[:rx_size]
                (evidence / f'i2c-{sequence}.bin').write_bytes(rx)
                print(f'I2C address={address:#04x} tx={tx.hex()} rx={rx.hex()}', flush=True)
    finally:
        if start_attempted:
            word(control, CLUSTER + 4, 0xffffffff)
            word(control, CLUSTER, 0xffffffff)
            halted = mapped_read(control, CLUSTER, 8)
            first = mapped_read(control, MAILBOX + 8, 4)
            time.sleep(0.01)
            second = mapped_read(control, MAILBOX + 8, 4)
            if not (halted[1] & 2 and halted[7] & 0x80) or first != second:
                raise OSError(f'auxiliary stop unverified ({halted.hex()}); USB replug required')
            print(f'auxiliary_stop=verified ({halted.hex()})', flush=True)
        mapped_write(control, RAM, ram)
        mapped_write(control, VECTOR, vector)
        restored = mapped_read(control, RAM, RAM_SIZE)
        (evidence / 'aux-ram-after.bin').write_bytes(restored)
        if restored != ram or mapped_read(control, VECTOR, 4) != vector:
            raise OSError('auxiliary RAM or reset vector restoration mismatch')
        replace(control, STAGING, staging)
        if read(control, 0xe9000, BOOT_BYTES) != boot or control(0x82008, 2) != bytes(2):
            raise OSError('boot state differs after restoration')
        if operation in (6, 7) and (control(0xf10114, 2) != bytes.fromhex('8000')
                               or control(0xf1010c, 2) != bytes.fromhex('0001')):
            raise OSError('system configuration still changed; USB replug required')
        after = mapped_read(control, CLUSTER, 8)
        (evidence / 'cluster-after.bin').write_bytes(after)
        print(f'auxiliary_ram_vector_boot=verified cluster_before={cluster.hex()} '
              f'cluster_after={after.hex()}', flush=True)
        if after != cluster:
            raise OSError('auxiliary control state differs; USB replug required')


def parse_transfer(value):
    try:
        address, tx, rx_size = value.split(':')
        address, tx, rx_size = int(address, 0), bytes.fromhex(tx), int(rx_size, 0)
        if address not in (0x10, 0x11) or len(tx) > 8 or not 0 <= rx_size <= 8 or not (tx or rx_size):
            raise ValueError()
    except ValueError as error:
        raise argparse.ArgumentTypeError('expected 0x10 or 0x11:TX hex:RX length (up to 8 bytes each)') from error
    return address, tx, rx_size


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface', help='WinUSB device interface path')
    parser.add_argument('build', type=Path)
    parser.add_argument('evidence', type=Path, help='new directory for RAM evidence')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check-clock', dest='operation', action='store_const', const=5, default=4)
    mode.add_argument('--read-demod', dest='operation', action='store_const', const=6)
    mode.add_argument('--transfer', action='append', type=parse_transfer, metavar='ADDR:TX:RX')
    args = parser.parse_args()
    data = image(args.build)
    args.evidence.mkdir(parents=True)
    with open_device(args.interface) as control:
        run(control, data, args.evidence, 7 if args.transfer else args.operation, args.transfer or ())
