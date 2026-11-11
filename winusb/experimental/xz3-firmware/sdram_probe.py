# SPDX-License-Identifier: GPL-2.0-only
"""GV-MVP/XZ3のTSリング周辺のSDRAMを、boot0のUSBメモリ要求で調べる。

DriverHost_PX4がXZ3を開いていない状態で実行する。WinUSBは排他で開くため、
受信中のDriverHostがある場合は開けずに終了する。

scan   : 読み出しだけ。TSリングの内容と、各ブロックの読み出し可否を表示する。
alias  : 各ブロックの先頭ワードへ固有値を書き、全ブロックの読み戻しで実体と
         折り返しを判定する。終了時に元の値を書き戻して照合する。
mark   : 一定間隔で固有値を書き、そのまま残す。受信後にcheckで上書きの有無を見る。
check  : markで書いた値が残っているかを照合する。
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from contextlib import contextmanager
import struct
import sys

# ts_probe.cのRING (0x80a90000) に対応するDMAアドレスとサイズ。
RING_BASE = 0x00a90000
RING_SIZE = 0x3fe00
RING_END = RING_BASE + RING_SIZE
# 0x00d00000以降はTSエンジン、I²C、USBなどのMMIOで、書き込みに副作用がある。
WINDOW_LIMIT = 0x00d00000
BLOCK = 0x10000
MARK_STEP = 0x1000
RECORD = 192
RUN = 0x00d02010

XZ3_GUID = '{44e46df4-5fbd-40c8-84d6-e56ddceb12be}'
XZ3_DESCRIPTOR_ID = bytes.fromhex('bb043005')


def fingerprint(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


# DriverHost_PX4のRelay::Init()と同じboot0の照合。これ以外の状態では書き込まない。
BOOT_CHECKS = (
    (0x82008, 2, lambda d: d == b'\0\0', 'boot state'),
    (0xe9ce8, 8, lambda d: fingerprint(d) == 0xce1c939aaeb27bae, 'boot code 0xe9ce8'),
    (0xe91e4, 16, lambda d: fingerprint(d) == 0x7df1d1a09a7c2775, 'boot code 0xe91e4'),
    (0xe9a2c, 4, lambda d: d == bytes.fromhex('00f00000'), 'boot word 0xe9a2c'),
    (0xc4810, 4, lambda d: d == bytes.fromhex('82000000'), 'boot word 0xc4810'),
    (0xe9a10, 8, lambda d: d == bytes.fromhex('c44d00849b800000'), 'USB hook'),
    (0xea61c, 2236, lambda d: fingerprint(d) == 0x16073e2aa1fd67b1, 'boot body'),
)


def read(memory, address, size):
    return b''.join(memory.read(address + at, min(64, size - at)) for at in range(0, size, 64))


def check_boot(memory):
    for address, size, valid, name in BOOT_CHECKS:
        if not valid(read(memory, address, size)):
            raise ValueError(f'unsupported XZ3 state: {name} at {address:#x}')


def check_range(start, end, step):
    if start % step or end % step or start >= end:
        raise ValueError(f'range must be aligned to {step:#x} and non-empty')
    if start < RING_BASE or end > WINDOW_LIMIT:
        raise ValueError(f'writes are limited to {RING_BASE:#x}..{WINDOW_LIMIT:#x}')


def require_idle(memory):
    # DMAが動作中だと、書き込んだ値とリングの更新を区別できない。
    run = struct.unpack('>I', memory.read(RUN, 4))[0]
    if run:
        raise ValueError(f'TS DMA is not stopped: run={run:#010x}')


def tag(address, salt):
    # アドレスごとに異なり、0や全ビット1になりにくい値。saltで操作ごとに分ける。
    value = (address * 0x9e3779b1 ^ salt) & 0xffffffff
    return struct.pack('>I', value or 0x5a5a5a5a)


def ts_records(data):
    """192バイトのリングレコードのうち、TS同期バイトを持つ数を返す。"""
    return sum(1 for at in range(0, len(data) - RECORD + 1, RECORD) if data[at + 4] == 0x47)


def scan(memory, start, end, step=BLOCK):
    rows = []
    for address in range(start, end, step):
        try:
            data = memory.read(address, 64)
        except OSError as error:
            rows.append((address, 'error', str(error)))
            break
        kind = 'zero' if data == bytes(64) else 'ones' if data == b'\xff' * 64 else 'data'
        rows.append((address, kind, data[:16].hex()))
    return rows


def alias(memory, start, end, step=BLOCK):
    """各ブロックへ固有値を書き、全部書いた後に読み戻す。

    実体のないアドレスは書いた値を保持せず、折り返すアドレスは後の書き込みで
    先の値が変わる。どちらも判定後に元の値へ戻す。
    """
    check_range(start, end, step)
    # 折り返すアドレスでは先の書き込みが見えるため、元の値は書き込み前に全部読む。
    saved = [(address, memory.read(address, 4)) for address in range(start, end, step)]
    rows = []
    try:
        for address, _ in saved:
            memory.write(address, tag(address, 0xa11a5000))
        for address in range(start, end, step):
            actual = memory.read(address, 4)
            rows.append((address, actual == tag(address, 0xa11a5000), actual.hex()))
    finally:
        for address, data in reversed(saved):
            memory.write(address, data)
        for address, data in saved:
            if memory.read(address, 4) != data:
                raise OSError(f'restore failed at {address:#x}')
    return rows


def mark(memory, start, end, step=MARK_STEP):
    check_range(start, end, step)
    for address in range(start, end, step):
        memory.write(address, tag(address, 0x3a7c0000))
    return check(memory, start, end, step)


def check(memory, start, end, step=MARK_STEP):
    check_range(start, end, step)
    changed = []
    for address in range(start, end, step):
        actual = memory.read(address, 4)
        if actual != tag(address, 0x3a7c0000):
            changed.append((address, actual.hex()))
    return (end - start) // step, changed


def contiguous(rows):
    """aliasの結果から、先頭から連続して値を保持した範囲の終端を返す。"""
    end = None
    for address, ok, _ in rows:
        if not ok:
            break
        end = address
    return end


class Setup(c.Structure):
    _fields_ = [('type', c.c_ubyte), ('request', c.c_ubyte),
                ('value', w.USHORT), ('index', w.USHORT), ('length', w.USHORT)]


def bind(dll, name, result, *args):
    function = getattr(dll, name)
    function.restype = result
    function.argtypes = args
    return function


def interface_paths():
    cfg = c.WinDLL('cfgmgr32')
    ole = c.WinDLL('ole32')
    guid = (c.c_ubyte * 16)()
    if bind(ole, 'CLSIDFromString', c.c_long, w.LPCWSTR, c.c_void_p)(XZ3_GUID, guid):
        raise OSError('invalid interface GUID')
    size = w.ULONG()
    get_size = bind(cfg, 'CM_Get_Device_Interface_List_SizeW', w.DWORD,
                    c.POINTER(w.ULONG), c.c_void_p, w.LPCWSTR, w.ULONG)
    get_list = bind(cfg, 'CM_Get_Device_Interface_ListW', w.DWORD,
                    c.c_void_p, w.LPCWSTR, c.c_wchar_p, w.ULONG, w.ULONG)
    if get_size(c.byref(size), guid, None, 0):
        raise OSError('CM_Get_Device_Interface_List_SizeW failed')
    buffer = c.create_unicode_buffer(size.value)
    if get_list(guid, None, buffer, size.value, 0):
        raise OSError('CM_Get_Device_Interface_ListW failed')
    return [path for path in buffer[:size.value].split('\0') if path]


class WinUsbMemory:
    def __init__(self, handle, transfer):
        self.handle = handle
        self.transfer = transfer

    def _control(self, request_type, address, buffer, size):
        actual = w.ULONG()
        setup = Setup(request_type, 0xbc, (address >> 8) & 0xff00, address & 0xffff, size)
        if not self.transfer(self.handle, setup, buffer, size, c.byref(actual), None):
            raise OSError(f'memory request {request_type:#x} at {address:#x}: '
                          f'{c.WinError(c.get_last_error())}')
        if actual.value != size:
            raise OSError(f'short memory request at {address:#x}: {actual.value}/{size}')

    def read(self, address, size):
        buffer = c.create_string_buffer(size)
        self._control(0xc0, address, buffer, size)
        return buffer.raw

    def write(self, address, data):
        # boot0の書き込みはDriverHostと同じく4バイト単位で行う。
        if len(data) != 4 or address & 3:
            raise ValueError('writes must be one aligned word')
        self._control(0x40, address, c.create_string_buffer(bytes(data), 4), 4)


@contextmanager
def open_device(path):
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
        if actual.value != 18 or descriptor.raw[8:12] != XZ3_DESCRIPTOR_ID:
            raise ValueError('USB descriptor is not GV-MVP/XZ3 (04bb:0530)')
        yield WinUsbMemory(handle, transfer)
    finally:
        if handle:
            free(handle)
        close_file(file)


def number(text):
    return int(text, 0)


def run(memory, args):
    check_boot(memory)
    print('boot0=matched', flush=True)
    if args.command == 'scan':
        ring = read(memory, RING_BASE, RECORD * 64)
        print(f'ring_records_with_sync={ts_records(ring)}/64', flush=True)
        for address, kind, detail in scan(memory, args.start, args.end):
            print(f'{address:#08x} {kind} {detail}', flush=True)
        return 0
    require_idle(memory)
    if args.command == 'alias':
        rows = alias(memory, args.start, args.end)
        for address, ok, actual in rows:
            print(f'{address:#08x} {"kept" if ok else "lost"} {actual}', flush=True)
        end = contiguous(rows)
        print('contiguous_end=' + (f'{end + BLOCK:#08x}' if end is not None else 'none'))
        print('restore=verified')
        return 0 if end is not None else 1
    total, changed = (mark if args.command == 'mark' else check)(memory, args.start, args.end)
    for address, actual in changed:
        print(f'{address:#08x} changed {actual}', flush=True)
    print(f'{args.command} words={total} changed={len(changed)}')
    return 1 if changed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('scan', 'alias', 'mark', 'check'))
    parser.add_argument('--start', type=number, default=None,
                        help='開始アドレス (scanの既定はリング先頭、ほかはリング末尾の次のブロック)')
    parser.add_argument('--end', type=number, default=WINDOW_LIMIT, help='終了アドレス (含まない)')
    parser.add_argument('--interface', help='WinUSBのデバイスインターフェイスのパス')
    args = parser.parse_args()
    if args.start is None:
        args.start = RING_BASE if args.command == 'scan' else (RING_END + BLOCK - 1) // BLOCK * BLOCK
    paths = [args.interface] if args.interface else interface_paths()
    if len(paths) != 1:
        parser.error(f'expected one GV-MVP/XZ3 interface, found {len(paths)}')
    with open_device(paths[0]) as memory:
        return run(memory, args)


if __name__ == '__main__':
    sys.exit(main())
