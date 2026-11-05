//! 取り込んだ生ブロックを、OFDM 復調と A・B 階層の誤り訂正へ別スレッドで流す受信の処理系。
//!
//! スレッドの構成:
//!   取り込み (呼び出し側) → [生ブロックのキュー] → OFDM 復調 → [シンボルのキュー] → A 階層の誤り訂正
//!                                                                → [シンボルのキュー] → B 階層の誤り訂正 (+ ビタビの作業スレッド)
//!
//! 取り込みのスレッドは等時転送の完了を待つため、キューが満杯でも待たずにそのチャンクを捨てる。
//! 捨てたチャンクはブロックのサンプル番号の欠落として OFDM 側で検出され、通常の欠落と同じく
//! 復調と誤り訂正の状態をすべて作り直す。OFDM から誤り訂正へは待つ形で渡し、誤り訂正の遅れを
//! 生ブロックのキューへ伝える。

use crate::blocks::{BlockCounter, BlockState};
use crate::fecstream::{FecStats, LayerPipe, TspTag};
use crate::ofdm::{self, Ofdm, SymbolOut, Tmcc};
use crate::raw384::decode_block;
use crate::remux::{RemuxStats, Remuxer};
use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

/// 生ブロックのチャンクの大きさ (256 ブロック = 98,304 サンプル、約 12 ms)
pub const CHUNK_BLOCKS: usize = 256;
const BLOCK: usize = 1024;

pub struct LiveConfig {
    /// B 階層のビタビのスレッド数
    pub viterbi_threads: usize,
    /// 生ブロックのキューの長さ (チャンク数、1 チャンク約 12 ms)。PX 系チューナーの録画と同時に動かした 600 秒の受信で、
    /// 64 (約 0.77 秒分) があふれる処理の停滞が 2 回あったため、呼び出し側は 256 (約 3.1 秒分、最大 64 MB) を使う
    pub raw_queue: usize,
    /// シンボルのキューの長さ。512 で約 0.58 秒分
    pub symbol_queue: usize,
    /// 階層ごとの TS の出力先 (A, B)
    pub ts_out: [Option<Box<dyn Write + Send>>; 2],
    /// A・B 階層を多重フレームの位置に従って再多重した TS の出力先
    pub ts_mux: Option<Box<dyn Write + Send>>,
}

#[derive(Clone, Debug, Default)]
pub struct LiveStats {
    /// ブロックの連続性 (OFDM スレッドで計数)
    pub blocks: u64,
    pub startup_blocks: u64,
    pub lost_samples: u64,
    pub gaps: u64,
    pub backward: u64,
    /// 不連続で状態を作り直した回数 + 1
    pub segments: u64,
    pub symbols: u64,
    pub ofdm: ofdm::Stats,
    /// 現在の区間の誤り訂正の統計 (A, B)
    pub layers: [FecStats; 2],
    /// 終わった区間の統計の合計 (A, B)
    pub layers_done: [FecStats; 2],
    /// TS の書き出しに失敗した出力 (A, B, 再多重)
    pub write_failed: [bool; 3],
    /// 再多重化の統計 (全区間の合計)
    pub remux: RemuxStats,
}

impl LiveStats {
    /// 終わった区間と現在の区間を合わせた統計
    pub fn layer_total(&self, li: usize) -> FecStats {
        let mut t = self.layers_done[li].clone();
        add_stats(&mut t, &self.layers[li]);
        t
    }
}

pub fn add_stats(acc: &mut FecStats, s: &FecStats) {
    acc.symbols += s.symbols;
    acc.acquired |= s.acquired;
    acc.acq_attempts += s.acq_attempts;
    acc.blocks += s.blocks;
    acc.clean += s.clean;
    acc.corrected += s.corrected;
    acc.corrected_bytes += s.corrected_bytes;
    acc.uncorrectable += s.uncorrectable;
    acc.tei += s.tei;
    acc.cc_errors += s.cc_errors;
    acc.packets += s.packets;
}

/// 取り込みのスレッドで使う投入口。push は待たない。
pub struct Feeder {
    tx: SyncSender<Vec<u8>>,
    buf: Vec<u8>,
    overflow: Arc<AtomicU64>,
}

impl Feeder {
    pub fn push(&mut self, blk: &[u8]) {
        self.buf.extend_from_slice(blk);
        if self.buf.len() >= CHUNK_BLOCKS * BLOCK {
            let chunk = std::mem::replace(&mut self.buf, Vec::with_capacity(CHUNK_BLOCKS * BLOCK));
            match self.tx.try_send(chunk) {
                Ok(()) => {}
                Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => {
                    self.overflow.fetch_add(1, Ordering::Relaxed);
                }
            }
        }
    }

    /// 録画ファイルからの投入用。キューが空くまで待つため、チャンクを捨てない。
    pub fn push_wait(&mut self, blk: &[u8]) {
        self.buf.extend_from_slice(blk);
        if self.buf.len() >= CHUNK_BLOCKS * BLOCK {
            let chunk = std::mem::replace(&mut self.buf, Vec::with_capacity(CHUNK_BLOCKS * BLOCK));
            let _ = self.tx.send(chunk);
        }
    }

    fn flush_wait(&mut self) {
        if !self.buf.is_empty() {
            let _ = self.tx.send(std::mem::take(&mut self.buf));
        }
    }
}

enum FecMsg {
    Sym(Arc<SymbolOut>),
    Reset,
}

/// 誤り訂正から再多重化へ渡す 1 回分のパケット列
struct RemuxMsg {
    /// 不連続で状態を作り直した回数 (両階層で同じ数え方)
    segment: u64,
    layer: usize,
    tmcc: Tmcc,
    tags: Vec<TspTag>,
    data: Vec<u8>,
}

pub struct Live {
    overflow: Arc<AtomicU64>,
    pub stats: Arc<Mutex<LiveStats>>,
    ofdm_thread: Option<JoinHandle<()>>,
    fec_threads: Vec<JoinHandle<()>>,
}

impl Live {
    pub fn start(mut cfg: LiveConfig) -> (Live, Feeder) {
        let (raw_tx, raw_rx) = sync_channel::<Vec<u8>>(cfg.raw_queue);
        let stats = Arc::new(Mutex::new(LiveStats { segments: 1, ..Default::default() }));
        let overflow = Arc::new(AtomicU64::new(0));
        let mut fec_tx = Vec::new();
        let mut fec_threads = Vec::new();
        // 再多重化は A 階層を約 1 フレーム (2,592 パケット) 待つため、その間の B 階層の出力を受けられる長さにする
        let (mux_tx, mux_rx) = sync_channel::<RemuxMsg>(1024);
        for li in 0..2 {
            let (tx, rx) = sync_channel::<FecMsg>(cfg.symbol_queue);
            fec_tx.push(tx);
            let out = cfg.ts_out[li].take();
            let st = stats.clone();
            let vt = if li == 1 { cfg.viterbi_threads } else { 1 };
            let mux = mux_tx.clone();
            fec_threads.push(
                std::thread::Builder::new()
                    .name(format!("fec-{}", ["a", "b"][li]))
                    .spawn(move || fec_loop(li, vt, rx, out, mux, st))
                    .expect("spawn fec thread"),
            );
        }
        drop(mux_tx);
        let mux_out = cfg.ts_mux.take();
        let st = stats.clone();
        fec_threads.push(
            std::thread::Builder::new()
                .name("remux".into())
                .spawn(move || remux_loop(mux_rx, mux_out, st))
                .expect("spawn remux thread"),
        );
        let st = stats.clone();
        let ofdm_thread = std::thread::Builder::new()
            .name("ofdm".into())
            .spawn(move || ofdm_loop(raw_rx, fec_tx, st))
            .expect("spawn ofdm thread");
        let feeder = Feeder { tx: raw_tx, buf: Vec::with_capacity(CHUNK_BLOCKS * BLOCK), overflow: overflow.clone() };
        (Live { overflow, stats, ofdm_thread: Some(ofdm_thread), fec_threads }, feeder)
    }

    /// キューが満杯で捨てたチャンクの数
    pub fn overflow_chunks(&self) -> u64 {
        self.overflow.load(Ordering::Relaxed)
    }

    /// 端数のチャンクを送って投入口を閉じ、キューに残った分を処理し終えるまで待って最終の統計を返す。
    pub fn finish(mut self, mut feeder: Feeder) -> LiveStats {
        feeder.flush_wait();
        drop(feeder);
        if let Some(h) = self.ofdm_thread.take() {
            let _ = h.join();
        }
        for h in self.fec_threads.drain(..) {
            let _ = h.join();
        }
        let s = self.stats.lock().unwrap().clone();
        s
    }
}

fn ofdm_loop(rx: Receiver<Vec<u8>>, fec_tx: Vec<SyncSender<FecMsg>>, stats: Arc<Mutex<LiveStats>>) {
    let mut counter = BlockCounter::new(384);
    let mut ofdm = Ofdm::new();
    let mut samples = Vec::with_capacity(CHUNK_BLOCKS * 384);
    let mut symbols = 0u64;
    let mut segments = 1u64;
    // 誤り訂正のスレッドが終わっていたら、それ以上は送らずに終える
    let send_sym = |s: Arc<SymbolOut>| fec_tx.iter().all(|t| t.send(FecMsg::Sym(s.clone())).is_ok());
    let send_reset = || fec_tx.iter().all(|t| t.send(FecMsg::Reset).is_ok());
    let run = |samples: &mut Vec<_>, ofdm: &mut Ofdm, symbols: &mut u64| -> bool {
        if samples.is_empty() {
            return true;
        }
        let out = ofdm.push(samples);
        samples.clear();
        for s in out {
            *symbols += 1;
            if !send_sym(Arc::new(s)) {
                return false;
            }
        }
        true
    };
    'outer: for chunk in rx {
        for blk in chunk.chunks_exact(BLOCK) {
            let state = counter.push(blk);
            if matches!(state, BlockState::Gap(_) | BlockState::Backward) && counter.gaps + counter.backward <= 12 {
                // 不連続の原因 (欠落か、ブロックの区切りのずれか) を見分けるため、最初の数回は番号を記録する
                let addr = u32::from_le_bytes([blk[0], blk[1], blk[2], blk[3]]);
                eprintln!("[live] discontinuity {:?} at block {} counter=0x{addr:08x} header={:02x?}", state, counter.blocks, &blk[..16]);
            }
            match state {
                BlockState::Startup => continue,
                BlockState::Gap(_) | BlockState::Backward => {
                    // 不連続の前までを処理してから、前後をつながずに状態をすべて作り直す。
                    // 不連続を検出したブロックは新しい区間の先頭になる
                    if !run(&mut samples, &mut ofdm, &mut symbols) {
                        break 'outer;
                    }
                    ofdm = Ofdm::new();
                    segments += 1;
                    if !send_reset() {
                        break 'outer;
                    }
                }
                BlockState::Ok => {}
            }
            decode_block(blk, &mut samples);
        }
        if !run(&mut samples, &mut ofdm, &mut symbols) {
            break;
        }
        let mut s = stats.lock().unwrap();
        s.blocks = counter.blocks;
        s.startup_blocks = counter.startup_blocks;
        s.lost_samples = counter.lost_samples;
        s.gaps = counter.gaps;
        s.backward = counter.backward;
        s.segments = segments;
        s.symbols = symbols;
        s.ofdm = ofdm.stats.clone();
    }
    // ここで送信側が閉じ、誤り訂正のスレッドはキューの残りを処理して終わる
}

fn fec_loop(
    li: usize,
    threads: usize,
    rx: Receiver<FecMsg>,
    mut out: Option<Box<dyn Write + Send>>,
    mux: SyncSender<RemuxMsg>,
    stats: Arc<Mutex<LiveStats>>,
) {
    let mut pipe: Option<LayerPipe> = None;
    let mut ts = Vec::new();
    let mut n = 0u64;
    let mut segment = 0u64;
    for m in rx {
        match m {
            FecMsg::Sym(s) => {
                if pipe.is_none() {
                    if let Some(t) = s.tmcc {
                        // 部分受信の A 階層が先頭のセグメントを使い、B 階層が続くセグメントを使う
                        let na = t.layers[0].nseg as usize;
                        let nb = t.layers[1].nseg as usize;
                        let segs: Vec<usize> = if li == 0 { (0..na).collect() } else { (na..na + nb).collect() };
                        if !segs.is_empty() {
                            pipe = Some(LayerPipe::new(&t.layers[li], &segs, threads));
                        }
                    }
                }
                if let Some(p) = pipe.as_mut() {
                    p.push(&s, &mut ts);
                    if !ts.is_empty() {
                        if let Some(w) = out.as_mut() {
                            if w.write_all(&ts).is_err() {
                                out = None;
                                stats.lock().unwrap().write_failed[li] = true;
                            }
                        }
                        if let Some(tmcc) = s.tmcc {
                            let msg = RemuxMsg { segment, layer: li, tmcc, tags: p.tags.clone(), data: std::mem::take(&mut ts) };
                            // 再多重化のスレッドが終わっていても受信は続ける
                            let _ = mux.send(msg);
                        }
                        ts.clear();
                    }
                    n += 1;
                    if n % 64 == 0 {
                        stats.lock().unwrap().layers[li] = p.stats.clone();
                    }
                }
            }
            FecMsg::Reset => {
                segment += 1;
                let mut s = stats.lock().unwrap();
                if let Some(p) = pipe.take() {
                    add_stats(&mut s.layers_done[li], &p.stats);
                }
                s.layers[li] = FecStats::default();
            }
        }
    }
    if let Some(w) = out.as_mut() {
        let _ = w.flush();
    }
    if let Some(p) = pipe {
        stats.lock().unwrap().layers[li] = p.stats;
    }
}

fn add_remux(acc: &mut RemuxStats, s: &RemuxStats) {
    acc.frames += s.frames;
    acc.packets += s.packets;
    acc.late += s.late;
    acc.untagged += s.untagged;
    acc.forced += s.forced;
    acc.partial_frames += s.partial_frames;
}

fn remux_loop(rx: Receiver<RemuxMsg>, mut out: Option<Box<dyn Write + Send>>, stats: Arc<Mutex<LiveStats>>) {
    let mut cur: Option<(u64, Remuxer)> = None;
    let mut done = RemuxStats::default();
    let mut buf = Vec::new();
    let write = |buf: &mut Vec<u8>, out: &mut Option<Box<dyn Write + Send>>| {
        if let Some(w) = out.as_mut() {
            if !buf.is_empty() && w.write_all(buf).is_err() {
                *out = None;
                stats.lock().unwrap().write_failed[2] = true;
            }
        }
        buf.clear();
    };
    let mut n = 0u64;
    for m in rx {
        match &cur {
            Some((s, _)) if *s == m.segment => {}
            Some((s, _)) if *s > m.segment => {
                // 前の区間の残り。新しい区間を出し始めた後のため捨てる
                done.late += m.tags.len() as u64;
                continue;
            }
            _ => {
                // 新しい区間。前の区間を出し切ってから、その区間の TMCC で作り直す
                if let Some((_, mut r)) = cur.take() {
                    r.finish(&mut buf);
                    add_remux(&mut done, &r.stats);
                }
                cur = Some((m.segment, Remuxer::new(&m.tmcc)));
            }
        }
        let r = &mut cur.as_mut().unwrap().1;
        r.push(m.layer, &m.tags, &m.data, &mut buf);
        write(&mut buf, &mut out);
        n += 1;
        if n % 64 == 0 {
            let mut t = done.clone();
            add_remux(&mut t, &r.stats);
            stats.lock().unwrap().remux = t;
        }
    }
    if let Some((_, mut r)) = cur.take() {
        r.finish(&mut buf);
        add_remux(&mut done, &r.stats);
    }
    write(&mut buf, &mut out);
    if let Some(w) = out.as_mut() {
        let _ = w.flush();
    }
    stats.lock().unwrap().remux = done;
}

/// プロセスの CPU 時間 (ユーザー + カーネル、秒)、ワーキングセット、コミット済みのプライベートメモリ (バイト)
pub fn process_usage() -> (f64, usize, usize) {
    use windows_sys::Win32::Foundation::FILETIME;
    use windows_sys::Win32::System::ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS};
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
    let ft = || FILETIME { dwLowDateTime: 0, dwHighDateTime: 0 };
    let (mut c, mut e, mut k, mut u) = (ft(), ft(), ft(), ft());
    let secs = |f: &FILETIME| (((f.dwHighDateTime as u64) << 32) | f.dwLowDateTime as u64) as f64 * 1e-7;
    let mut pmc: PROCESS_MEMORY_COUNTERS = unsafe { std::mem::zeroed() };
    pmc.cb = std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32;
    unsafe {
        let h = GetCurrentProcess();
        GetProcessTimes(h, &mut c, &mut e, &mut k, &mut u);
        K32GetProcessMemoryInfo(h, &mut pmc, pmc.cb);
    }
    (secs(&k) + secs(&u), pmc.WorkingSetSize, pmc.PagefileUsage)
}
