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

#### Capture window and 138.75 MHz test

The standalone window probe polled the capture pointer for 10,000
hardware timer ticks after one start. In nine trials across 13ch,
14ch, and 138.75 MHz the pointer reached the end of the capture
range, returned to the base once, and stayed there. The probe
returned a burst for each trial. This confirms that one start
does not produce a continuously wrapping stream in this setup.

At 138.75 MHz the PLL locked and three standalone I/Q bursts were
captured. Their complex RMS was 4.02 versus 35.94 for the reference
capture at that frequency. Each burst was submitted separately to
serve-s1ud with a zero tag. The decoder consumed the block, but did
not acquire a frame or return a TS packet. The bursts have independent
start times and do not provide a continuous stream for evaluating
J.83B reception or measuring the sample loss rate.

#### Back-to-back capture batches

The batch probe alternates two DMA ranges and captures up to eight
tagged blocks before the host reads them. The initial probes often
stopped reporting after two blocks. Waiting for the next pointer to
advance before packing the preceding block allowed the host to
recognize a completed capture even when packing lasted through the
entire DMA window. The corrected trial received eight unique blocks
for 13ch and for each of five gain settings at 138.75 MHz, with
48 accepted USB submissions, 48 completions, and no rejects.

The completion-observation intervals alternated between 617–618 and
844–856 timer ticks. These observations include polling and packing
latency. They indicate a sample timing gap but do not give an exact
sample loss count. The maximum mean-removed complex RMS among the
138.75 MHz gain settings was 5.69 at field value 63, compared with
35.94 for the earlier reference capture. Each eight-block batch was
passed separately to serve-s1ud. No frame synchronization or TS
packets were reported.

The capture probe also supports a bounded batch test. Send argument
11 to tune 138.75 MHz, or 8/9 for the terrestrial controls. An
optional argument in the range 24–87 writes the six-bit RF gain
field with the argument minus 24. Send argument 12 to acquire up
to eight blocks and stop capture. This command returns after the
batch finishes or its bounded DMA wait expires.

#### Continuous streaming

Request 890 has the same number as the reference J.83B request 0x37A.
An argument of 90,000,000 or more is a frequency in Hz. It starts
a continuous stream of 892 (0x37C) blocks, like the reference firmware.
Frequencies from 90 MHz to 775 MHz are accepted, the same range as
DriverHost. Frequencies above that range return result 1.

The tuning follows the reference routines at 0x47afc and 0x478b4.
The sub-band boundaries are 84, 150, and 360 MHz. Below 84 MHz the PLL
step is one sixteenth of the 12 MHz reference. Below 360 MHz it is one
eighth, and above that it is twice the reference.
N is the frequency divided by the step, with a half rounded up. The
signed 14-bit fraction is the remainder times 8192 divided by the step,
rounded the same way. Bits 8 and 9 are 0x200 below 360 MHz. A 23-entry
table of (address, mask, value per sub-band) sets the RF registers. It
contains the same values as the reference table at 0x4ae84. Bit 18 of
0x852040 is set below 360 MHz, and bit 2 is cleared. The computed PLL
word matched the reference at all 18 sampled frequencies from 93 MHz to
765 MHz. 0x852004, 0x852040, and bits 19–20 of 0x852000 also matched at
all 18. The ADC path and clock registers did not change with frequency
in the reference.

The RF gain field of 0x852000 is set by an AGC in the streaming task.
There are 10 steps below 360 MHz and 5 above. They are gain values that
the reference AGC selected while streaming. Each step raised the level
by 1.3–2 times on 327.25, 333.75, and 138.75 MHz, and the high band
order was checked on 580.75 MHz. The level comes from 64 words spread
over each packed block. The absolute values are summed over 32 blocks.
The AGC moves one step up below a mean absolute value of 14, which is
a complex RMS of about 25, and one step down above 32, about 57.
An earlier lower limit of 11 left weak channels near RMS 20, and their
MER was 1.5–2 dB below the reference. `agc_limits` holds the two sums.
A nonzero `gain_override` fixes 0x852000 to that value and disables
the AGC. Both can be written with argument 100 before a stream starts.

On this cable feed, the reference firmware decoded 7 of the 16 J.83B
channels in the BonDriver channel list. The 4 s v32 recordings decoded
the same 7 channels:

| Channel (MHz) | AGC step | MER (dB) | Reference MER (dB) |
| ---: | ---: | ---: | ---: |
| 132.25 | 1 | 30.8 | 30.9 |
| 138.75 | 1 | 31.7 | 32.0 |
| 281.75 | 1 | 32.0 | 30.9 |
| 288.25 | 1 | 31.2 | 31.2 |
| 327.25 | 6 | 30.0 | 30.9 |
| 333.75 | 7 | 30.2 | 30.1 |
| 580.75 | 2 | 33.6 | 32.7 |

The other 9 channels did not decode with either firmware. Frequencies
below 90 MHz were not tested. After the first frequency request, argument 0 stops
the stream. Both replies are 891 (0x37B) with seven little-endian words:
result, filled, sent, late, dropped, timeouts, and errors. The counts
accumulate from the first start.

The first frequency request creates a ROM task, using ROM entry 0x14bdc
with the reference arguments. The task streams, so the stop request is
still processed during the stream. The capture ring holds exactly one
block, as in the reference. The task packs samples behind the DMA write
pointer. When the DMA writes the last ring word (0xf6f7fc), the task
re-arms it at once. Interrupts are masked with ROM entries 0xb64 and 0xb94
from the end-of-ring wait to the restart, as in the reference. Without
the mask, 40% of the block boundaries lost more than the 4–7 samples
expected by `s1ud.rs`. The decoder then failed its gap trial and
resynchronized frequently. Twelve 0x4000-byte slots at 0xe20000 are sent
in rotation. A slot that is still waiting for USB completion is not packed.
That block is counted as dropped, and the 20-bit sequence number still
advances. Bits 20–21 of the tag are 1 for an on-time block and 2 for the
first block or a block after a late re-arm.

The level difference at 138.75 MHz was caused by bit 18 of 0x852040.
The cable PLL mask sequence clears it. The reference sets it while
streaming (0x852040 = 0xfab67fa1). Setting it raised the mean-removed
complex RMS from 1.04 to 35.7. The reference value is 35.6. Writing
the other DSP registers that differ from the reference did not change
the level or decoding. They include 0x84007c, 0x84037c, and the
0x841100–0x8414f4 blocks.

A 20 s run was taken with a Python WinUSB reader. It received 32,044
blocks, and 460 were dropped when the reader fell behind. The first
11,000 blocks had no drops. `s1ud_j83b` decoded them to 180,366 TS
packets at MER 31.5 dB, with 49 uncorrectable RS blocks and no
demodulator restart. 11,000 reference-firmware blocks from the same
channel gave 178,053 packets, with 655 blocks dropped by the chip.
DriverHost cold-boot loading, other frequencies, and long runs have not
been tested.

#### Reloading another firmware

The first 0xa0 bytes of the image now use the reference layout. The
cold entry is at offset 0, the warm entry address at 0x10, and the
staging address 0xe00000 at 0x14. The probe handles the warm reload
requests that DriverHost sends, in this order:

- 702 stops any stream and replies 703.
- 660 writes data at 0xe00000-based addresses into 0xe20000. RAM at
  0xe00000 is only 64 KB, mirrored at 0xe10000, so it cannot hold
  a whole image.
- 662 returns the word sum of that staging area. 662 for other
  addresses is left to the ROM.
- 704 checks the staged warm entry (0x4025d–0x59fff) and the staging
  word, as the ROM does. A routine in the first 0xa0 bytes then saves
  the ROM work area 0x40240–0x4025f to 0xe50000, writes the warm entry
  to 0x40200, and copies the image body to 0x40300. It then calls the
  ROM soft reset at 0x123c.

From the cold-booted probe, this sequence loaded the reference J.83B
firmware without unplugging the device. Its IQ stream then worked.

The ROM reset handler calls the warm entry with the reset cause. When
the entry returns nonzero, the ROM initializes the heap and USB stack.
It then calls the hook at 0x40218, and finally `_start` through
0x4021c. The CPU reset keeps the USB connection, so the host does not
enumerate the device again. A firmware that lets the ROM rebuild its
USB stack from scratch therefore loses the host. In an early build the
warm entry returned 0. After that reset the device stayed present, but
every request timed out. Without administrator rights,
`CM_Reenumerate_DevNode` returned `CR_ACCESS_DENIED`.
`pnputil /restart-device`, run as administrator, left the port showing
"Device Descriptor Request Failed".

The probe now keeps the ROM USB state across the reset, as the
reference firmware does:

- The warm entry restores the ROM work area saved before the reset and
  the reset cause at 0x40250, then clears the hooks at 0x40200–0x4023f.
- It sets the clock registers again in the reference order, with
  interrupts masked.
- It calls ROM 0x5fa4 with 0 and 1.
- It copies the ROM USB state to 0xe40000 with the marker 0xaa11bb22.
  The state is the words at 0xf67a38 and 0xf67fa8, the halfwords at
  0xf67f10 and 0xf67f12, and the byte at 0xf66bc4. It also includes the
  heap blocks referenced by 0xf67a24, 0xf67a28, 0xf67a2c, 0xf67a30,
  0xf67a44, and 0xf67a34. Two blocks referenced from the 0xf67a30
  structure at +0x30 and +0x34 are copied as well, and so is the
  variable-length block at 0xf67a3c.
- It calls ROM 0x1604, then restores the image prefix at 0x40260.
- It installs the HIF, restore, start, and 0x4023c hooks, and returns 1.

The 0x40218 hook runs after the ROM has reinitialized its heap. It
allocates heap copies of the saved blocks and writes their pointers
back to the ROM variables. It restores the ROM USB callback table,
which consists of ROM function addresses from 0x63e0 to 0x67c0. It then
rebuilds the links inside the restored structures. Finally it calls ROM
0x6d20, 0x128dc, 0x13130, 0x58c0, 0x6030, and 0x12bbc, as the reference
does, and continues with the probe initialization. Cold boot still
configures the board clock with ROM 0x19f0. The warm path does not call
it.

Without unplugging, the probe was loaded from the reference J.83B
firmware and from the normal ISDB-T firmware. Each switch used the
DriverHost reload sequence, and the probe then loaded the other
firmware again. Two round trips with the reference firmware streamed in
every phase. After a switch from the ISDB-T firmware, a 4 s recording at
138.75 MHz decoded to 92,002 TS packets at MER 32.0 dB. The ISDB-T
firmware reached mode 6 after `INIT_DEVICE`.

#### Use as the DriverHost J.83B firmware

The capture probe image can be set as `J83BFirmwarePath` in the
`[PX-S1UD]` section. DriverHost sends `INIT_DEVICE` (578) after
loading a firmware that is not in mode 6, and the probe replies 579.
The DriverHost cold boot uses a 0x200-byte launch stack. Before calling
ROM 0x19f0, the probe switches to its own stack at 0xe6f000.

`siano_live_test j83b:138750 60` ran with the probe as the J.83B
firmware. DriverHost cold-booted the normal firmware and loaded the
probe by warm reload in about 0.8 s. It then streamed for 60 s:
1,598,186 TS packets with no sync or TEI errors and 141 continuity
errors. The chip statistics were filled 98,991, sent 98,931, and
dropped 60. The next `siano_live_test 13 20` reloaded the normal
firmware from the probe. ISDB-T channel 13 gave 232,960 packets with no
continuity errors. The device still answered after 70 s idle in each
firmware. Once, after a script switched from the probe to the ISDB-T
firmware outside DriverHost, the device stopped answering about a
minute later. That has not recurred under DriverHost, and its cause is
unknown. Other channels, long recordings, and BonDriver or TVTest use
are not tested.

#### Send slots and dropped blocks

With 32 overlapped 16 KB host reads, the standalone probe dropped 0.136%
of the blocks in 30 minutes with 12 slots. The reference firmware
dropped 0.114% in 10 minutes. In both, every drop was a block whose
slot was still waiting for USB completion. The host arrival intervals
were mostly 0.60–0.65 ms. Occasional runs of 3–7 ms intervals let the
send queue grow. While it drained, the fastest intervals were
0.40–0.45 ms, only 10–50% above the data rate. When all slots were busy,
one whole round of slots was dropped, so the burst length matched the
slot count. Changing the reference task argument r7 from 0x1e to 1 did
not change the rate.

`slot_config` sets the slot count (up to 64), the first slot address,
the header and descriptor addresses, the slot spacing, the number of
slots before a second region, the second region address, and the
blocks per send (1 or 2). RAM was found at 0xe00000–0xe0ffff (mirrored
at 0xe10000) and 0xe20000–0xe6ffff. In 10-minute runs, 20 slots dropped
0.087%. 26 slots dropped 0.105%, and ROM_SEND_BULK rejected 265 sends,
which suggests a ROM limit of 20 to 25 queued sends. Sending two blocks
per 0x37C message dropped 0.095% with 11 slots, against 0.130% for one
block with 20 slots. That is within the variation between runs of the
same setting (0.059–0.136%). The defaults stay at 12 slots of one block.
A host that receives two-block messages must split the payload into
14,724-byte blocks.

Argument 100 followed by an address and a value writes one register.
It replies with 894: address, value, and readback. Read registers
with the ROM request 662 over a four-byte range. The reply is the word
itself. Avoid sending requests other than stop during a stream. A burst
of about 20 pokes during streaming stopped the probe from responding.

Argument 2 returns the batch trace in 893. The first twelve
(address, value) pairs contain the initial timer value, the number
of completed slots, eight completion timestamps, the final observed
DMA pointer, and the DMA base expected at exit. The remaining
pairs read RF gain, PLL frequency, PLL status, and the capture
pointer. Read only the completed slots with arguments 16–23,
waiting for argument 0 to report USB completion before requesting
the next slot or starting a new capture. Each 892 batch payload
ends with its zero-based slot number as a 32-bit little-endian word.
The tag restarts at zero for each batch.
