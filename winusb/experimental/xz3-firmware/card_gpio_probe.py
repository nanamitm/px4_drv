# SPDX-License-Identifier: GPL-2.0-only
"""GV-MVP/XZ3の0x00f10018の端子を1ビットずつ駆動し、カードのATRを待つ試験。

TDA8034ATはCMDVCCN（Lowで有効）がLowになるまでカードへ給電せず、CLKも出さない。
カードのRSTはRSTIN（Highで有効）に従い、カードはRSTのLowからHighでATRを1回送る。

--level low : 端子を1ビットずつLowにしてから、PIX-DT295と同じ19項目の電源投入で
              ATRを待つ。
--level high: 先に19項目の電源投入で受信を準備し、端子を1ビットずつHighにして
              ATRを待つ。--wait-msを延ばすと、その間に端子の電圧を測れる。

XZ3のCMDVCCNは0x00fea004のbit 3が駆動し、19項目の電源投入は最後にbit 3を立てる
（CMDVCCNをHighにする）ため、この試験だけではATRを受信しない。経緯はREADMEの
「内蔵カードリーダーの調査」を参照。

DriverHost_PX4がXZ3を開いていない状態で、DriverHostで一度受信した後
（0x00f10014=0xd0f5、0x00f10018=0x00e5）にカードを挿して実行する。
各ビットの試験後は、例外の有無にかかわらずカードUARTを停止手順へ戻し、
端子の出力値と出力許可を元の値へ戻して読み戻しで照合する。
"""
import argparse
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sdram_probe  # noqa: E402

PIN_DIRECTION = 0x00f10014
PIN_OUTPUT = 0x00f10018
# DriverHostのブートストラップ（操作6、操作5の手順0〜5、操作3）を適用した後の値。
EXPECTED_DIRECTION = 0xd0f5
EXPECTED_OUTPUT = 0x00e5
# bit 0と6は状態機械がチューナーの電源とリセットに使う。--bitsで明示したときだけ試す。
TUNER_BITS = frozenset((0, 6))

CONTROL0 = 0xfea000
CONTROL1 = 0xfea004
DIVIDER = 0xfea008
START = 0xfea00c
MODE = 0xfea010
FIFO = 0xfea014
ENABLE = 0xfea018
STATUS = 0xfea01c
BAUD = 0xfea020
FRACTION = 0xfea024
RX_DATA = 0xfea02c
POWER = 0xfea034
TX_CONFIG = 0xfea038
GUARD = 0xfea03c
PRESENT = 0x400

# dt295/card_uart.cppのReset()とPowerDown()と同じ手順。（番地, 値, 待機ms）
UART_RESET = (
    (ENABLE, 0, 0), (POWER, 0, 0), (STATUS, 0x3ff, 0), (CONTROL1, 0, 0), (FIFO, 0, 0),
    (CONTROL0, 0, 20), (CONTROL0, 7, 20), (CONTROL1, 8, 0), (DIVIDER, 0xba7f, 20),
    (CONTROL1, 0xa, 0), (CONTROL0, 0, 20), (CONTROL0, 7, 0), (GUARD, 0x3b, 0),
    (BAUD, 9600, 0), (FRACTION, 0, 0), (ENABLE, 0, 0), (FIFO, 0x20, 50), (POWER, 1, 0),
    (CONTROL1, 0xe, 0),
)
UART_POWER_DOWN = (
    (FIFO, 0, 0), (CONTROL0, 3, 0), (CONTROL1, 0, 0), (ENABLE, 0, 0), (POWER, 0, 1),
    (ENABLE, 0, 0), (POWER, 0, 0), (CONTROL1, 4, 0), (FIFO, 0x20, 0), (CONTROL0, 7, 0),
    (DIVIDER, 0xba7f, 1), (GUARD, 0, 0), (BAUD, 0, 0), (FRACTION, 0, 0), (START, 0, 0),
    (MODE, 0x200, 0), (TX_CONFIG, 0, 0),
)

# TDA8034ATは、CMDVCCNが30 msを超えてHighだった後のLowで5 Vを選ぶ（B-CASは5 V）。
HIGH_HOLD_MS = 50
# CMDVCCNのLowからRST有効までは数ms（内部発振の384周期と23T/2）。余裕を取って待つ。
ACTIVATE_MS = 20
# 以前のXZ3の調査と同じく、電源投入後1.8秒の間に受信FIFOを確認する。
ATR_WAIT_MS = 1800
POLL_MS = 20
ATR_LIMIT = 33


class Pins:
    """0x00f10014と0x00f10018の上位16ビット（16ビットのレジスター）を扱う。

    boot0のメモリ要求は4バイト単位のため、下位16ビット（0x00f10016、0x00f1001a）は
    読んだ値をそのまま書き戻し、書き込み後に変わっていないことを照合する。
    """

    def __init__(self, memory):
        self.memory = memory

    def read(self, address):
        return struct.unpack('>I', self.memory.read(address, 4))[0]

    def high(self, address):
        return self.read(address) >> 16

    def set_high(self, address, value):
        word = self.read(address)
        expected = (value & 0xffff) << 16 | word & 0xffff
        self.memory.write(address, struct.pack('>I', expected))
        actual = self.read(address)
        if actual != expected:
            raise OSError(f'pin write mismatch at {address:#x}: '
                          f'wrote {expected:#010x}, read {actual:#010x}')


class Uart:
    def __init__(self, memory, sleep):
        self.memory = memory
        self.sleep = sleep

    def read(self, address):
        return struct.unpack('>I', self.memory.read(address, 4))[0]

    def run(self, steps):
        for address, value, pause in steps:
            self.memory.write(address, struct.pack('>I', value))
            if pause:
                self.sleep(pause)

    def present(self):
        return bool(self.read(STATUS) & PRESENT)

    def wait_atr(self, wait_ms, poll_ms):
        """受信FIFOを読み、受け取ったバイト列と最後の状態を返す。"""
        received = bytearray()
        waited = 0
        idle = 0
        while True:
            count = self.read(FIFO) >> 16
            for _ in range(min(count, ATR_LIMIT - len(received))):
                received.append(self.read(RX_DATA) & 0xff)
            status = self.read(STATUS)
            idle = 0 if count else idle + poll_ms
            # ATRの途中で受信が100 ms止まったら、残りの待ちを打ち切る。
            if len(received) >= ATR_LIMIT or waited >= wait_ms or (received and idle >= 100):
                return bytes(received), status
            self.sleep(poll_ms)
            waited += poll_ms


def candidates(direction, output, bits, level='low'):
    if bits is not None:
        return list(bits)
    if level == 'high':
        # Lowを出している端子と入力の端子。Highを出している端子は対象外。
        return [bit for bit in range(16)
                if not (direction >> bit & 1 and output >> bit & 1) and bit not in TUNER_BITS]
    return [bit for bit in range(16)
            if direction >> bit & 1 and output >> bit & 1 and bit not in TUNER_BITS]


def check_pins(pins):
    direction = pins.high(PIN_DIRECTION)
    output = pins.high(PIN_OUTPUT)
    if direction != EXPECTED_DIRECTION or output != EXPECTED_OUTPUT:
        raise ValueError(f'unexpected pin state: {PIN_DIRECTION:#x}={direction:#06x}, '
                         f'{PIN_OUTPUT:#x}={output:#06x} (expected {EXPECTED_DIRECTION:#06x}, '
                         f'{EXPECTED_OUTPUT:#06x}; receive once with DriverHost_PX4 first)')
    return direction, output


def drive_low(pins, bit, direction, output):
    """bitをLowで駆動する。入力の端子は、出力値をLowにしてから出力を許可する。"""
    mask = 1 << bit
    pins.set_high(PIN_OUTPUT, output & ~mask)
    if not direction & mask:
        pins.set_high(PIN_DIRECTION, direction | mask)


def drive_high(pins, bit, direction, output):
    """bitをHighで駆動する。入力の端子は、出力値をHighにしてから出力を許可する。"""
    mask = 1 << bit
    pins.set_high(PIN_OUTPUT, output | mask)
    if not direction & mask:
        pins.set_high(PIN_DIRECTION, direction | mask)


def probe_bit_high(pins, uart, bit, direction, output, sleep, wait_ms=ATR_WAIT_MS,
                   poll_ms=POLL_MS):
    """受信を準備した後に端子をHighにし、RSTINの立ち上がりで送られるATRを待つ。"""
    mask = 1 << bit
    uart.run(UART_POWER_DOWN)
    try:
        before = uart.read(STATUS)
        uart.run(UART_RESET)
        drive_high(pins, bit, direction, output)
        low = uart.read(STATUS)
        atr, after = uart.wait_atr(wait_ms, poll_ms)
    finally:
        try:
            uart.run(UART_POWER_DOWN)
        finally:
            restore_pins(pins, direction, output)
            sleep(HIGH_HOLD_MS)
    return {'bit': bit, 'input': not direction & mask, 'status_before': before,
            'status_low': low, 'status_after': after, 'atr': atr}


def restore_pins(pins, direction, output):
    # 出力許可を先に戻し、入力へ戻した端子をLowで駆動し続けない。
    pins.set_high(PIN_DIRECTION, direction)
    pins.set_high(PIN_OUTPUT, output)


def probe_bit(pins, uart, bit, direction, output, sleep, wait_ms=ATR_WAIT_MS, poll_ms=POLL_MS):
    mask = 1 << bit
    uart.run(UART_POWER_DOWN)
    try:
        # 試験する端子をHighに保ち、TDA8034ATに5 Vを選ばせる。入力の端子は出力値だけ
        # 変わり、外部のプルアップでHighのままになる。
        pins.set_high(PIN_OUTPUT, output | mask)
        sleep(HIGH_HOLD_MS)
        before = uart.read(STATUS)
        drive_low(pins, bit, direction, output | mask)
        sleep(ACTIVATE_MS)
        low = uart.read(STATUS)
        uart.run(UART_RESET)
        atr, after = uart.wait_atr(wait_ms, poll_ms)
    finally:
        try:
            uart.run(UART_POWER_DOWN)
        finally:
            restore_pins(pins, direction, output)
            sleep(HIGH_HOLD_MS)
    return {'bit': bit, 'input': not direction & mask, 'status_before': before,
            'status_low': low, 'status_after': after, 'atr': atr}


def run(memory, bits, sleep, level='low', wait_ms=ATR_WAIT_MS):
    sdram_probe.check_boot(memory)
    sdram_probe.require_idle(memory)
    pins = Pins(memory)
    uart = Uart(memory, sleep)
    direction, output = check_pins(pins)
    if not uart.present():
        raise ValueError(f'no card detected ({STATUS:#x} bit 10 is 0)')
    print(f'boot0=matched direction={direction:#06x} output={output:#06x} level={level}',
          flush=True)
    step = probe_bit_high if level == 'high' else probe_bit
    results = []
    for bit in candidates(direction, output, bits, level):
        result = step(pins, uart, bit, direction, output, sleep, wait_ms)
        results.append(result)
        print(f'bit={bit:2d} input={"yes" if result["input"] else "no"} '
              f'status={result["status_before"]:#x}->{result["status_low"]:#x}->'
              f'{result["status_after"]:#x} atr={result["atr"].hex() or "none"}', flush=True)
    if (pins.high(PIN_DIRECTION), pins.high(PIN_OUTPUT)) != (direction, output):
        raise OSError('pin state was not restored')
    print('restore=verified')
    return results


def bit_list(text):
    try:
        bits = [int(part, 0) for part in text.split(',') if part]
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if not bits or any(not 0 <= bit < 16 for bit in bits):
        raise argparse.ArgumentTypeError('bits must be 0..15')
    return bits


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bits', type=bit_list,
                        help='試すビット（例: 2,5,7）。既定はHighで出力しているビットのうち、'
                             'チューナーのbit 0と6を除いたもの')
    parser.add_argument('--level', choices=('low', 'high'), default='low',
                        help='端子を駆動する向き。highの既定の対象は、Lowを出している端子と'
                             '入力の端子のうち、bit 0と6を除いたもの')
    parser.add_argument('--wait-ms', type=int, default=ATR_WAIT_MS,
                        help='ATRを待つ時間。テスターで電圧を測るときは長くする')
    parser.add_argument('--interface', help='WinUSBのデバイスインターフェイスのパス')
    args = parser.parse_args()
    if args.wait_ms <= 0:
        parser.error('--wait-ms must be positive')
    paths = [args.interface] if args.interface else sdram_probe.interface_paths()
    if len(paths) != 1:
        parser.error(f'expected one GV-MVP/XZ3 interface, found {len(paths)}')
    with sdram_probe.open_device(paths[0]) as memory:
        results = run(memory, args.bits, lambda ms: time.sleep(ms / 1000), args.level,
                      args.wait_ms)
    return 0 if any(result['atr'] for result in results) else 1


if __name__ == '__main__':
    sys.exit(main())
