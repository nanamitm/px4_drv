//! J.83 Annex B の受信の処理系。地上デジタル放送の live.rs と同じく、取り込みのスレッドを待たせない。
//!
//! スレッドの構成:
//!   取り込み (呼び出し側) → [生ブロックのキュー] → QAM 復調 → [シンボルのキュー] → 誤り訂正 → TS
//!
//! 取り込みのスレッドはキューが満杯でもチャンクを捨てて進み、捨てたチャンクはブロックのサンプル番号の欠落として
//! 復調側で検出される。欠落の前後はつながないため、復調と誤り訂正の状態をすべて作り直す。

use crate::blocks::{BlockCounter, BlockState};
use crate::j83b::{J83bDecoder, J83bStats};
use crate::live::{Feeder, CHUNK_BLOCKS};
use crate::qam::{QamDemod, QamStats};
use crate::raw384::decode_block;
use crate::s1ud::{S1udFrontend, BLOCK_BYTES as S1UD_BLOCK_BYTES, BLOCK_SAMPLES as S1UD_BLOCK_SAMPLES};
use num_complex::Complex32;
use std::collections::HashMap;
use std::io::Write;
use std::sync::atomic::AtomicU64;
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Instant;

const BLOCK: usize = 1024;
/// MSi2500 の 8,126,984 S/s
const FS: f64 = 512e6 / 63.0;
/// PX-S1UD (SMS2270) の IQ の 12 MS/s
const S1UD_FS: f64 = 12e6;

pub struct J83bLiveConfig {
    /// 生ブロックのキューの長さ (チャンク数、1 チャンク約 12 ms)。live.rs の LiveConfig と同じ理由で 256 を使う
    pub raw_queue: usize,
    /// シンボルのキューの長さ (1 チャンク約 12 ms 分のシンボル)
    pub symbol_queue: usize,
    /// 判定帰還 PLL の帯域の倍率 (1 が既定)
    pub pll_scale: f32,
    pub ts_out: Option<Box<dyn Write + Send>>,
}

#[derive(Clone, Debug, Default)]
pub struct J83bLiveStats {
    pub blocks: u64,
    pub startup_blocks: u64,
    pub lost_samples: u64,
    pub gaps: u64,
    pub backward: u64,
    /// 不連続で状態を作り直した回数 + 1
    pub segments: u64,
    /// 復調に入れたサンプル数 (全区間)
    pub samples: u64,
    /// I か Q が ADC の最大値に達したサンプル数。利得が大きすぎると MER はほとんど下がらないまま
    /// RS の訂正が急に増えたため (検証環境の H008、利得 58)、利得の過大を見分ける目安にする
    pub clipped: u64,
    /// 現在の区間の復調の統計
    pub qam: QamStats,
    /// 現在の区間の誤り訂正の統計と、終わった区間の合計
    pub fec: J83bStats,
    pub fec_done: J83bStats,
    /// 出力した TS の検査 (全区間)
    pub ts_packets: u64,
    pub tei: u64,
    pub cc_errors: u64,
    /// 処理系の開始から最初の TS を出すまでの秒数
    pub first_ts_s: Option<f64>,
    pub write_failed: bool,
    /// samples のサンプルレート。0 は MSi2500 の 8,126,984 S/s
    pub sample_rate: f64,
}

impl J83bLiveStats {
    /// 終わった区間と現在の区間を合わせた誤り訂正の統計
    pub fn fec_total(&self) -> J83bStats {
        let mut t = self.fec_done.clone();
        add_fec(&mut t, &self.fec);
        t
    }
    /// 信号の秒数 (全区間)
    pub fn signal_s(&self) -> f64 {
        self.samples as f64 / if self.sample_rate > 0.0 { self.sample_rate } else { FS }
    }
}

pub fn add_fec(acc: &mut J83bStats, s: &J83bStats) {
    acc.symbols += s.symbols;
    acc.acquisitions += s.acquisitions;
    acc.frames += s.frames;
    acc.frame_sync_losses += s.frame_sync_losses;
    acc.rs_blocks += s.rs_blocks;
    acc.rs_clean += s.rs_clean;
    acc.rs_corrected += s.rs_corrected;
    acc.rs_corrected_symbols += s.rs_corrected_symbols;
    acc.rs_uncorrectable += s.rs_uncorrectable;
    acc.packets += s.packets;
    acc.checksum_errors += s.checksum_errors;
    acc.packet_sync_losses += s.packet_sync_losses;
    acc.orientation_flips += s.orientation_flips;
    acc.control_word_rejects += s.control_word_rejects;
    acc.conj = s.conj;
    if s.control_word.is_some() {
        acc.control_word = s.control_word;
    }
}

enum SymMsg {
    Syms(Vec<Complex32>),
    Reset,
}

pub struct J83bLive {
    overflow: Arc<AtomicU64>,
    pub stats: Arc<Mutex<J83bLiveStats>>,
    threads: Vec<JoinHandle<()>>,
}

impl J83bLive {
    pub fn start(mut cfg: J83bLiveConfig) -> (J83bLive, Feeder) {
        let (raw_tx, raw_rx) = sync_channel::<Vec<u8>>(cfg.raw_queue);
        let (sym_tx, sym_rx) = sync_channel::<SymMsg>(cfg.symbol_queue);
        let stats = Arc::new(Mutex::new(J83bLiveStats { segments: 1, ..Default::default() }));
        let overflow = Arc::new(AtomicU64::new(0));
        let t0 = Instant::now();
        let out = cfg.ts_out.take();
        let st = stats.clone();
        let fec = std::thread::Builder::new()
            .name("j83b-fec".into())
            .spawn(move || fec_loop(sym_rx, out, st, t0))
            .expect("spawn fec thread");
        let st = stats.clone();
        let pll = cfg.pll_scale;
        let qam = std::thread::Builder::new()
            .name("j83b-qam".into())
            .spawn(move || qam_loop(raw_rx, sym_tx, st, pll))
            .expect("spawn qam thread");
        let feeder = Feeder::new(raw_tx, overflow.clone());
        (J83bLive { overflow, stats, threads: vec![qam, fec] }, feeder)
    }

    /// キューが満杯で捨てたチャンクの数
    pub fn overflow_chunks(&self) -> u64 {
        self.overflow.load(std::sync::atomic::Ordering::Relaxed)
    }

    /// PX-S1UD の IQ (s1ud.rs のブロック 14724 バイトを並べたチャンク) を受ける処理系。
    /// 投入口はチャンクの送信側で、閉じると残りを処理して終わる (join で待つ)
    pub fn start_s1ud(mut cfg: J83bLiveConfig) -> (J83bLive, SyncSender<Vec<u8>>) {
        let (raw_tx, raw_rx) = sync_channel::<Vec<u8>>(cfg.raw_queue);
        let (sym_tx, sym_rx) = sync_channel::<SymMsg>(cfg.symbol_queue);
        let stats = Arc::new(Mutex::new(J83bLiveStats { segments: 1, sample_rate: S1UD_FS, ..Default::default() }));
        let t0 = Instant::now();
        let out = cfg.ts_out.take();
        let st = stats.clone();
        let fec = std::thread::Builder::new()
            .name("j83b-fec".into())
            .spawn(move || fec_loop(sym_rx, out, st, t0))
            .expect("spawn fec thread");
        let st = stats.clone();
        let pll = cfg.pll_scale;
        let qam = std::thread::Builder::new()
            .name("s1ud-qam".into())
            .spawn(move || s1ud_loop(raw_rx, sym_tx, st, pll))
            .expect("spawn qam thread");
        (J83bLive { overflow: Arc::new(AtomicU64::new(0)), stats, threads: vec![qam, fec] }, raw_tx)
    }

    /// 投入口を閉じた後に、キューに残った分を処理し終えるまで待って最終の統計を返す
    pub fn join(mut self) -> J83bLiveStats {
        for h in self.threads.drain(..) {
            let _ = h.join();
        }
        let s = self.stats.lock().unwrap().clone();
        s
    }

    /// 端数のチャンクを送って投入口を閉じ、キューに残った分を処理し終えるまで待って最終の統計を返す。
    pub fn finish(mut self, mut feeder: Feeder) -> J83bLiveStats {
        feeder.flush_wait();
        drop(feeder);
        for h in self.threads.drain(..) {
            let _ = h.join();
        }
        let s = self.stats.lock().unwrap().clone();
        s
    }
}

fn qam_loop(rx: Receiver<Vec<u8>>, tx: SyncSender<SymMsg>, stats: Arc<Mutex<J83bLiveStats>>, pll: f32) {
    let mut counter = BlockCounter::new(384);
    let mut demod = QamDemod::new(FS, pll);
    let mut samples = Vec::with_capacity(CHUNK_BLOCKS * 384);
    let mut segments = 1u64;
    let mut total_samples = 0u64;
    let mut clipped = 0u64;
    const CLIP: f32 = 0.99;
    // 処理済みのサンプルを復調へ渡し、得たシンボルを送る。誤り訂正のスレッドが終わっていれば false
    let run = |samples: &mut Vec<Complex32>, demod: &mut QamDemod, total: &mut u64| -> bool {
        if samples.is_empty() {
            return true;
        }
        let mut syms = Vec::with_capacity(samples.len() * 3 / 4);
        demod.push(samples, &mut syms);
        *total += samples.len() as u64;
        samples.clear();
        syms.is_empty() || tx.send(SymMsg::Syms(syms)).is_ok()
    };
    'outer: for chunk in rx {
        for blk in chunk.chunks_exact(BLOCK) {
            let state = counter.push(blk);
            if matches!(state, BlockState::Gap(_) | BlockState::Backward) && counter.gaps + counter.backward <= 12 {
                let addr = u32::from_le_bytes([blk[0], blk[1], blk[2], blk[3]]);
                eprintln!("[j83b] discontinuity {:?} at block {} counter=0x{addr:08x}", state, counter.blocks);
            }
            match state {
                BlockState::Startup => continue,
                BlockState::Gap(_) | BlockState::Backward => {
                    // 不連続の前までを処理してから、前後をつながずに状態をすべて作り直す
                    if !run(&mut samples, &mut demod, &mut total_samples) {
                        break 'outer;
                    }
                    demod.reset();
                    segments += 1;
                    if tx.send(SymMsg::Reset).is_err() {
                        break 'outer;
                    }
                }
                BlockState::Ok => {}
            }
            let start = samples.len();
            decode_block(blk, &mut samples);
            // 10 ビットの最大値 511 は ±1 に正規化した値で 0.998
            clipped += samples[start..].iter().filter(|z| z.re.abs() >= CLIP || z.im.abs() >= CLIP).count() as u64;
        }
        if !run(&mut samples, &mut demod, &mut total_samples) {
            break;
        }
        let mut s = stats.lock().unwrap();
        s.blocks = counter.blocks;
        s.startup_blocks = counter.startup_blocks;
        s.lost_samples = counter.lost_samples;
        s.gaps = counter.gaps;
        s.backward = counter.backward;
        s.segments = segments;
        s.samples = total_samples;
        s.clipped = clipped;
        s.qam = demod.stats.clone();
    }
}

/// PX-S1UD のブロックを S1udFrontend で復調する。チップやキューが落としたブロックはブロック番号から
/// S1udFrontend が検出し、8 ブロックまではつなぎ、それより長ければ作り直す (作り直したら誤り訂正へも伝える)
fn s1ud_loop(rx: Receiver<Vec<u8>>, tx: SyncSender<SymMsg>, stats: Arc<Mutex<J83bLiveStats>>, pll: f32) {
    let mut front = S1udFrontend::new(S1UD_FS, pll);
    let mut syms = Vec::new();
    'outer: for chunk in rx {
        for blk in chunk.chunks_exact(S1UD_BLOCK_BYTES) {
            syms.clear();
            if front.push_block(blk, &mut syms) && tx.send(SymMsg::Reset).is_err() {
                break 'outer;
            }
            if !syms.is_empty() && tx.send(SymMsg::Syms(std::mem::take(&mut syms))).is_err() {
                break 'outer;
            }
        }
        let mut s = stats.lock().unwrap();
        s.blocks = front.stats.blocks;
        s.lost_samples = front.stats.lost_blocks * S1UD_BLOCK_SAMPLES as u64;
        s.gaps = front.stats.lost_blocks;
        s.segments = front.stats.resets + 1;
        // 復調器は作り直すたびに数え直すため、受けたブロック数から全区間のサンプル数を求める
        s.samples = front.stats.blocks * S1UD_BLOCK_SAMPLES as u64;
        s.qam = front.demod_stats().clone();
    }
}

/// 出力する TS の TEI と continuity counter の検査。null パケットとペイロードのないパケットは数えない
#[derive(Default)]
struct TsCheck {
    last_cc: HashMap<u16, u8>,
    packets: u64,
    tei: u64,
    cc_errors: u64,
}

impl TsCheck {
    fn push(&mut self, ts: &[u8]) {
        for p in ts.chunks_exact(188) {
            self.packets += 1;
            if p[1] & 0x80 != 0 {
                self.tei += 1;
                continue;
            }
            let pid = (((p[1] & 0x1f) as u16) << 8) | p[2] as u16;
            let afc = (p[3] >> 4) & 3;
            let cc = p[3] & 0x0f;
            if pid == 0x1fff || afc & 1 == 0 {
                continue;
            }
            // adaptation field の discontinuity_indicator が立っていれば数え直す
            let disc = afc & 2 != 0 && p[4] > 0 && p[5] & 0x80 != 0;
            if let Some(&prev) = self.last_cc.get(&pid) {
                if !disc && cc != (prev + 1) & 0x0f && cc != prev {
                    self.cc_errors += 1;
                }
            }
            self.last_cc.insert(pid, cc);
        }
    }
}

fn fec_loop(rx: Receiver<SymMsg>, mut out: Option<Box<dyn Write + Send>>, stats: Arc<Mutex<J83bLiveStats>>, t0: Instant) {
    let mut dec = J83bDecoder::new();
    let mut check = TsCheck::default();
    let mut ts = Vec::new();
    let mut n = 0u64;
    for m in rx {
        match m {
            SymMsg::Syms(syms) => {
                dec.push(&syms, &mut ts);
                if !ts.is_empty() {
                    check.push(&ts);
                    {
                        // serve は TS のフレームの品質 (DriverHost の信号レベル) を RS の統計から求め、BonDriver の利用側は
                        // 最初の TS の時点で信号レベルが 0 でないことを期待する。統計を TS より先に反映し、最初の TS で RS の結果が見えるようにする
                        let mut s = stats.lock().unwrap();
                        s.fec = dec.stats.clone();
                        (s.ts_packets, s.tei, s.cc_errors) = (check.packets, check.tei, check.cc_errors);
                        if s.first_ts_s.is_none() {
                            s.first_ts_s = Some(t0.elapsed().as_secs_f64());
                        }
                    }
                    if let Some(w) = out.as_mut() {
                        if w.write_all(&ts).is_err() {
                            out = None;
                            stats.lock().unwrap().write_failed = true;
                        }
                    }
                    ts.clear();
                }
                n += 1;
                if n % 16 == 0 {
                    let mut s = stats.lock().unwrap();
                    s.fec = dec.stats.clone();
                    (s.ts_packets, s.tei, s.cc_errors) = (check.packets, check.tei, check.cc_errors);
                }
            }
            SymMsg::Reset => {
                // 区間をまたいで continuity counter を比べないよう、検査の履歴も捨てる
                check.last_cc.clear();
                let mut s = stats.lock().unwrap();
                add_fec(&mut s.fec_done, &dec.stats);
                s.fec = J83bStats::default();
                dec = J83bDecoder::new();
            }
        }
    }
    if let Some(w) = out.as_mut() {
        let _ = w.flush();
    }
    let mut s = stats.lock().unwrap();
    s.fec = dec.stats;
    (s.ts_packets, s.tei, s.cc_errors) = (check.packets, check.tei, check.cc_errors);
}
