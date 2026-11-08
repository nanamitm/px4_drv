// SPDX-License-Identifier: GPL-3.0-or-later

use msi2500_frontend::{
    j83b::J83bDecoder,
    s1ud::{S1udFrontend, BLOCK_BYTES},
};
use std::env;
use std::fs::{File, OpenOptions};
use std::io::{self, BufReader, BufWriter, Read, Write};

const SAMPLE_RATE: f64 = 12_000_000.0;

/// 入力は Siano の型 0x037C のメッセージ本体 (サンプル 14720 バイトとブロック番号 4 バイト) を並べたもの。
/// 欠落の長さの判定と時間軸の再構成は s1ud.rs を参照
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    let [input, output] = args.as_slice() else {
        return Err("usage: s1ud_j83b INPUT.blocks OUTPUT.ts".into());
    };
    let input = File::open(input)?;
    let length = input.metadata()?.len();
    if length == 0 || length % BLOCK_BYTES as u64 != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Expected complete PX-S1UD stream messages (14720 sample bytes and a 4-byte block number)",
        )
        .into());
    }

    let mut front = S1udFrontend::new(SAMPLE_RATE, 1.0);
    let mut decoder = J83bDecoder::new();
    let mut symbols = Vec::new();
    let mut ts = Vec::new();
    let mut input = BufReader::new(input);
    let mut output = BufWriter::new(
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(output)?,
    );
    let mut block = vec![0; BLOCK_BYTES];
    let mut packets = 0u64;
    for _ in 0..length / BLOCK_BYTES as u64 {
        input.read_exact(&mut block)?;
        symbols.clear();
        if front.push_block(&block, &mut symbols) {
            packets += decoder.stats.packets;
            decoder = J83bDecoder::new();
        }
        ts.clear();
        decoder.push(&symbols, &mut ts);
        output.write_all(&ts)?;
    }
    output.flush()?;
    packets += decoder.stats.packets;
    eprintln!("{:?}", front.stats);
    eprintln!("{:?}", front.demod_stats());
    eprintln!("{:?}", decoder.stats);
    eprintln!("packets {packets}");
    if packets == 0 {
        return Err("No transport stream decoded".into());
    }
    Ok(())
}
