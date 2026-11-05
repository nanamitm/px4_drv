//! msi2500_frontend: RSP1 互換機の helper。
//!
//! 現在のサブコマンド:
//!   capture [--seconds N] [--out PATH|-] [--freq HZ] [--gain DB] [--bw HZ] [--frontend default|sdrplay] [--index N]
//!     8,126,984 S/s・384 形式で取り込み、開始区間を除いた生ブロック (1024 バイト単位) を書き出す。
//!     毎秒の統計を標準エラーへ出す。終了コード: 0 正常、2 引数の誤り、3 デバイスの失敗。

use msi2500_frontend::blocks::{BlockCounter, BlockState};
use msi2500_frontend::regs::{Format, FrontEnd, IfMode, Tuner};
use msi2500_frontend::usb::{capture, CaptureLimits, CaptureStats, Device, SDRPLAY_INTERFACE_GUID};
use std::io::Write;
use std::time::Instant;

const SAMPLE_RATE: u32 = 8_126_984;

struct Args {
    seconds: f64,
    out: Option<String>,
    freq: u32,
    gain: i32,
    bw: u32,
    front_end: FrontEnd,
    index: u32,
}

fn parse(mut it: impl Iterator<Item = String>) -> Result<Args, String> {
    let mut a = Args { seconds: 10.0, out: None, freq: 473_142_857, gain: 40, bw: 6_000_000, front_end: FrontEnd::Default, index: 0 };
    while let Some(k) = it.next() {
        let mut v = || it.next().ok_or(format!("{k} needs a value"));
        match k.as_str() {
            "--seconds" => a.seconds = v()?.parse().map_err(|e| format!("--seconds: {e}"))?,
            "--out" => a.out = Some(v()?),
            "--freq" => a.freq = v()?.parse().map_err(|e| format!("--freq: {e}"))?,
            "--gain" => a.gain = v()?.parse().map_err(|e| format!("--gain: {e}"))?,
            "--bw" => a.bw = v()?.parse().map_err(|e| format!("--bw: {e}"))?,
            "--index" => a.index = v()?.parse().map_err(|e| format!("--index: {e}"))?,
            "--frontend" => {
                a.front_end = match v()?.as_str() {
                    "default" => FrontEnd::Default,
                    "sdrplay" => FrontEnd::SdrPlay,
                    x => return Err(format!("unknown --frontend {x}")),
                }
            }
            x => return Err(format!("unknown argument {x}")),
        }
    }
    if !(0..=102).contains(&a.gain) {
        return Err("--gain must be 0..102".into());
    }
    Ok(a)
}

fn run_capture(a: Args) -> i32 {
    let mut t = Tuner::new();
    t.front_end = a.front_end;
    t.format_auto = false;
    t.format = Format::S384;
    t.rate = SAMPLE_RATE;
    t.freq = a.freq;
    t.if_mode = IfMode::Zero;
    let _ = t.set_bandwidth(a.bw);
    let _ = t.set_tuner_gain(a.gain);
    let setup = t.configure_sequence();

    let dev = match Device::open(&SDRPLAY_INTERFACE_GUID, a.index) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("[capture] open failed: {e}");
            return 3;
        }
    };
    eprintln!("[capture] {} freq={} gain={} bw={} frontend={:?} writes={}", dev.path, a.freq, a.gain, a.bw, a.front_end, setup.len());
    let mut out: Option<Box<dyn Write>> = match a.out.as_deref() {
        None => None,
        Some("-") => Some(Box::new(std::io::BufWriter::with_capacity(1 << 20, std::io::stdout()))),
        Some(p) => Some(Box::new(std::io::BufWriter::with_capacity(1 << 20, std::fs::File::create(p).expect("output")))),
    };
    let mut counter = BlockCounter::new(Format::S384.samples_per_block());
    let mut stats = CaptureStats::default();
    let t0 = Instant::now();
    let mut next_report = 1.0f64;
    let mut write_failed = false;
    let r = capture(&dev, &setup, &CaptureLimits::default(), &mut stats, |blk, st| {
        let prev_blocks = counter.blocks;
        let state = counter.push(blk);
        let addr = u32::from_le_bytes([blk[0], blk[1], blk[2], blk[3]]);
        match state {
            BlockState::Gap(n) => eprintln!("[capture] gap: {n} samples lost at {:.3}s (block {prev_blocks}, counter 0x{addr:08x})", t0.elapsed().as_secs_f64()),
            BlockState::Backward => eprintln!("[capture] counter went backward at {:.3}s (block {prev_blocks}, counter 0x{addr:08x})", t0.elapsed().as_secs_f64()),
            _ => {}
        }
        if state != BlockState::Startup {
            if let Some(w) = out.as_mut() {
                if w.write_all(blk).is_err() {
                    write_failed = true;
                    return false;
                }
            }
        }
        let el = t0.elapsed().as_secs_f64();
        if el >= next_report {
            next_report += 1.0;
            eprintln!("[capture] t={el:7.2}s blocks={} startup={} lost={} gaps={} backward={} bad_packets={} transfer_errors={} empty={}/{}",
                counter.blocks, counter.startup_blocks, counter.lost_samples, counter.gaps, counter.backward,
                st.bad_packets, st.transfer_errors, st.empty_packets, st.packets);
        }
        el < a.seconds
    });
    if let Some(mut w) = out {
        let _ = w.flush();
    }
    let el = t0.elapsed().as_secs_f64();
    let rate = counter.blocks as f64 * 384.0 / el.max(1e-9);
    eprintln!("[capture] total: {:.2}s blocks={} samples={} startup_blocks={} lost={} gaps={} backward={} bad_packets={} transfer_errors={} mean_rate={:.0}",
        el, counter.blocks, counter.blocks * 384, counter.startup_blocks, counter.lost_samples, counter.gaps, counter.backward,
        stats.bad_packets, stats.transfer_errors, rate);
    match r {
        Ok(()) if !write_failed => 0,
        Ok(()) => {
            eprintln!("[capture] output write failed");
            3
        }
        Err(e) => {
            eprintln!("[capture] stopped: {e}");
            3
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let code = match args.next().as_deref() {
        Some("capture") => match parse(args) {
            Ok(a) => run_capture(a),
            Err(e) => {
                eprintln!("{e}");
                2
            }
        },
        _ => {
            eprintln!("usage: msi2500_frontend capture [--seconds N] [--out PATH|-] [--freq HZ] [--gain DB] [--bw HZ] [--frontend default|sdrplay] [--index N]");
            2
        }
    };
    std::process::exit(code);
}
