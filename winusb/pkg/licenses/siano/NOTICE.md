# Siano PX-S1UD backend

The protocol.c and protocol.h files are copied without modification from
[Khronos31/siano-userland](https://github.com/Khronos31/siano-userland),
commit `2f32c2a34665d6ccca39c9362eb786be78351347` (GPL-2.0-or-later).
The frontend.cpp control sequence is adapted from that revision's siano-device.c.
The combined DriverHost is distributed under GPL version 2; COPYING contains
the GPL version 2 text. Corresponding modified sources are included in the
px4_drv source distribution under winusb/src/DriverHost_PX4/siano/.

Statistics field offsets and SNR units follow Linux v6.12
drivers/media/common/siano/smscoreapi.h and smsdvb-main.c.

Local adaptation: native WinUSB transport owned by DriverHost_PX4, bounded
command waits, pending-I/O cancellation and completion, receiver integration,
channel/capture generation checks and existing plain TS framing.
The libusb daemon and dtv-userland Core/P adapters are not included.

## Firmware

`firmware/isdbt_rio.inp` is an unmodified Siano firmware binary.
Copyright (c) 2005-2014 Siano Mobile Silicon Ltd. All rights reserved.
The redistribution conditions and disclaimer are reproduced in LICENCE.siano.
No reverse engineering, decompilation or disassembly of this firmware is permitted.

Upstream provenance: linux-firmware commit
`e981caea6ed33c48d25b7dbf473327dbd01df163`, `isdbt_rio.inp`.
The binary was retrieved from the wkennington/linux-firmware GitHub mirror;
its SHA-256 equals the pinned upstream input recorded by siano-userland:
`054520642d5d09cb7ab7d08dbd6fd9ba9365de56adf2e7d7d06927f9845ff818`.
