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
  after completion. These commands do not tune or capture RF.
