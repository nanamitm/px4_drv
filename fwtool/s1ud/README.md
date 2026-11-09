# PX-S1UD RAM probes

These probes target the Rio-A0 boot ROM observed on the PX-S1UD (USB 3275:0080).
They run from RAM. Disconnect and reconnect USB to return to the boot ROM.
Close any application or DriverHost instance that has opened the receiver.

Build with Python 3 and GNU binutils for ARC (tested with 2.42).

```sh
python3 fwtool/s1ud/build_probe.py /tmp/s1ud-boot.inp
python3 fwtool/s1ud/build_probe.py --probe bulk /tmp/s1ud-bulk.inp
```

The builder also accepts `--cross-prefix` for a locally extracted toolchain.
It writes a new file with a 12-byte little-endian header and the linked image
at `0x40260`.

For both images, confirm ROM mode `255` with request 668, write the payload
with request 660 in chunks of at most 240 bytes, verify its little-endian
32-bit word sum with request 662, then invoke request 664 with
`[0x40260, 6, 0x200, 0, 4]`. Each request uses Siano HIF destination 11.

- The boot probe stores `0x53315544` in its own image at offset `0x40` and
  returns to ROM. Request 662 over the same range reports the new sum.
- The bulk probe identifies itself as `PX-S1UD RAM PROBE` (mode 254). Send
  request 890 with the argument 1 to submit one fixed-size 892 frame. Its
  payload is 14,720 bytes of `0x5a` followed by a zero 32-bit word. Request
  890 with the argument 0 queries the 891 payload, five little-endian words:
  result, accepted count, completion count, busy flag, rejection count.
  Poll until the completed count catches up and the busy flag clears before
  reusing the buffer. A single successful transfer reports `[0, 1, 1, 0, 0]`
  after completion.
- Request 890 with the argument 2 reads a fixed list of clock, tuner, and
  capture registers. Reply 893 contains 23 little-endian (address, value)
  pairs. The host cannot supply a register address. This provides a
  baseline before adding RF initialization.

- Request 890 with argument 3 tests write access to the PLL frequency
  field in register `0x852008`. It attempts the fixed 13ch setting,
  preserving bits 0–9, then restores the original register word.
  Argument 4 runs the same test with bit 1 of `0x820010` temporarily
  cleared. Reply 894 contains six little-endian words: original PLL,
  attempted PLL, immediate PLL readback, restored PLL readback,
  control readback during the test, and restored control readback.
  The last two words are zero for argument 3.

These probes do not tune or capture RF.

## RF comparison on PX-S1UD

A RAM-only diagnostic of the reference firmware read 23 registers while
idle and while capturing. Each frequency returned 120 HIF 892 I/Q frames
before capture was stopped.

| Frequency (Hz) | PLL register `0x852008` | Integer N | Signed fraction |
| ---: | ---: | ---: | ---: |
| 473142857 | `0xdb6c5000` | 20 | -2341 |
| 479142857 | `0xfb6c5000` | 20 | -293 |
| 138750000 | `0xc0017600` | 93 | -4096 |

For these measurements the reference clock selection was 12 MHz
(bit 5 clear in `0x8300f8`). The PLL step was twice that clock for
the two UHF frequencies and one eighth of it at 138.75 MHz. Round
frequency divided by step to get N, rounding a half up. Round
`(frequency - N * step) * 8192 / step` to get the signed fraction.
The reference preserves bits 0–9 of `0x852008`, places N in bits
10–17, and places the 14-bit two's complement fraction in bits 18–31.
The active low bits were `0x000` for both UHF measurements and
`0x200` at 138.75 MHz.

During all three captures, clock registers `0x82005c` and
`0x820060` read `0x01050006` and `0x00000013`. Capture registers
`0x856000`, `0x856014`, and `0x856018` read `0x0002001a`,
`0x00f68500`, and `0x00f6f800`; all three were zero in the earlier warm-reload idle measurement.
These are register snapshots. The initialization write order and
the capture start sequence still need to be verified on the RAM probe.

### Selection and capture transitions

With the reference diagnostic already loaded, the host polled the same
23 registers around each tune and stop request. Reply 891 arrived while
the PLL still held the previous frequency. The two UHF tunes then
passed through these observed states:

| State | PLL register at 13ch | Capture control | Capture buffer |
| --- | --- | --- | --- |
| Early selection | `0xdb6c5000` | `0x0` | 0 / 0 |
| Intermediate capture | `0xdb6c5002` | `0x12` | `0xd00000` / `0xd000c0` |
| Intermediate PLL settings | `0x1b6c5000`, then `0x1b6c4c00` | `0x12` | `0xd00000` / `0xd000c0` |
| PLL returned to target | `0xdb6c5000` | `0x12` | `0xd00000` / `0xd000c0` |
| Streaming | `0xdb6c5000` | `0x2001a` | `0xf68500` / `0xf6f800` |

The first host-observed I/Q arrived about 76 ms after the 13ch reply
and 67 ms after the 14ch reply in this run. These are host polling
times. The snapshots can miss intermediate writes. After stopping an
active stream, capture control and buffer registers returned to zero.
The PLL and clock register values persisted.

### Cold boot and PLL write test

Before the first tune after a ROM boot, the reference diagnostic
reported capture control `0x12` and the temporary buffer range
`0xd00000` to `0xd000c0`. The first snapshot was 0.45 s after
starting the firmware upload. All 23 values were unchanged in a
second snapshot one second later. The reference clock registers
`0x82005c` and `0x820060` were still zero. The first tune cleared
the temporary buffer registers at acknowledgement, and the host
subsequently received 13ch I/Q.

On a separate ROM boot, the RAM probe attempted `0xdb6c5000` in
`0x852008`. The original value, immediate readback, and readback
after restore were all zero. All 23 diagnostic values were unchanged
after the test. The fixed USB transfer and its completion still
succeeded. The experiment does not identify the prerequisite for
writing this RF register from the RAM probe.

### PLL access with the early control state

On the first reference tune, the target PLL word was observed about
5 ms after acknowledgement while `0x820010` was still `0x10` and
`0x82005c` and `0x820060` were both zero. On a separate ROM boot,
the RAM probe changed `0x820010` from `0x12` to `0x10` by clearing
bit 1 and verified the change by reading it back. The PLL write still
read back as zero, as in the control run without the bit change.
Restoring `0x820010` returned all 23 measured registers to their
initial values. The fixed USB transfer and its completion succeeded.

### RF access before the first tune

The cold reference log contains only version (668), download (660),
checksum (662), boot (664), and diagnostic snapshot (615) requests
through the second snapshot. There is no separate host initialization
or tune request in this interval. Comparing cold starts, 17 of 23
registers differ between the reference and RAM probe. The differences
are one control word, 12 RF words from the sampled 0x852000 region,
and four capture words. Both sampled clock registers are zero.

On the first tune, the reference PLL had already reached the 13ch
word in the sample at 0.006 s, with both clock words still zero.
Their first observed nonzero values were in the sample at 0.012 s.
This establishes an order for the observed states but does not
identify the RF access gate or the earlier boot write sequence.

### Warm reload comparison

The untuned reference diagnostic reported `0x00005002` at
`0x852008` before warm reload. Both the normal RAM probe and a
zero-padded copy matching the reference payload length were accepted
by the staging requests, but reload response 705 returned status 1
and the device still reported the reference version. Repeating the
same sequence with the reference image returned status 0. RF state
retention across a handoff to the RAM probe could not be measured,
so the PLL write test was not run under that condition.

The Linux Siano host driver obtains the warm download address from
the little-endian word at payload offset 0x14. The reference image
has 0xe00000 there, while the cold-only probe has an instruction word
at that offset. A scratch build reserved the field and set it to
0xe00000, with an entry branch over the field. Both its natural length
and its reference-length padded variant still returned status 1.
The device continued to report S1UD 1.0 after both attempts.

The transport was tested with the untuned reference as a control
before and after the custom attempts. Keeping the image and declared
payload length fixed, removing the 12 trailing zero bytes or reducing
the transfer chunk from 240 to 120 bytes had no effect on response 705.
The reference returned 0 in all four runs and the reference-length
custom image returned 1 in all three runs. The reference answered
after every attempt and all 23 snapshot values matched before and
after each run.

With the same transport settings, swapping two entries in the
added read-only diagnostic register table also returned status 0.
The next snapshot returned those entries in the swapped order,
confirming that the new payload ran. A separate build with an extra
four-byte word at the end of the diagnostic payload also returned 0.
The baseline was restored and all 23 measured values matched in each
run. These tests show that neither exact byte identity of the tested
diagnostic payload nor its original total length is required.

#### Reload entry checks

The reload execution handler reads two little-endian words from the
staged payload. At offset 0x10 it expects a warm entry address in
the inclusive range 0x4025d through 0x59fff. At offset 0x14 it
expects 0xe00000. These offsets are relative to the payload after
the 12-byte INP header. The entry range comes from the handler's
unsigned comparison, and does not imply that unaligned addresses
can execute ARC instructions.

In separate hardware tests, replacing only the entry word with
0x4025c or 0x5a000, or only the staging word with 0xe00004,
returned status 1. Restoring both words returned status 0, and the
new diagnostic register order proved that the uploaded image ran.

An independent probe was then linked at 0x40300, with that entry
address and the expected staging word in a 0xa0-byte prefix. The
immediate status-1 response disappeared, but USB stopped responding
after request 704. The entry checks and successful startup are
separate milestones. The standalone probe still needs a working
warm entry routine before it can test RF register retention.

#### PLL access after reference warm startup

A scratch diagnostic added to the reference RAM image successfully
ran after a warm reload and executed the same masked PLL write used
by the independent probe. With capture stopped, register 0x852008
read 0x00005002 before the test, 0xdb6c5002 after the write, and
0x00005002 after restoration. The independent ROM-started probe
had read back zero from its write. This confirms that the write
instruction and address work in the reference-initialized context.

A second test first selected 13ch, stopped capture, and then reloaded
the diagnostic image. The PLL was 0xdb6c5000 before reload and
0x00005002 after startup. Twelve of the 23 sampled values changed.
The reference warm path therefore does not retain the tuned register
state through startup. These tests still use reference startup code
and do not establish standalone warm startup or identify the RF
initialization step that makes the PLL write take effect.

#### RF access from a cold standalone probe

With the device booted from ROM directly into the standalone test
probe, clearing bits 0 and 1 of 0x820068 alone did not enable PLL
writes. The control test repeated immediately afterward still read
zero. Applying the observed masks to 0x820000 in the next test
changed that register from 0x0001c19f to 0x00e1e19b
(clear 0x00040004, set 0x00e02000). With the mask active, the PLL
read 0x00005100 and accepted the test value 0xdb6c5100. The test
restored the original PLL word and 0x820000.

After restoration, 13 measured registers differed from their
cold baseline, and a further PLL write without the mask did not
take effect. The fixed USB test and completion still passed.
The individual bits needed for access, the full tuning sequence,
and I/Q capture remain to be established.

#### Standalone RF capture attempts

The standalone capture probes accepted the 13ch and 14ch PLL words,
programmed the 0x7300-byte capture range, and sent six burst buffers
over USB per build. The second build also applied the route selection
and capture start pulse. RF status 0x852014 then changed with frequency,
but capture pointer 0x856024 remained at 0xf68500 and all transferred
bursts still contained the initialization pattern. These files do not
contain acquired I/Q.

A third build enabled additional clock and signal-path controls and
repeated the same six bursts. The PLL status still changed between
channels, while the DMA pointer stayed at the ring base and all
bursts contained only the initialization pattern.

#### ADC and capture transition comparison

In the reference 13ch trial the first snapshot with the capture
pointer inside the I/Q ring occurred at 0.074 s in the host log.
Earlier, the clock-control words changed at 0.013 s and 0x850004
was observed high during ADC calibration at 0.014 s. The ADC result
at 0x85001c then became nonzero. RF status bit 30 was set in
0x852014 before the first I/Q capture. The start route words
0x840280 and 0x8403b0 read 1 and 0x11 at the first moving pointer.

The fourth standalone attempt reproduced the observed clock-control
order and issued a bounded ADC calibration request. The measured
ADC result stayed zero and RF status bit 30 stayed clear for the
reported 13ch and 14ch samples. The capture pointer remained at the
base and all six buffers contained the initialization pattern.
Reproducing the RF lock and ADC calibration behavior remains the
next comparison before changing the USB path.

#### Standalone PLL lock

The fifth standalone probe applied the UHF RF register masks before
the PLL latch sequence and allowed the reference settle time.
The lock indication (bit 30 of 0x852014) was set in all six bursts.
The first sampled status was 0x6d4b80c0 at 13ch and 0x6d971000 at
14ch. A sixth probe applied additional ADC and signal-path setup
and reproduced both lock results. In both builds, the measured ADC
calibration result stayed zero and the capture write pointer stayed
at the ring base. No acquired I/Q was observed.

#### ADC request and capture route readback

Standalone builds 7 through 9 kept the PLL locked and matched the
reference state of 0x820068 during capture. The ADC request at
0x850018 read back 0x80 immediately, but never self-cleared within
the bounded poll. The probe cleared it after timeout and the result
at 0x85001c was zero. In builds 8 and 9, both route words read zero
immediately after writing 1 to 0x840280 and 0x11 to 0x8403b0.
The reference reports those nonzero words during active capture.
Setting the additional startup controls at 0x800010, 0x940008
and 0x90003c did not change that result. No acquired I/Q was observed.

#### Reference route availability before capture

The reference route diagnostic read zero immediately after attempting
to write 0x11 to 0x8403b0 in the idle state. After selecting 13ch
with the same image, 0x840280 read 1, 0x8403b0 read 0x11, and the
capture pointer advanced. In the separate cold reference trace,
0x8407f4 changed from 2 at idle to 3 before capture, and 0x8407fc
changed from all ones at idle to zero during the tune transition
and then to 0x7ffffeb9. The ADC request at 0x850018 was observed
counting down to zero. The exact prerequisite for route access
has not been isolated.

Standalone build 10 attempted the reference capture values 3 and
0x7ffffeb9 at 0x8407f4 and 0x8407fc. Both read back zero, along with
0x840038, 0x8400e0, 0x840280 and 0x8403b0. The sampled system clock
and power values 0x820000, 0x820008, 0x82000c, 0x820054, 0x820064
and 0x820068 matched the active reference values. PLL lock and
the 0x850000 write (0x309) also succeeded. This isolates the current
readback discrepancy to the sampled 0x840xxx block and leaves its
startup prerequisite unresolved. No acquired I/Q was observed.

#### Internal reference checkpoints

Read-only checkpoints in the reference 13ch and 14ch tune path
captured 0x8407f4, 0x8407fc and 0x840038. The three words were
zero immediately after PLL setup. After the clock-control routine
they read 2, 0xffffffff and 0x14. After ADC input configuration
they read 3, 0x7ffffeb9 and 0x40. I/Q was received in both tests.

Standalone build 11 repeated the clock-off and clock-on sequence
after PLL setup and delayed before re-enabling it. The PLL still
locked, but the sampled 0x840xxx words and ADC result remained zero.
All six USB bursts still contained the initialization pattern.

Per-write checkpoints in the reference 13ch and 14ch tune path
showed that 0x8407f4 and 0x8407fc first became nonzero immediately
after ORing 0x00ffe000 into 0x820008. The observed values were 2
and 0xffffffff. They were zero after the four preceding clock writes.
In standalone build 12 the same clock register readbacks matched at
each checkpoint, including the final 0x820008 value of 0x0fffffff.
The two downstream words nevertheless stayed zero, the PLL remained
locked, and the capture pointer stayed at the ring base. This
identifies the observed reference transition, but the shared clock
register values do not reproduce the prerequisite state by themselves.

#### Standalone I/Q bursts after ROM startup setup

The reference startup task calls ROM entry 0x19f0 with a 36-byte
configuration for the 12 MHz board clock. Calling the routine from
the standalone USB command handler caused subsequent version
requests to time out. Build 14 called it from the RAM entry before
installing the USB hooks, using a 0x1000-byte launch stack.
After the final clock write, 0x8407f4 and 0x8407fc then read 2 and
0xffffffff, as in the reference trace.

Build 14 returned three separate 14,720-byte payloads per channel
at 13ch and 14ch, with all six USB completions observed. The
buffers contained no four-byte initialization-pattern words, and
repeated bursts differed. The mean in-band to edge power ratios
were 14.26 dB and 12.34 dB, versus 14.10 dB and 12.35 dB for the
corresponding reference captures. The pointer read at the ring
base in the two late polls, so this verifies burst acquisition
only. Continuous capture, buffer wrap handling, TS decoding,
and J.83B reception were not tested with build 14.

### Build the standalone capture probe

Build the experimental burst image with
`python3 fwtool/s1ud/build_probe.py --probe capture /tmp/s1ud-capture.inp`.
Use a ROM launch stack of `0x1000` in request 664. The capture image
is intended for bench testing on the measured 12 MHz PX-S1UD.

Send request 890 with argument 8 for 13ch or 9 for 14ch. Wait about
60 ms, send argument 10 to stop and pack the capture buffer, then
send argument 1 to read the 892 payload. Poll argument 0 for USB
completion before starting another capture. Each payload contains
14,720 sample bytes in signed I0, I1, Q0, Q1 order, followed by a
four-byte zero word. Negate the signed Q samples for the convention
used by the host decoder. The probe uses a fixed-size capture ring
and has no continuous-stream sequence counter.
