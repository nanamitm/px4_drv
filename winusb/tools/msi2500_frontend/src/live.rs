//! 取り込んだ生ブロックを、OFDM 復調と A・B 階層の誤り訂正へ別スレッドで流す受信の処理系。
//!
//! スレッドの構成:
//!   取り込み (呼び出し側) → [生ブロックのキュー] → OFDM 復調 → [シンボルのキュー] → A 階層の誤り訂正
//!                                                                → [シンボルのキュー] → B 階層の誤り訂正の前段
//!                                                                  → ビタビ (+ 作業スレッド) → 後段 (バイトデインターリーブと RS)
//!
//! 取り込みのスレッドは等時転送の完了を待つため、キューが満杯でも待たずにそのチャンクを捨てる。
//! 捨てたチャンクはブロックのサンプル番号の欠落として OFDM 側で検出され、通常の欠落と同じく
//! 復調と誤り訂正の状態をすべて作り直す。OFDM から誤り訂正へは待つ形で渡し、誤り訂正の遅れを
//! 生ブロックのキューへ伝える。

use crate::blocks::{BlockCounter, BlockState};
use crate::fecstream::{stages, FecStats, LayerPipe, PostStage, PreStage, TspTag, VitStage};
use crate::ofdm::{self, Ofdm, SymbolOut, Tmcc};
use crate::raw384::decode_block;
use crate::remux::{RemuxStats, Remuxer};
use std::io::Write;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

/// 生ブロックのチャンクの大きさ (256 ブロック = 98,304 サンプル、約 12 ms)
pub const CHUNK_BLOCKS: usize = 256;
const BLOCK: usize = 1024;
/// 状態を作り直さずに埋める欠落の上限 (サンプル数、256 シンボル = 約 290 ms)。これより長い欠落と番号の巻き戻りは作り直す。
/// 録画 IQ (13ch、B 階層 64QAM 3/4、MER 約 20.7 dB) から欠落を 3 回抜いた試験では、
/// 18 ms までは TS が欠落のない場合と一致し、36 ms で 1 回あたり約 140 パケット、290 ms で約 5,000 パケットを失った。
/// 290 ms でも同期は保たれ、作り直し (約 2 秒、B 階層だけで約 2 万パケット) より損失が小さい。
/// それより長い欠落は試していないため作り直す
pub const MAX_BRIDGE_SAMPLES: u64 = 256 * crate::ofdm::L as u64;

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
    /// 状態を作り直さずに埋めた欠落の回数 (gaps に含まれる)
    pub bridged: u64,
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
    /// 別の処理系 (J.83B) が同じ投入口を使うための生成
    pub(crate) fn new(tx: SyncSender<Vec<u8>>, overflow: Arc<AtomicU64>) -> Self {
        Self { tx, buf: Vec::with_capacity(CHUNK_BLOCKS * BLOCK), overflow }
    }

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

    pub(crate) fn flush_wait(&mut self) {
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
    pub fn start(cfg: LiveConfig) -> (Live, Feeder) {
        Self::start_with_reset(cfg, None)
    }

    pub fn start_with_reset(mut cfg: LiveConfig, reset: Option<Arc<AtomicBool>>) -> (Live, Feeder) {
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
            let sink = LayerSink { li, out, mux: mux_tx.clone(), stats: st, ts: Vec::new(), n: 0, segment: 0 };
            fec_threads.push(
                std::thread::Builder::new()
                    .name(format!("fec-{}", ["a", "b"][li]))
                    .spawn(move || if li == 1 { fec_loop_staged(li, vt, rx, sink) } else { fec_loop(li, vt, rx, sink) })
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
            .spawn(move || ofdm_loop(raw_rx, fec_tx, st, reset))
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

fn ofdm_loop(
    rx: Receiver<Vec<u8>>,
    fec_tx: Vec<SyncSender<FecMsg>>,
    stats: Arc<Mutex<LiveStats>>,
    reset: Option<Arc<AtomicBool>>,
) {
    let mut counter = BlockCounter::new(384);
    let mut ofdm = Ofdm::new();
    let mut samples = Vec::with_capacity(CHUNK_BLOCKS * 384);
    let mut symbols = 0u64;
    let mut segments = 1u64;
    let mut bridged = 0u64;
    // 作り直す前の区間で消失として出したシンボルの数
    let mut erased_done = 0u64;
    // 誤り訂正のスレッドが終わっていたら、それ以上は送らずに終える
    let send_sym = |s: Arc<SymbolOut>| fec_tx.iter().all(|t| t.send(FecMsg::Sym(s.clone())).is_ok());
    let restart = |ofdm: &mut Ofdm, segments: &mut u64, erased_done: &mut u64| {
        *erased_done += ofdm.stats.erased_symbols;
        *ofdm = Ofdm::new();
        *segments += 1;
        fec_tx.iter().all(|t| t.send(FecMsg::Reset).is_ok())
    };
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
        if reset.as_ref().is_some_and(|flag| flag.swap(false, Ordering::Relaxed)) {
            eprintln!("[live] no valid TS; resetting OFDM and FEC");
            if !restart(&mut ofdm, &mut segments, &mut erased_done) {
                break;
            }
        }
        for blk in chunk.chunks_exact(BLOCK) {
            let state = counter.push(blk);
            if matches!(state, BlockState::Gap(_) | BlockState::Backward) && counter.gaps + counter.backward <= 12 {
                // 不連続の原因 (欠落か、ブロックの区切りのずれか) を見分けるため、最初の数回は番号を記録する
                let addr = u32::from_le_bytes([blk[0], blk[1], blk[2], blk[3]]);
                eprintln!("[live] discontinuity {:?} at block {} counter=0x{addr:08x} header={:02x?}", state, counter.blocks, &blk[..16]);
            }
            match state {
                BlockState::Startup => continue,
                BlockState::Gap(lost) if lost <= MAX_BRIDGE_SAMPLES && ofdm.tracking() => {
                    // 短い欠落は失ったサンプル数だけ埋めて時刻を保ち、同期と誤り訂正の状態を引き継ぐ。
                    // 作り直すと初期同期と時間インターリーブの充填に約 2 秒かかり、その間の TS をすべて失う
                    if !run(&mut samples, &mut ofdm, &mut symbols) {
                        break 'outer;
                    }
                    for s in ofdm.push_gap(lost as usize) {
                        symbols += 1;
                        if !send_sym(Arc::new(s)) {
                            break 'outer;
                        }
                    }
                    bridged += 1;
                }
                BlockState::Gap(_) | BlockState::Backward => {
                    // 不連続の前までを処理してから、前後をつながずに状態をすべて作り直す。
                    // 不連続を検出したブロックは新しい区間の先頭になる
                    if !run(&mut samples, &mut ofdm, &mut symbols) {
                        break 'outer;
                    }
                    if !restart(&mut ofdm, &mut segments, &mut erased_done) {
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
        s.bridged = bridged;
        s.symbols = symbols;
        s.ofdm = ofdm.stats.clone();
        s.ofdm.erased_symbols += erased_done;
    }
    // ここで送信側が閉じ、誤り訂正のスレッドはキューの残りを処理して終わる
}

/// 前段・ビタビ・後段のスレッドの間のキューの長さ (シンボル数)。段どうしの処理時間の揺れを吸収するためのもので、
/// B 階層の前段の出力は 1 シンボル約 41 KB のため、64 シンボル (約 72 ms) で約 2.6 MB になる
const STAGE_QUEUE: usize = 64;

/// 部分受信の A 階層が先頭のセグメントを使い、B 階層が続くセグメントを使う
fn layer_segs(li: usize, t: &Tmcc) -> Option<Vec<usize>> {
    let na = t.layers[0].nseg as usize;
    let nb = t.layers[1].nseg as usize;
    let segs: Vec<usize> = if li == 0 { (0..na).collect() } else { (na..na + nb).collect() };
    (!segs.is_empty()).then_some(segs)
}

/// 誤り訂正の出力を、階層の TS、再多重化、統計へ渡す
struct LayerSink {
    li: usize,
    out: Option<Box<dyn Write + Send>>,
    mux: SyncSender<RemuxMsg>,
    stats: Arc<Mutex<LiveStats>>,
    ts: Vec<u8>,
    n: u64,
    segment: u64,
}

impl LayerSink {
    /// PostStage::push で ts に追加したパケットを送り出す。tmcc は同じシンボルの SymbolOut::tmcc
    fn emit(&mut self, post: &PostStage, tmcc: Option<Tmcc>) {
        if !self.ts.is_empty() {
            if let Some(w) = self.out.as_mut() {
                if w.write_all(&self.ts).is_err() {
                    self.out = None;
                    self.stats.lock().unwrap().write_failed[self.li] = true;
                }
            }
            if let Some(tmcc) = tmcc {
                let msg = RemuxMsg { segment: self.segment, layer: self.li, tmcc, tags: post.tags.clone(), data: std::mem::take(&mut self.ts) };
                // 再多重化のスレッドが終わっていても受信は続ける
                let _ = self.mux.send(msg);
            }
            self.ts.clear();
        }
        self.n += 1;
        if self.n % 64 == 0 {
            self.stats.lock().unwrap().layers[self.li] = post.stats.clone();
        }
    }

    fn reset(&mut self, post: Option<PostStage>) {
        self.segment += 1;
        let mut s = self.stats.lock().unwrap();
        if let Some(p) = post {
            add_stats(&mut s.layers_done[self.li], &p.stats);
        }
        s.layers[self.li] = FecStats::default();
    }

    fn finish(mut self, post: Option<PostStage>) {
        if let Some(w) = self.out.as_mut() {
            let _ = w.flush();
        }
        if let Some(p) = post {
            self.stats.lock().unwrap().layers[self.li] = p.stats;
        }
    }
}

/// A 階層の誤り訂正。処理量が小さいため 3 段を同じスレッドで順に処理する
fn fec_loop(li: usize, threads: usize, rx: Receiver<FecMsg>, mut sink: LayerSink) {
    let mut pipe: Option<LayerPipe> = None;
    for m in rx {
        match m {
            FecMsg::Sym(s) => {
                if pipe.is_none() {
                    if let Some(t) = s.tmcc {
                        if let Some(segs) = layer_segs(li, &t) {
                            pipe = Some(LayerPipe::new(&t.layers[li], &segs, threads));
                        }
                    }
                }
                if let Some(p) = pipe.as_mut() {
                    p.push(&s, &mut sink.ts);
                    sink.emit(&p.post, s.tmcc);
                }
            }
            FecMsg::Reset => sink.reset(pipe.take().map(|p| p.post)),
        }
    }
    sink.finish(pipe.map(|p| p.post));
}

enum VitMsg {
    Start(VitStage, PostStage),
    Data { q: Vec<i8>, produced: bool, frame_symbol: Option<u64>, tmcc: Option<Tmcc> },
    Reset,
}

enum PostMsg {
    Start(PostStage),
    Data { bytes: Vec<u8>, produced: bool, frame_symbol: Option<u64>, tmcc: Option<Tmcc> },
    Reset,
}

/// B 階層の誤り訂正。前段、ビタビ、後段を別々のスレッドで動かし、処理を重ねる。
/// 3 段を同じスレッドで順に処理すると、Intel N150 では前段と後段だけで実時間の約 0.8 倍かかり、
/// ビタビと合わせると実時間に間に合わなかった。段の間はシンボルの順に渡すため、出力は順に処理した場合と一致する
fn fec_loop_staged(li: usize, threads: usize, rx: Receiver<FecMsg>, mut sink: LayerSink) {
    let name = ["a", "b"][li];
    let (vit_tx, vit_rx) = sync_channel::<VitMsg>(STAGE_QUEUE);
    let (post_tx, post_rx) = sync_channel::<PostMsg>(STAGE_QUEUE);
    let vit_thread = std::thread::Builder::new()
        .name(format!("fec-{name}-vit"))
        .spawn(move || {
            let mut vit: Option<VitStage> = None;
            // 作業スレッドで復号中のブロックは、返す時点がスレッドの進み具合で変わる。入力の終わりと状態の作り直しの前に
            // 待って後段へ流し、出力が同じ入力に対して変わらないようにする (渡す位置は直前のシンボル)
            let mut last: (Option<u64>, Option<Tmcc>) = (None, None);
            let drain = |vit: &mut Option<VitStage>, last: (Option<u64>, Option<Tmcc>)| {
                let mut bytes = Vec::new();
                if let Some(v) = vit.as_mut() {
                    v.drain(&mut bytes);
                }
                (!bytes.is_empty()).then(|| PostMsg::Data { bytes, produced: false, frame_symbol: last.0, tmcc: last.1 })
            };
            for m in vit_rx {
                let pm = match m {
                    VitMsg::Start(v, p) => {
                        vit = Some(v);
                        PostMsg::Start(p)
                    }
                    VitMsg::Data { q, produced, frame_symbol, tmcc } => {
                        let mut bytes = Vec::new();
                        if let (true, Some(v)) = (produced, vit.as_mut()) {
                            v.push(&q, &mut bytes);
                        }
                        last = (frame_symbol, tmcc);
                        PostMsg::Data { bytes, produced, frame_symbol, tmcc }
                    }
                    VitMsg::Reset => {
                        if let Some(pm) = drain(&mut vit, last) {
                            if post_tx.send(pm).is_err() {
                                break;
                            }
                        }
                        vit = None;
                        PostMsg::Reset
                    }
                };
                if post_tx.send(pm).is_err() {
                    break;
                }
            }
            if let Some(pm) = drain(&mut vit, last) {
                let _ = post_tx.send(pm);
            }
        })
        .expect("spawn viterbi stage thread");
    let post_thread = std::thread::Builder::new()
        .name(format!("fec-{name}-post"))
        .spawn(move || {
            let mut post: Option<PostStage> = None;
            for m in post_rx {
                match m {
                    PostMsg::Start(p) => post = Some(p),
                    PostMsg::Data { bytes, produced, frame_symbol, tmcc } => {
                        if let Some(p) = post.as_mut() {
                            p.push(frame_symbol, produced, &bytes, &mut sink.ts);
                            sink.emit(p, tmcc);
                        }
                    }
                    PostMsg::Reset => sink.reset(post.take()),
                }
            }
            sink.finish(post);
        })
        .expect("spawn post stage thread");
    let mut pre: Option<PreStage> = None;
    for m in rx {
        let vm = match m {
            FecMsg::Sym(s) => {
                if pre.is_none() {
                    if let Some(t) = s.tmcc {
                        if let Some(segs) = layer_segs(li, &t) {
                            let (p, v, po) = stages(&t.layers[li], &segs, threads, Some(&format!("fec-{name}-vit")));
                            pre = Some(p);
                            if vit_tx.send(VitMsg::Start(v, po)).is_err() {
                                break;
                            }
                        }
                    }
                }
                let Some(p) = pre.as_mut() else { continue };
                let mut q = Vec::new();
                let produced = p.push(&s, &mut q);
                VitMsg::Data { q, produced, frame_symbol: s.frame_symbol, tmcc: s.tmcc }
            }
            FecMsg::Reset => {
                pre = None;
                VitMsg::Reset
            }
        };
        if vit_tx.send(vm).is_err() {
            break;
        }
    }
    // 送信側を閉じ、後ろの段がキューの残りを処理し終えるまで待つ (Live::finish はこのスレッドの終了を待つ)
    drop(vit_tx);
    let _ = vit_thread.join();
    let _ = post_thread.join();
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
#[cfg(windows)]
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requested_ofdm_resync_also_resets_both_fec_layers() {
        let (input, rx) = sync_channel(1);
        let (fec_a, rx_a) = sync_channel(1);
        let (fec_b, rx_b) = sync_channel(1);
        let stats = Arc::new(Mutex::new(LiveStats::default()));
        let reset = Arc::new(AtomicBool::new(true));
        input.send(vec![0; BLOCK]).unwrap();
        drop(input);

        ofdm_loop(rx, vec![fec_a, fec_b], stats.clone(), Some(reset.clone()));

        assert!(!reset.load(Ordering::Relaxed));
        assert!(matches!(rx_a.try_recv(), Ok(FecMsg::Reset)));
        assert!(matches!(rx_b.try_recv(), Ok(FecMsg::Reset)));
        assert_eq!(stats.lock().unwrap().segments, 2);
    }
}
