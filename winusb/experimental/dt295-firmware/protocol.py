# SPDX-License-Identifier: GPL-2.0-only
"""Host codec for the experimental PIF1 protocol. Transport is intentionally separate."""
from dataclasses import dataclass
import struct

MAGIC = 0x50494631
ABI = 1
CAPS, PING, I2C = 1, 2, 3
OK, BAD_MAGIC, BAD_VERSION, UNSUPPORTED, NOT_READY = range(5)


def request(sequence, operation, payload=b'', timeout_us=0, selector=0):
    if not 0 <= sequence <= 0xffffffff or not 0 <= operation < 0x80000000:
        raise ValueError('sequence or operation out of range')
    if len(payload) > 40:
        raise ValueError('payload exceeds packet capacity')
    return struct.pack('>6I', MAGIC, sequence, operation, ABI, timeout_us, selector) + payload.ljust(40,b'\0')


def i2c_request(sequence, address, tx=b'', rx_size=0, timeout_us=0):
    # 期限はプロトコルの予約フィールド。対応バックエンドの実装時に範囲を定める。
    if not 0 <= address <= 0x7f or len(tx)>40 or not 0 <= rx_size <= 40 or not (tx or rx_size):
        raise ValueError('invalid combined I2C transfer')
    selector = (address << 16) | (len(tx) << 8) | rx_size
    return request(sequence,I2C,tx,timeout_us,selector)


@dataclass(frozen=True)
class Response:
    status: int
    payload: bytes


def response(packet, sequence, operation):
    if len(packet)!=64:
        raise ValueError('response must contain exactly 64 bytes')
    magic, seq, op, status, abi, length = struct.unpack('>6I',packet[:24])
    if magic!=MAGIC or seq!=sequence or op!=(operation|0x80000000) or abi!=ABI:
        raise ValueError('response identity mismatch')
    if status not in range(5) or length>40 or (status!=OK and length):
        raise ValueError('invalid response status or length')
    if any(packet[24+length:]):
        raise ValueError('nonzero response padding')
    if status==OK and ((operation==CAPS and length!=16) or (operation==PING and length!=0)):
        raise ValueError('unexpected successful response length')
    return Response(status,packet[24:24+length])
