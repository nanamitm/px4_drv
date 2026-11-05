//! msi2500_frontend: RSP1 互換機の helper。
//!
//! 現在のサブコマンド:
//!   capture [--seconds N] [--out PATH|-] [--freq HZ] [--gain DB] [--bw HZ] [--frontend default|sdrplay] [--index N]
//!     8,126,984 S/s・384 形式で取り込み、開始区間を除いた生ブロック (1024 バイト単位) を書き出す。
//!     毎秒の統計を標準エラーへ出す。終了コード: 0 正常、2 引数の誤り、3 デバイスの失敗。
//!   live [--seconds N] [--channel N] [--input RAW_BLOCKS_FILE] [--ts PATH] [--ts-a PATH] [--ts-b PATH] [--viterbi-threads N] [--priority normal|high]
//!     取り込みから OFDM 復調と A・B 階層の誤り訂正までを別スレッドで実時間に処理し、毎秒の統計と CPU 使用率を出す。
//!     --ts は A・B 階層を多重フレームの位置に従って再多重した TS、--ts-a と --ts-b は階層ごとの TS。
//!     --input を指定すると機器の代わりに capture の生ブロックファイルを読む。終了コード: 0 正常、2 引数の誤り、3 デバイスの失敗。
//!   serve --path DEVICE_PATH|--index N --frequency HZ [--gain DB] [--viterbi-threads N]
//!     DriverHost_PX4 から起動される形。標準出力へ RTL-SDR の helper と同じ形式のフレーム (hostproto) で
//!     再多重した TS と状態を出し、標準入力の終わりで停止する。終了コード: 0 正常、1 失敗、2 引数の誤り。
//!   demod RAW_BLOCKS_FILE [TS_OUT_B]
//!     capture の生ブロックを OFDM 復調と誤り訂正にかけ、MER、RS の訂正結果、段ごとの処理時間を出す。
//!     TS_OUT_B を指定すると B 階層の TS を書き出す。終了コード: 0 TMCC と制御キャリヤーを確認、1 未確認、2 入力の誤り。

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
    // live だけが使う
    input: Option<String>,
    ts_a: Option<String>,
    ts_b: Option<String>,
    ts_mux: Option<String>,
    viterbi_threads: usize,
    high_priority: bool,
}

/// UHF 13〜62ch の中心周波数 (473 + 1/7 MHz から 6 MHz 間隔)
fn uhf_channel_hz(ch: u32) -> Option<u32> {
    (13..=62).contains(&ch).then(|| 473_142_857 + (ch - 13) * 6_000_000)
}

fn parse(mut it: impl Iterator<Item = String>) -> Result<Args, String> {
    let mut a = Args {
        seconds: 10.0, out: None, freq: 473_142_857, gain: 40, bw: 6_000_000, front_end: FrontEnd::Default, index: 0,
        input: None, ts_a: None, ts_b: None, ts_mux: None,
        viterbi_threads: std::thread::available_parallelism().map(|n| n.get().min(4)).unwrap_or(2),
        high_priority: false,
    };
    while let Some(k) = it.next() {
        let mut v = || it.next().ok_or(format!("{k} needs a value"));
        match k.as_str() {
            "--seconds" => a.seconds = v()?.parse().map_err(|e| format!("--seconds: {e}"))?,
            "--out" => a.out = Some(v()?),
            "--freq" => a.freq = v()?.parse().map_err(|e| format!("--freq: {e}"))?,
            "--channel" => {
                let ch: u32 = v()?.parse().map_err(|e| format!("--channel: {e}"))?;
                a.freq = uhf_channel_hz(ch).ok_or("--channel must be 13..62")?;
            }
            "--input" => a.input = Some(v()?),
            "--ts-a" => a.ts_a = Some(v()?),
            "--ts-b" => a.ts_b = Some(v()?),
            "--ts" => a.ts_mux = Some(v()?),
            "--viterbi-threads" => a.viterbi_threads = v()?.parse().map_err(|e| format!("--viterbi-threads: {e}"))?,
            "--priority" => {
                a.high_priority = match v()?.as_str() {
                    "normal" => false,
                    "high" => true,
                    x => return Err(format!("unknown --priority {x}")),
                }
            }
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

/// 8,126,984 S/s・384 形式・ゼロ IF の設定列
fn tuner_setup(a: &Args) -> Vec<msi2500_frontend::regs::Op> {
    let mut t = Tuner::new();
    t.front_end = a.front_end;
    t.format_auto = false;
    t.format = Format::S384;
    t.rate = SAMPLE_RATE;
    t.freq = a.freq;
    t.if_mode = IfMode::Zero;
    let _ = t.set_bandwidth(a.bw);
    let _ = t.set_tuner_gain(a.gain);
    t.configure_sequence()
}

fn run_capture(a: Args) -> i32 {
    let setup = tuner_setup(&a);

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
    eprintln!("[capture] total: {:.2}s blocks={} samples={} startup_blocks={} lost={} gaps={} backward={} bad_packets={} odd_packets={} transfer_errors={} mean_rate={:.0}",
        el, counter.blocks, counter.blocks * 384, counter.startup_blocks, counter.lost_samples, counter.gaps, counter.backward,
        stats.bad_packets, stats.odd_packets, stats.transfer_errors, rate);
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

/// 録画した生ブロック (capture --out の出力) を OFDM 復調と誤り訂正にかけ、同期・TMCC・MER・RS の結果を
/// 信号 1 秒ごとに出す。ts_out を指定すると B 階層の TS (188 バイト単位) を書き出す。
fn run_demod(path: &str, ts_out: Option<&str>) -> i32 {
    use msi2500_frontend::fecstream::LayerPipe;
    use msi2500_frontend::ofdm::{Ofdm, FS, L};
    use msi2500_frontend::raw384::decode_block;
    use std::io::Read;
    let f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("[demod] {path}: {e}");
            return 2;
        }
    };
    let mut r = std::io::BufReader::with_capacity(1 << 20, f);
    // capture --out は開始区間を除いて書き出すため、ここでは開始区間を設けずに連続性だけを確かめる
    let mut counter = BlockCounter::with_startup_run(384, 1);
    let mut ofdm = Ofdm::new();
    let mut blk = vec![0u8; 1024];
    let mut batch = Vec::with_capacity(256 * 384);
    let (mut symbols, mut segments) = (0u64, 1u32);
    let mut pipes: Option<(LayerPipe, LayerPipe)> = None;
    let mut done_stats = Vec::new();
    let mut ts_file = ts_out.map(|p| std::io::BufWriter::new(std::fs::File::create(p).expect("ts output")));
    let (mut ts_a, mut ts_b) = (Vec::new(), Vec::new());
    let sym_rate = FS / L as f64;
    let t0 = Instant::now();
    // 同じスレッドで順に処理するため、段ごとの時間を分けて実時間との比を出す
    // 誤り訂正の時間は、同期の捕捉 (初回だけ多数の RS 検査をする) を含む区間と、捕捉後の定常区間に分ける
    let (mut ofdm_time, mut fec_time) = (0f64, 0f64);
    let (mut steady_time, mut steady_symbols) = (0f64, 0u64);
    loop {
        let end = r.read_exact(&mut blk).is_err();
        if !end {
            if let BlockState::Gap(_) | BlockState::Backward = counter.push(&blk) {
                // 不連続の前後をつながず、状態をすべて作り直す
                eprintln!("[demod] discontinuity at block {}; resetting OFDM state", counter.blocks);
                ofdm = Ofdm::new();
                batch.clear();
                segments += 1;
                if let Some((a, b)) = pipes.take() {
                    done_stats.push((a.stats, b.stats));
                }
            }
            decode_block(&blk, &mut batch);
        }
        if batch.len() >= 256 * 384 || (end && !batch.is_empty()) {
            let t_ofdm = Instant::now();
            let syms = ofdm.push(&batch);
            ofdm_time += t_ofdm.elapsed().as_secs_f64();
            let t_fec = Instant::now();
            let steady = pipes.as_ref().is_some_and(|(a, b)| a.stats.acquired && b.stats.acquired);
            if steady {
                steady_symbols += syms.len() as u64;
            }
            for sym in syms {
                symbols += 1;
                if pipes.is_none() {
                    if let Some(t) = sym.tmcc {
                        let na = t.layers[0].nseg as usize;
                        let nb = t.layers[1].nseg as usize;
                        let sa: Vec<usize> = (0..na).collect();
                        let sb: Vec<usize> = (na..na + nb).collect();
                        // B 階層のビタビのスレッド数。4 を超えても処理時間はほとんど縮まなかった
                        let th = std::thread::available_parallelism().map(|n| n.get().min(4)).unwrap_or(2);
                        pipes = Some((LayerPipe::new(&t.layers[0], &sa, 1), LayerPipe::new(&t.layers[1], &sb, th)));
                    }
                }
                if let Some((pa, pb)) = pipes.as_mut() {
                    pa.push(&sym, &mut ts_a);
                    pb.push(&sym, &mut ts_b);
                    if let Some(w) = ts_file.as_mut() {
                        w.write_all(&ts_b).expect("ts write");
                    }
                    ts_a.clear();
                    ts_b.clear();
                }
                if symbols % 882 == 0 {
                    let s = &ofdm.stats;
                    eprintln!("[demod] signal={:7.2}s cfo={:+.1}Hz drift={:+.3}ppm res(d4={:+.4} f={:+.2}Hz pos={:+.2}) MER A/B={:.1}/{:.1}dB",
                        symbols as f64 / sym_rate, s.cfo_hz, s.drift_ppm, s.last_timing_residual, s.last_cfo_residual,
                        s.last_pos_error, s.mer_db[0], s.mer_db[1]);
                }
            }
            let dt = t_fec.elapsed().as_secs_f64();
            fec_time += dt;
            if steady {
                steady_time += dt;
            }
            batch.clear();
        }
        if end {
            break;
        }
    }
    if let Some(mut w) = ts_file {
        let _ = w.flush();
    }
    if let Some((a, b)) = pipes.take() {
        done_stats.push((a.stats, b.stats));
    }
    let mut unc = 0u64;
    for (i, (a, b)) in done_stats.iter().enumerate() {
        unc += a.uncorrectable + b.uncorrectable;
        eprintln!("[demod] segment {i}: B blocks={} clean={} corrected={} uncorrectable={} tei={} cc={} | A blocks={} corrected={} uncorrectable={} cc={}",
            b.blocks, b.clean, b.corrected, b.uncorrectable, b.tei, b.cc_errors, a.blocks, a.corrected, a.uncorrectable, a.cc_errors);
    }
    let s = &ofdm.stats;
    if unc > 0 {
        eprintln!("[demod] uncorrectable RS blocks: {unc}");
    }
    let signal = symbols as f64 / sym_rate;
    let steady_signal = steady_symbols as f64 / sym_rate;
    eprintln!("[demod] time: signal={signal:.2}s ofdm={ofdm_time:.2}s ({:.2}x) fec={fec_time:.2}s ({:.2}x) fec_after_sync={steady_time:.2}s for {steady_signal:.2}s ({:.2}x)",
        ofdm_time / signal.max(1e-9), fec_time / signal.max(1e-9), steady_time / steady_signal.max(1e-9));
    eprintln!("[demod] done: blocks={} symbols_out={symbols} segments={segments} ctrl_matches_center={:?} tmcc={:?} MER A/B={:.2}/{:.2}dB wall={:.2}s",
        counter.blocks, s.ctrl_matches_center, s.tmcc.map(|t| t.layers), s.mer_db[0], s.mer_db[1], t0.elapsed().as_secs_f64());
    if s.tmcc.is_some() && s.ctrl_matches_center == Some(true) { 0 } else { 1 }
}

/// 取り込み (または録画ファイル) から OFDM 復調と誤り訂正までを別スレッドで実時間に処理する。
fn run_live(a: Args) -> i32 {
    use msi2500_frontend::live::{process_usage, Live, LiveConfig, LiveStats};
    let open_out = |p: &Option<String>| -> Result<Option<Box<dyn Write + Send>>, String> {
        match p {
            None => Ok(None),
            Some(p) => std::fs::File::create(p)
                .map(|f| Some(Box::new(std::io::BufWriter::with_capacity(1 << 20, f)) as Box<dyn Write + Send>))
                .map_err(|e| format!("{p}: {e}")),
        }
    };
    let (ts_out, ts_mux) = match (open_out(&a.ts_a), open_out(&a.ts_b), open_out(&a.ts_mux)) {
        (Ok(x), Ok(y), Ok(z)) => ([x, y], z),
        (Err(e), _, _) | (_, Err(e), _) | (_, _, Err(e)) => {
            eprintln!("[live] {e}");
            return 2;
        }
    };
    let cfg = LiveConfig { viterbi_threads: a.viterbi_threads.max(1), raw_queue: 256, symbol_queue: 512, ts_out, ts_mux };
    let (live, mut feeder) = Live::start(cfg);
    let t0 = Instant::now();
    let (cpu0, _, _) = process_usage();
    let (mut last_t, mut last_cpu, mut next_report) = (0f64, cpu0, 1.0f64);
    let mut max_cores = 0f64;
    let mut max_ws = 0usize;
    let mut report = |live: &Live, force: bool| {
        let el = t0.elapsed().as_secs_f64();
        if !force && el < next_report {
            return;
        }
        next_report = el.floor() + 1.0;
        let (cpu, ws, private) = process_usage();
        let cores = (cpu - last_cpu) / (el - last_t).max(1e-9);
        (last_t, last_cpu) = (el, cpu);
        max_cores = max_cores.max(cores);
        max_ws = max_ws.max(ws);
        let s = live.stats.lock().unwrap().clone();
        let (fa, fb) = (s.layer_total(0), s.layer_total(1));
        eprintln!("[live] t={el:7.2}s signal={:7.2}s blocks={} lost={} gaps={} overflow={} seg={} MER A/B={:.1}/{:.1}dB | B blk={} corr={} unc={} tei={} cc={} | A blk={} unc={} cc={} | cpu={cores:.2} ws={}MB private={}MB",
            s.symbols as f64 * L as f64 / FS, s.blocks, s.lost_samples, s.gaps, live.overflow_chunks(), s.segments,
            s.ofdm.mer_db[0], s.ofdm.mer_db[1], fb.blocks, fb.corrected, fb.uncorrectable, fb.tei, fb.cc_errors,
            fa.blocks, fa.uncorrectable, fa.cc_errors, ws >> 20, private >> 20);
    };
    use msi2500_frontend::ofdm::{FS, L};

    let mut device_error = None;
    if let Some(path) = &a.input {
        let f = match std::fs::File::open(path) {
            Ok(f) => f,
            Err(e) => {
                eprintln!("[live] {path}: {e}");
                return 2;
            }
        };
        let mut r = std::io::BufReader::with_capacity(1 << 20, f);
        let mut blk = vec![0u8; 1024];
        while std::io::Read::read_exact(&mut r, &mut blk).is_ok() {
            feeder.push_wait(&blk);
            report(&live, false);
        }
    } else {
        let setup = tuner_setup(&a);
        let dev = match Device::open(&SDRPLAY_INTERFACE_GUID, a.index) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("[live] open failed: {e}");
                return 3;
            }
        };
        if a.high_priority {
            // 取り込みのスレッドは等時転送の完了を待って次の転送を出すため、他の処理より先に動かす
            use windows_sys::Win32::System::Threading::{GetCurrentThread, SetThreadPriority, THREAD_PRIORITY_HIGHEST};
            unsafe { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) };
        }
        eprintln!("[live] {} freq={} gain={} viterbi_threads={} priority={}", dev.path, a.freq, a.gain, a.viterbi_threads,
            if a.high_priority { "high" } else { "normal" });
        let mut stats = CaptureStats::default();
        let r = capture(&dev, &setup, &CaptureLimits::default(), &mut stats, |blk, _| {
            feeder.push(blk);
            report(&live, false);
            t0.elapsed().as_secs_f64() < a.seconds
        });
        if let Err(e) = r {
            eprintln!("[live] capture stopped: {e}");
            device_error = Some(e);
        }
        eprintln!("[live] usb: transfers={} packets={} bad_packets={} odd_packets={} transfer_errors={} last_error=0x{:x} unwritten_blocks={}",
            stats.transfers, stats.packets, stats.bad_packets, stats.odd_packets, stats.transfer_errors, stats.last_error, stats.unwritten_blocks);
    }
    let wall_in = t0.elapsed().as_secs_f64();
    let overflow = live.overflow_chunks();
    let fin: LiveStats = live.finish(feeder);
    let wall = t0.elapsed().as_secs_f64();
    let (cpu, _, _) = process_usage();
    let signal = fin.symbols as f64 * L as f64 / FS;
    for (li, name) in [(1, "B"), (0, "A")] {
        let f = fin.layer_total(li);
        eprintln!("[live] total {name}: blocks={} clean={} corrected={} ({} bytes) uncorrectable={} tei={} cc={} packets={}{}",
            f.blocks, f.clean, f.corrected, f.corrected_bytes, f.uncorrectable, f.tei, f.cc_errors, f.packets,
            if fin.write_failed[li] { " (TS write failed)" } else { "" });
    }
    let m = &fin.remux;
    eprintln!("[live] remux: frames={} packets={} late={} untagged={} forced={} partial_frames={}{}",
        m.frames, m.packets, m.late, m.untagged, m.forced, m.partial_frames, if fin.write_failed[2] { " (TS write failed)" } else { "" });
    eprintln!("[live] done: input={wall_in:.2}s drain={:.2}s signal={signal:.2}s blocks={} startup={} lost={} gaps={} backward={} overflow_chunks={overflow} segments={} cpu_mean={:.2} cpu_max={max_cores:.2} ws_max={}MB",
        wall - wall_in, fin.blocks, fin.startup_blocks, fin.lost_samples, fin.gaps, fin.backward, fin.segments,
        (cpu - cpu0) / wall.max(1e-9), max_ws >> 20);
    if device_error.is_some() { 3 } else { 0 }
}

enum OutMsg {
    Ready,
    Ts(Vec<u8>),
    Error(String),
    Done,
}

/// 再多重化の出力を標準出力のスレッドへ渡す。DriverHost の読み取りが遅れると send で待ち、
/// その遅れは処理系のキューを経て取り込みの欠落として検出される
struct ChannelWriter(std::sync::mpsc::SyncSender<OutMsg>);

impl Write for ChannelWriter {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        self.0.send(OutMsg::Ts(b.to_vec())).map_err(|_| std::io::Error::from(std::io::ErrorKind::BrokenPipe))?;
        Ok(b.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

/// DriverHost_PX4 から起動される受信。標準出力へ hostproto の形式で TS と状態を出し、
/// 標準入力の終わり (または 1 バイト) を停止要求として扱う。終了コード: 0 正常、1 失敗、2 引数の誤り。
fn run_serve(mut it: impl Iterator<Item = String>) -> i32 {
    use msi2500_frontend::hostproto::{retain_valid_packets, write_frame, Kind, MAX_TS};
    use msi2500_frontend::live::{Live, LiveConfig};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::mpsc::{sync_channel, RecvTimeoutError};
    use std::sync::Arc;
    use std::time::Duration;

    let mut a = Args {
        seconds: f64::INFINITY, out: None, freq: 0, gain: 40, bw: 6_000_000, front_end: FrontEnd::Default, index: 0,
        input: None, ts_a: None, ts_b: None, ts_mux: None,
        viterbi_threads: std::thread::available_parallelism().map(|n| n.get().min(4)).unwrap_or(2),
        high_priority: false,
    };
    let mut path: Option<String> = None;
    let parsed: Result<(), String> = (|| {
        while let Some(k) = it.next() {
            let v = it.next().ok_or(format!("{k} needs a value"))?;
            match k.as_str() {
                "--path" => path = Some(v),
                "--index" => a.index = v.parse().map_err(|e| format!("--index: {e}"))?,
                "--frequency" => a.freq = v.parse().map_err(|e| format!("--frequency: {e}"))?,
                "--gain" => a.gain = v.parse().map_err(|e| format!("--gain: {e}"))?,
                "--viterbi-threads" => a.viterbi_threads = v.parse().map_err(|e| format!("--viterbi-threads: {e}"))?,
                x => return Err(format!("unknown argument {x}")),
            }
        }
        if !(470_000_000..=770_000_000).contains(&a.freq) {
            return Err("--frequency must be a UHF channel centre in Hz".into());
        }
        if !(0..=102).contains(&a.gain) {
            return Err("--gain must be 0..102".into());
        }
        Ok(())
    })();
    let mut stdout = std::io::stdout().lock();
    if let Err(e) = parsed {
        eprintln!("[serve] {e}");
        let _ = write_frame(&mut stdout, Kind::Error, e.as_bytes(), 0, 0, 0).and_then(|_| stdout.flush());
        return 2;
    }

    let stop = Arc::new(AtomicBool::new(false));
    {
        let stop = stop.clone();
        std::thread::spawn(move || {
            let mut b = [0u8; 1];
            let _ = std::io::Read::read(&mut std::io::stdin(), &mut b);
            eprintln!("[serve] stop requested via stdin");
            stop.store(true, Ordering::Relaxed);
        });
    }
    let (otx, orx) = sync_channel::<OutMsg>(256);
    let cfg = LiveConfig {
        viterbi_threads: a.viterbi_threads.max(1),
        raw_queue: 256,
        symbol_queue: 512,
        ts_out: [None, None],
        ts_mux: Some(Box::new(ChannelWriter(otx.clone()))),
    };
    let (live, mut feeder) = Live::start(cfg);
    let stats = live.stats.clone();
    let capture_thread = {
        let otx = otx.clone();
        let stop = stop.clone();
        std::thread::Builder::new().name("capture".into()).spawn(move || {
            let setup = tuner_setup(&a);
            let dev = match path {
                Some(p) => Device::open_path(p),
                None => Device::open(&SDRPLAY_INTERFACE_GUID, a.index),
            };
            let dev = match dev {
                Ok(d) => d,
                Err(e) => {
                    let _ = otx.send(OutMsg::Error(format!("open failed: {e}")));
                    live.finish(feeder);
                    return;
                }
            };
            eprintln!("[serve] {} freq={} gain={} viterbi_threads={}", dev.path, a.freq, a.gain, a.viterbi_threads);
            let mut cs = CaptureStats::default();
            let mut ready = false;
            let r = capture(&dev, &setup, &CaptureLimits::default(), &mut cs, |blk, _| {
                if !ready {
                    ready = true;
                    let _ = otx.send(OutMsg::Ready);
                }
                feeder.push(blk);
                !stop.load(Ordering::Relaxed)
            });
            eprintln!("[serve] usb: transfers={} packets={} bad_packets={} odd_packets={} transfer_errors={} last_error=0x{:x} unwritten_blocks={}",
                cs.transfers, cs.packets, cs.bad_packets, cs.odd_packets, cs.transfer_errors, cs.last_error, cs.unwritten_blocks);
            let fin = live.finish(feeder);
            let (fa, fb) = (fin.layer_total(0), fin.layer_total(1));
            eprintln!("[serve] done: blocks={} lost={} gaps={} backward={} segments={} B blk={} unc={} | A blk={} unc={} | remux frames={} packets={} late={} forced={}",
                fin.blocks, fin.lost_samples, fin.gaps, fin.backward, fin.segments, fb.blocks, fb.uncorrectable, fa.blocks, fa.uncorrectable,
                fin.remux.frames, fin.remux.packets, fin.remux.late, fin.remux.forced);
            let _ = otx.send(match r {
                Err(e) if !stop.load(Ordering::Relaxed) => OutMsg::Error(format!("capture stopped: {e}")),
                _ => OutMsg::Done,
            });
        }).expect("spawn capture thread")
    };
    drop(otx);

    // 品質は直近の状態フレームからの B 階層の RS の成功割合 (0〜10000)。RF の C/N ではない
    let mut code = 0;
    let mut out_ok = true;
    let (mut quality, mut last_acc, mut last_rej) = (0u32, 0u64, 0u64);
    let mut last_status = Instant::now();
    let mut last_ts: Option<Instant> = None;
    let mut dropped = 0usize;
    loop {
        let m = orx.recv_timeout(Duration::from_millis(250));
        let mut write = |kind: Kind, payload: &[u8], quality: u32, acc: u64, rej: u64| {
            if out_ok && write_frame(&mut stdout, kind, payload, quality, acc, rej).and_then(|_| stdout.flush()).is_err() {
                // DriverHost が読み取りを止めた。受信を止め、残りを読み捨てて終わる
                out_ok = false;
                stop.store(true, Ordering::Relaxed);
            }
        };
        let (acc, rej) = {
            let s = stats.lock().unwrap();
            let b = s.layer_total(1);
            (b.clean + b.corrected, b.uncorrectable)
        };
        match m {
            Ok(OutMsg::Ready) => write(Kind::Ready, &[], 0, acc, rej),
            Ok(OutMsg::Ts(mut ts)) => {
                dropped += retain_valid_packets(&mut ts);
                // DriverHost は TS のフレームの品質で信号レベルを更新し、BonDriver の利用側は最初の TS の時点で
                // 信号レベルが 0 でないことを期待する。状態フレームの値はまだ 0 の場合があるため、
                // 直近の状態フレーム以降の RS の結果 (なければ累計) から求める
                let (da, dr) = (acc - last_acc.min(acc), rej - last_rej.min(rej));
                let q = if da + dr > 0 {
                    (da * 10_000 / (da + dr)) as u32
                } else if acc + rej > 0 {
                    (acc * 10_000 / (acc + rej)) as u32
                } else {
                    quality
                };
                for chunk in ts.chunks(MAX_TS) {
                    write(Kind::Ts, chunk, q, acc, rej);
                }
                if !ts.is_empty() {
                    last_ts = Some(Instant::now());
                }
            }
            Ok(OutMsg::Error(e)) => {
                eprintln!("[serve] {e}");
                write(Kind::Error, e.as_bytes(), 0, acc, rej);
                code = 1;
                stop.store(true, Ordering::Relaxed);
            }
            Ok(OutMsg::Done) | Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {}
        }
        if last_status.elapsed() >= Duration::from_millis(250) {
            let (da, dr) = (acc - last_acc.min(acc), rej - last_rej.min(rej));
            quality = if last_ts.is_some_and(|t| t.elapsed() < Duration::from_secs(3)) && da + dr > 0 {
                (da * 10_000 / (da + dr)) as u32
            } else {
                0
            };
            (last_acc, last_rej) = (acc, rej);
            write(Kind::Status, &[], quality, acc, rej);
            last_status = Instant::now();
        }
    }
    let _ = capture_thread.join();
    if dropped > 0 {
        eprintln!("[serve] dropped {dropped} packets that DriverHost would reject (TEI or invalid adaptation field)");
    }
    code
}

fn main() {
    let mut args = std::env::args().skip(1);
    let code = match args.next().as_deref() {
        Some("demod") => match args.next() {
            Some(p) => {
                let ts = args.next();
                run_demod(&p, ts.as_deref())
            }
            None => {
                eprintln!("usage: msi2500_frontend demod RAW_BLOCKS_FILE [TS_OUT_B]");
                2
            }
        },
        Some("capture") => match parse(args) {
            Ok(a) => run_capture(a),
            Err(e) => {
                eprintln!("{e}");
                2
            }
        },
        Some("serve") => run_serve(args),
        Some("live") => match parse(args) {
            Ok(a) => run_live(a),
            Err(e) => {
                eprintln!("{e}");
                2
            }
        },
        _ => {
            eprintln!("usage: msi2500_frontend capture [--seconds N] [--out PATH|-] [--freq HZ|--channel N] [--gain DB] [--bw HZ] [--frontend default|sdrplay] [--index N]");
            eprintln!("       msi2500_frontend live [--seconds N] [--freq HZ|--channel N] [--gain DB] [--index N] [--input RAW_BLOCKS_FILE] [--ts PATH] [--ts-a PATH] [--ts-b PATH] [--viterbi-threads N] [--priority normal|high]");
            eprintln!("       msi2500_frontend demod RAW_BLOCKS_FILE [TS_OUT_B]");
            2
        }
    };
    std::process::exit(code);
}
