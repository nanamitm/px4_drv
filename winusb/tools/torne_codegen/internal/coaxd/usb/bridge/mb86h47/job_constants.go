// Copyright 2026 Till0196
// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
// Build-time constants extracted from pinned job.go.
package mb86h47
import "time"
const (
	jobArea     = 0xa3a00
	jobSequence = 0x00
	jobKind     = 0x04
	jobParam    = 0x08
	jobDone     = 0x40
	jobError    = 0x44
	jobResult   = 0x48
	jobSpin     = 0x60
	jobParams   = 14
	jobResults  = 3

	// jobMailbox is the mailbox layout a job builds its transfers in.
	jobMailbox = 0xa3b00
)
const (
	tableBase    = 0xa2800
	tableEntry   = 16
	tableEntries = 64
	tableHeader  = tableBase + tableEntries*tableEntry
	tableNext    = tableHeader + 4
	tableMagic   = 0x7ab1e002

	// Bits of an entry's second word.
	tableTMCC    = 1 << 8
	tableCaptrim = 1 << 9

	// programLimit is where core 1's program has to end: the table follows.
	programLimit = tableBase
)
const (
	// jobSpinKind runs the wait loop for the number of passes in parameter 0.
	jobSpinKind = 1
	// jobCaptrimKind is the DiB0090's capacitor trim search; see Captrim.
	jobCaptrimKind = 2
	// jobTMCCRecallKind gives the transmission parameters remembered for a
	// frequency; see RecallTMCC.
	jobTMCCRecallKind = 3
	// jobTMCCRememberKind reads the parameters the demodulator locked with and
	// remembers them; see RememberTMCC.
	jobTMCCRememberKind = 4
)
const (
	spinCalibration = 100 * time.Millisecond
	// spinPasses is more than core 1 makes in the calibration; the job is ended
	// once read.
	spinPasses = 100_000_000
)

const rememberedStep = 4
