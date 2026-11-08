// SPDX-License-Identifier: GPL-3.0-or-later

use msi2500_frontend::{j83b::J83bDecoder, qam::QamDemod};
use num_complex::Complex32;
use std::env;
use std::fs::{File, OpenOptions};
use std::io::{self, BufReader, BufWriter, Read, Write};

const SAMPLE_RATE: f64 = 12_000_000.0;
const BLOCK_BYTES: usize = 14_720;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    let [input, output] = args.as_slice() else {
        return Err("usage: s1ud_j83b INPUT.iq OUTPUT.ts".into());
    };
    let input = File::open(input)?;
    let length = input.metadata()?.len();
    if length == 0 || length % BLOCK_BYTES as u64 != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Expected complete PX-S1UD sample blocks",
        )
        .into());
    }

    let mut demod = QamDemod::new(SAMPLE_RATE, 1.0);
    let mut decoder = J83bDecoder::new();
    let mut samples = Vec::with_capacity(BLOCK_BYTES / 2);
    let mut symbols = Vec::new();
    let mut ts = Vec::new();
    let mut input = BufReader::new(input);
    let mut output = BufWriter::new(
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(output)?,
    );
    let mut block = [0; BLOCK_BYTES];
    // Input contains only sample bytes from consecutive USB blocks.
    for _ in 0..length / BLOCK_BYTES as u64 {
        input.read_exact(&mut block)?;
        samples.clear();
        // The USB payload packs two consecutive samples as I0, I1, Q0, Q1.
        for pair in block.chunks_exact(4) {
            for i in 0..2 {
                let re = f32::from(pair[i] as i8) / 128.0;
                let im = -f32::from(pair[i + 2] as i8) / 128.0;
                samples.push(Complex32::new(re, im));
            }
        }
        symbols.clear();
        demod.push(&samples, &mut symbols);
        ts.clear();
        decoder.push(&symbols, &mut ts);
        output.write_all(&ts)?;
    }
    output.flush()?;
    eprintln!("{:?}", demod.stats);
    eprintln!("{:?}", decoder.stats);
    if decoder.stats.packets == 0 {
        return Err("No transport stream decoded".into());
    }
    Ok(())
}
