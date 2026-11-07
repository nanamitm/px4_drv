# SPDX-License-Identifier: GPL-2.0-only
"""WinUSB RAM-only PIF1 bring-up for the known PIX-DT295 boot image."""
import argparse
from contextlib import contextmanager
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import secrets
import struct

from boot_contract import BOOT_BYTES, inspect_build, validate_boot
from protocol import CAPS, PING, OK, request, response


class Setup(c.Structure):
    _fields_ = [('type', c.c_ubyte), ('request', c.c_ubyte),
                ('value', w.USHORT), ('index', w.USHORT), ('length', w.USHORT)]


def bind(dll, name, result, *args):
    function = getattr(dll, name)
    function.restype = result
    function.argtypes = args
    return function


@contextmanager
def open_device(path, *, bulk_input=False):
    kernel = c.WinDLL('kernel32', use_last_error=True)
    usb = c.WinDLL('winusb', use_last_error=True)
    open_file = bind(kernel, 'CreateFileW', w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD,
                     c.c_void_p, w.DWORD, w.DWORD, w.HANDLE)
    close_file = bind(kernel, 'CloseHandle', w.BOOL, w.HANDLE)
    initialize = bind(usb, 'WinUsb_Initialize', w.BOOL, w.HANDLE, c.POINTER(w.HANDLE))
    free = bind(usb, 'WinUsb_Free', w.BOOL, w.HANDLE)
    policy = bind(usb, 'WinUsb_SetPipePolicy', w.BOOL, w.HANDLE, c.c_ubyte, w.ULONG,
                  w.ULONG, c.c_void_p)
    transfer = bind(usb, 'WinUsb_ControlTransfer', w.BOOL, w.HANDLE, Setup, c.c_void_p,
                    w.ULONG, c.POINTER(w.ULONG), c.c_void_p)
    get_descriptor = bind(usb, 'WinUsb_GetDescriptor', w.BOOL, w.HANDLE, c.c_ubyte,
                          c.c_ubyte, w.USHORT, c.c_void_p, w.ULONG, c.POINTER(w.ULONG))

    file = open_file(path, 0xc0000000, 0, None, 3, 0x40000000, None)
    if file == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    handle = w.HANDLE()
    try:
        if not initialize(file, c.byref(handle)):
            raise c.WinError(c.get_last_error())
        timeout_ms = w.ULONG(1000)
        if not policy(handle, 0, 3, c.sizeof(timeout_ms), c.byref(timeout_ms)):
            raise c.WinError(c.get_last_error())
        descriptor = c.create_string_buffer(18)
        actual = w.ULONG()
        if not get_descriptor(handle, 1, 0, 0, descriptor, 18, c.byref(actual)):
            raise c.WinError(c.get_last_error())
        if actual.value != 18 or descriptor.raw[8:12] != bytes.fromhex('b8063b10'):
            raise ValueError('USB descriptor is not PIX-DT295 (06b8:103b)')

        def control(address, data, selector=0):
            reading = isinstance(data, int)
            buffer = c.create_string_buffer(data) if reading else c.create_string_buffer(data, len(data))
            setup = Setup(0xc0 if reading else 0x40, 0xbc, (address >> 8 & 0xff00) | selector,
                          address & 0xffff, len(buffer))
            if not transfer(handle, setup, buffer, len(buffer), c.byref(actual), None):
                raise OSError(f'BC {setup.type:02x}/{setup.value:04x}/{setup.index:04x}: '
                              f'{c.WinError(c.get_last_error())}')
            if actual.value != len(buffer):
                raise OSError(f'short BC transfer at {address:#x}: {actual.value}/{len(buffer)}')
            return buffer.raw

        if bulk_input:
            read_pipe = bind(usb, 'WinUsb_ReadPipe', w.BOOL, w.HANDLE, c.c_ubyte,
                             c.c_void_p, w.ULONG, c.POINTER(w.ULONG), c.c_void_p)
            if not policy(handle, 0x81, 3, c.sizeof(timeout_ms), c.byref(timeout_ms)):
                raise c.WinError(c.get_last_error())

            def bulk(size):
                payload = c.create_string_buffer(size)
                count = w.ULONG()
                if not read_pipe(handle, 0x81, payload, size, c.byref(count), None):
                    raise c.WinError(c.get_last_error())
                return payload.raw[:count.value]

            control.bulk = bulk
        yield control
    finally:
        if handle:
            free(handle)
        close_file(file)


def read(control, address, size):
    return b''.join(control(address + offset, min(64, size - offset))
                    for offset in range(0, size, 64))


def replace(control, address, data):
    chunk = len(data) if address in (0xea6fc, 0xc4620) else 4
    for offset in range(0, len(data), chunk):
        control(address + offset, data[offset:offset + chunk])
    if read(control, address, len(data)) != data:
        raise OSError(f'RAM verification failed at {address:#x}')


def run(control, build, evidence, execute, *, during_probe=None):
    state = control(0x82008, 2)
    print(f'boot_state={state.hex()}', flush=True)
    if state not in (b'\0\0', b'\xff\xff'):
        raise ValueError('device is outside the idle boot states')
    snapshot = read(control, 0xe9000, BOOT_BYTES)
    (evidence / 'boot-before.bin').write_bytes(snapshot)
    validate_boot(snapshot)
    print('boot_snapshot=matched', flush=True)
    if not execute:
        return

    if state != b'\0\0':
        raise ValueError('execution requires the verified boot state 0000')
    segments = inspect_build(build)
    resident = (build / 'resident.bin').read_bytes()
    adapter = (build / 'bootstrap.bin').read_bytes()
    hook = (build / 'boot-hook.bin').read_bytes()
    resident = resident.ljust((segments['binary']['bytes'] + 3) & ~3, b'\0')
    adapter = adapter.ljust((segments['bootstrap']['bytes'] + 3) & ~3, b'\0')
    originals = [(0xe1100, read(control, 0xe1100, len(resident))),
                 (0xe1000, read(control, 0xe1000, len(adapter))),
                 (0xc4620, read(control, 0xc4620, 64)),
                 (0xea6fc, snapshot[0x16fc:0x1704])]
    for address, data in originals:
        (evidence / f'ram-{address:06x}-before.bin').write_bytes(data)
    # Stock BC block writes also use the staging buffer as scratch.
    changed = [originals[2]]
    try:
        changed.append(originals[0])
        replace(control, 0xe1100, resident)
        changed.append(originals[1])
        replace(control, 0xe1000, adapter)
        changed.append(originals[3])
        replace(control, 0xea6fc, hook)
        for operation in (CAPS, PING):
            sequence = secrets.randbits(32)
            control(0xc4620, request(sequence, operation), selector=0x7e)
            packet = control(0xc4620, 64)
            (evidence / f'response-{operation}.bin').write_bytes(packet)
            reply = response(packet, sequence, operation)
            expected = struct.pack('>4I', 0, 40, 40, 0x06b8103b) if operation == CAPS else b''
            if reply.status != OK or reply.payload != expected:
                raise ValueError(f'unexpected diagnostic result for operation {operation}')
            print(f'operation={operation} response=verified', flush=True)
        if during_probe is not None:
            return during_probe()
    finally:
        # Restore the hook before touching code reachable from it. A failed
        # hook restore leaves the dependent code in place for a USB power cycle.
        for address, data in reversed(changed):
            replace(control, address, data)
        for address, data in originals:
            if read(control, address, len(data)) != data:
                raise OSError(f'final RAM comparison failed at {address:#x}')
        after = read(control, 0xe9000, BOOT_BYTES)
        (evidence / 'boot-after.bin').write_bytes(after)
        if after != snapshot or control(0x82008, 2) != state:
            raise OSError('boot snapshot or state differs after restoration')
        print('restore=verified', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface', help='WinUSB device interface path')
    parser.add_argument('build', type=Path)
    parser.add_argument('evidence', type=Path, help='new directory for RAM evidence')
    parser.add_argument('--execute', action='store_true', help='enable diagnostic RAM writes')
    args = parser.parse_args()
    args.evidence.mkdir(parents=True)
    with open_device(args.interface) as transfer:
        run(transfer, args.build, args.evidence, args.execute)
