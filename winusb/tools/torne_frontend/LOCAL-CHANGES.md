# Local adapter changes

The normal DriverHost now uses a generated C++ port (`dib8096_native_generated.inc`)
and a native bus adapter. The Go executable remains a diagnostic reference only.
`port_native.go.txt` is the development-only AST/type-aware porting tool;
`tests/check_torne_native.py` compares all mock I2C/wait/result events with the
independent Go fakeChip/fakeClock fixtures in `native_trace_test.go`.
The native generated sources retain the same GPL-2.0-only copyright notices.

The pinned revision is 471328e03942219544e78004e961d8f69d27f46f. The optional
TMCC memory and capacitor-trim interfaces are adapted by hand in
`dib8096_native.cpp`; their implementations submit bounded core-1 jobs through
the synchronous mailbox adapter. Cache tables retain the upstream 64-entry RAM
layout and magic. The adapter neither shares the mailbox with an outstanding
ring request nor extends the caller's absolute deadline for a job.
Native/Go trace fixtures also cover cache recall, changed transmission parameters,
capacitor-trim dispatch and the one-second initial-search retry.

Upstream source is pinned by `upstream.json`; original demodulator timing and search code are retained.
The local `internal/coaxd/demod/dib8096/tune_result.go` adds `SearchFailed()` to expose an existing
terminal state (`statusTuneFailed` and `demodStop`) after `Tune` returns without an I/O error.
It does not implement a signal-strength threshold or shorten acquisition timeouts.

The stdio adapter reports this result as a successful `0x11` response with body `01`.
An empty body retains its legacy meaning. Transport errors remain separate error responses.
Tests exercise no-signal search completion and exclusion of pending/locked states.

The additional files use GPL-2.0-only. Retain upstream copyright and license notices and include
these adapter sources and build instructions with corresponding-source distribution.
