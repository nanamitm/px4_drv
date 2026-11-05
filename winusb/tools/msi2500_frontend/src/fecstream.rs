//! 階層ごとのストリーム誤り訂正。OFDM のシンボル出力を受け取り、TS パケットを返す。
//!
//! 仕様の細部は一括版 (main.rs) で RS の成否により確かめた組み合わせに固定する:
//! ローテーション解除 (k - s)、セグメント間デインターリーブあり、ビット遅延は b0 が 120、
//! 64QAM は b2 が内側で 1、b4 が |x|=3,5 で 1。

use crate::fastvit::{Isa, StreamDecoder};
use crate::ofdm::{LayerParam, SymbolOut, NDC};
use isdbt_dsp::deinterleave::FREQ_PERM_MODE3;
use isdbt_dsp::rs;
use isdbt_dsp::ts::{best_sync_phase, ByteDeinterleaver, EnergyPrbs, BI_I, BI_M, TSP};
use num_complex::Complex32;
use std::collections::HashMap;

const PRBS_INIT: u16 = 0xa9;
const BYTE_LATENCY: usize = BI_M * BI_I * (BI_I - 1);

fn puncture_pattern(rate: u8) -> &'static [u8] {
    match rate {
        0 => &[1, 1],
        1 => &[1, 1, 0, 1],
        2 => &[1, 1, 0, 1, 1, 0],
        3 => &[1, 1, 0, 1, 1, 0, 0, 1, 1, 0],
        _ => &[1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0],
    }
}

#[derive(Default, Debug, Clone)]
pub struct FecStats {
    pub symbols: u64,
    pub acquired: bool,
    pub acq_attempts: u32,
    pub blocks: u64,
    pub clean: u64,
    pub corrected: u64,
    pub corrected_bytes: u64,
    pub uncorrectable: u64,
    pub tei: u64,
    pub cc_errors: u64,
    pub packets: u64,
}

enum ByteStage {
    Acquire(Vec<u8>),
    Run {
        di: ByteDeinterleaver,
        j: usize,
        ph: usize,
        blk: Vec<u8>,
        bi: usize,
        r: usize,
    },
}

pub struct LayerPipe {
    nc: usize,
    mbits: usize,
    pat: &'static [u8],
    depth: usize,
    map: Vec<usize>, // 層内位置 → SymbolOut の z の添字
    ring_z: Vec<Complex32>,
    ring_w: Vec<f32>,
    t: usize,
    tdel: Vec<usize>,
    delays: Vec<usize>,
    hist: [[i8; 6]; 128],
    hpos: usize,
    pp: usize,
    pow_acc: f64,
    pow_n: u64,
    scale: f32,
    qscale: f32,
    vit: StreamDecoder,
    qbuf: Vec<i8>,
    acc: u8,
    nb: usize,
    stage: ByteStage,
    reset: usize,
    masks: Vec<[u8; TSP]>,
    checker: rs::Checker,
    last_cc: HashMap<u16, u8>,
    pub stats: FecStats,
    /// 直前の push で ts に追加した各パケットの多重フレーム上の位置 (追加した順)。push の先頭で消す
    pub tags: Vec<TspTag>,
    /// 時間インターリーブと遅延補正を合わせた遅延のフレーム数
    lag_frames: u64,
    /// 処理中のシンボルの frame_symbol
    cur_frame_symbol: Option<u64>,
}

/// 誤り訂正の出力パケットの多重フレーム上の位置
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TspTag {
    /// 送信側の多重フレーム番号 (OFDM の frame_symbol / 204 と同じ数え方)。フレーム同期の前は None
    pub frame: Option<i64>,
    /// フレーム内でこの階層の何番目の TSP か (エネルギー拡散の初期化からの位置)
    pub k: u32,
}

/// Mode 3 の時間インターリーブの遅延 (95×I シンボル) と送信側の遅延補正 (I = 1, 2, 4 で 109, 14, 28 シンボル) の
/// 合計はフレーム長 204 シンボルの整数倍になり、階層ごとのフレーム数だけ受信側の出力が遅れる
fn interleave_lag_frames(i: usize) -> u64 {
    match i {
        0 => 0,
        1 | 2 => 1,
        _ => 2,
    }
}

/// パケットを出力したシンボルからの推定で、復号の遅れ (ビットインターリーブ、ビタビのまとめ処理、
/// バイトデインターリーブ) として見込むシンボル数。フレームの判定はこの値から ±102 シンボルの範囲で正しくなる
const DECODE_LATENCY_SYMBOLS: i64 = 8;

impl LayerPipe {
    /// segs: この階層の論理セグメント番号 (昇順)
    pub fn new(p: &LayerParam, segs: &[usize], threads: usize) -> Self {
        let n_l = segs.len();
        let nc = n_l * NDC;
        let mut map = vec![0usize; nc];
        for (j, &s) in segs.iter().enumerate() {
            for k in 0..NDC {
                let kr = (k + NDC - s % NDC) % NDC;
                let src = FREQ_PERM_MODE3[kr] as usize;
                map[k * n_l + j] = s * NDC + src;
            }
        }
        let idepth = p.idepth();
        let depth = idepth * 95 + 1;
        let mbits = p.mbits();
        let reset = [48, 64, 72, 80, 84][p.rate.min(4) as usize] * mbits / 2 * n_l;
        let mut prbs = EnergyPrbs::with_init(PRBS_INIT);
        let masks = (0..reset).map(|_| {
            let mut m = [0u8; TSP];
            for b in &mut m[1..] { *b = prbs.clock(8) as u8; }
            prbs.clock(8);
            m
        }).collect();
        Self {
            nc,
            mbits,
            pat: puncture_pattern(p.rate),
            depth,
            map,
            ring_z: vec![Complex32::new(0.0, 0.0); depth * nc],
            ring_w: vec![0.0; depth * nc],
            t: 0,
            tdel: (0..nc).map(|c| idepth * (95 - (5 * c) % 96)).collect(),
            delays: (0..mbits).map(|b| 120 - 120 * b / (mbits - 1)).collect(),
            hist: [[0; 6]; 128],
            hpos: 0,
            pp: 0,
            pow_acc: 0.0,
            pow_n: 0,
            scale: 0.0,
            qscale: 12.0,
            vit: StreamDecoder::new(8192, 192, threads, Isa::Base),
            qbuf: Vec::new(),
            acc: 0,
            nb: 0,
            stage: ByteStage::Acquire(Vec::new()),
            reset,
            masks,
            checker: rs::Checker::new(),
            last_cc: HashMap::new(),
            stats: FecStats::default(),
            tags: Vec::new(),
            lag_frames: interleave_lag_frames(idepth),
            cur_frame_symbol: None,
        }
    }

    /// 1 シンボルを投入し、得られた TS パケット (188 バイト単位) を ts に追加する。
    pub fn push(&mut self, s: &SymbolOut, ts: &mut Vec<u8>) {
        self.tags.clear();
        self.cur_frame_symbol = s.frame_symbol;
        let nc = self.nc;
        let slot = self.t % self.depth;
        for (li, &src) in self.map.iter().enumerate() {
            self.ring_z[slot * nc + li] = s.z[src];
            self.ring_w[slot * nc + li] = s.w[src];
        }
        let warm = self.depth - 1;
        if self.t < warm {
            for li in 0..nc {
                self.pow_acc += s.z[self.map[li]].norm_sqr() as f64;
            }
            self.pow_n += nc as u64;
            self.t += 1;
            return;
        }
        if self.scale == 0.0 {
            if self.pow_n == 0 {
                for li in 0..nc { self.pow_acc += s.z[self.map[li]].norm_sqr() as f64; }
                self.pow_n = nc as u64;
            }
            let target = if self.mbits == 6 { 42.0 } else { 2.0 };
            self.scale = (target / (self.pow_acc / self.pow_n as f64)).sqrt() as f32;
        }
        self.qbuf.clear();
        let mut soft = [0f32; 6];
        for c in 0..nc {
            let ts_ = self.t - self.tdel[c];
            let sl = ts_ % self.depth;
            let x = self.ring_z[sl * nc + c] * self.scale;
            let w = self.ring_w[sl * nc + c];
            demap(x, w, self.mbits, &mut soft);
            let mut qs = [0i8; 6];
            for b in 0..self.mbits { qs[b] = (soft[b] * self.qscale).round().clamp(-127.0, 127.0) as i8; }
            self.hist[self.hpos] = qs;
            for b in 0..self.mbits {
                let v = self.hist[(self.hpos + 128 - self.delays[b]) & 127][b];
                while self.pat[self.pp] == 0 {
                    self.qbuf.push(0);
                    self.pp = (self.pp + 1) % self.pat.len();
                }
                self.qbuf.push(v);
                self.pp = (self.pp + 1) % self.pat.len();
            }
            self.hpos = (self.hpos + 1) & 127;
        }
        self.t += 1;
        self.stats.symbols += 1;
        let bits = self.vit.push(&self.qbuf);
        let mut bytes = Vec::with_capacity(bits.len() / 8 + 1);
        for b in bits {
            self.acc = (self.acc << 1) | (b & 1);
            self.nb += 1;
            if self.nb == 8 {
                bytes.push(self.acc);
                self.acc = 0;
                self.nb = 0;
            }
        }
        self.push_bytes(&bytes, ts);
    }

    fn push_bytes(&mut self, bytes: &[u8], ts: &mut Vec<u8>) {
        if let ByteStage::Acquire(buf) = &mut self.stage {
            buf.extend_from_slice(bytes);
            let need = BYTE_LATENCY + BI_I + TSP * 400.min(self.reset * 2).max(100);
            if buf.len() < need { return; }
            let buf = std::mem::take(buf);
            self.stats.acq_attempts += 1;
            match self.acquire(&buf) {
                Some((c, ph, r)) => {
                    self.stage = ByteStage::Run { di: ByteDeinterleaver::new(), j: 0, ph, blk: Vec::with_capacity(TSP), bi: 0, r };
                    self.stats.acquired = true;
                    let rest = buf[c..].to_vec();
                    self.run_bytes(&rest, ts);
                }
                None => {
                    // 探索に失敗したら、新しいバイトで取り直す
                    self.stage = ByteStage::Acquire(Vec::new());
                }
            }
            return;
        }
        self.run_bytes(bytes, ts);
    }

    fn acquire(&self, bytes: &[u8]) -> Option<(usize, usize, usize)> {
        let mut best = (0usize, 0usize, 0usize, 0usize);
        for c in 0..BI_I {
            let mut di = ByteDeinterleaver::new();
            let stream: Vec<u8> = bytes[c..].iter().enumerate()
                .filter_map(|(j, &b)| { let o = di.push(b); (j >= BYTE_LATENCY).then_some(o) }).collect();
            let (ph, sc) = best_sync_phase(&stream);
            if sc < 0.5 { continue; }
            let nblk = stream.len().saturating_sub(ph) / TSP;
            if nblk < 12 { continue; }
            let probe: Vec<usize> = (0..12).map(|i| i * nblk / 12).collect();
            for r in 0..self.reset {
                let mut ok = 0;
                for &bi in &probe {
                    let m = &self.masks[(bi + self.reset - r) % self.reset];
                    let s = ph + bi * TSP;
                    let mut p = [0u8; TSP];
                    for j in 0..TSP { p[j] = stream[s + j] ^ m[j]; }
                    ok += usize::from(self.checker.is_codeword(&p));
                }
                if ok > best.3 { best = (c, ph, r, ok); }
            }
        }
        (best.3 >= 6).then_some((best.0, best.1, best.2))
    }

    fn run_bytes(&mut self, bytes: &[u8], ts: &mut Vec<u8>) {
        let first_tag = self.tags.len();
        self.decode_bytes(bytes, ts);
        self.assign_frames(first_tag);
    }

    /// tags[first..] のフレーム番号を決める。同期の捕捉の直後は数フレーム分のパケットを一度に出すため、
    /// 最後のパケット (いま処理したシンボルの付近にある) を基準にし、それより前は k の巻き戻りで 1 フレームずつ遡る。
    /// k 番目の TSP は送信側でフレームの先頭から k×204/N シンボル付近にあり、出力したシンボルから
    /// 階層の遅延フレーム数、その位置、復号の遅れを引くとフレームの先頭からの位置が求まる
    fn assign_frames(&mut self, first: usize) {
        let (Some(fs), Some(last)) = (self.cur_frame_symbol, self.tags.len().checked_sub(1)) else { return };
        if last < first {
            return;
        }
        let kl = self.tags[last].k as i64;
        let x = fs as i64 - (self.lag_frames * 204) as i64 - kl * 204 / self.reset as i64 - DECODE_LATENCY_SYMBOLS;
        let mut f = (x + 102).div_euclid(204);
        self.tags[last].frame = Some(f);
        for i in (first..last).rev() {
            if self.tags[i].k >= self.tags[i + 1].k {
                f -= 1;
            }
            self.tags[i].frame = Some(f);
        }
    }

    fn decode_bytes(&mut self, bytes: &[u8], ts: &mut Vec<u8>) {
        let ByteStage::Run { di, j, ph, blk, bi, r } = &mut self.stage else { return };
        for &b in bytes {
            let o = di.push(b);
            let jj = *j;
            *j += 1;
            if jj < BYTE_LATENCY + *ph { continue; }
            blk.push(o);
            if blk.len() < TSP { continue; }
            let k = (*bi + self.reset - *r) % self.reset;
            let m = &self.masks[k];
            *bi += 1;
            let mut p = [0u8; TSP];
            for k in 0..TSP { p[k] = blk[k] ^ m[k]; }
            blk.clear();
            self.stats.blocks += 1;
            if self.checker.is_codeword(&p) {
                self.stats.clean += 1;
            } else if let Some(cw) = rs::decode(&p) {
                self.stats.corrected += 1;
                self.stats.corrected_bytes += cw.iter().zip(p.iter()).filter(|(x, y)| x != y).count() as u64;
                p.copy_from_slice(&cw[..TSP]);
            } else {
                self.stats.uncorrectable += 1;
                continue;
            }
            let pkt = &p[..188];
            if pkt[1] & 0x80 != 0 { self.stats.tei += 1; }
            let pid = (((pkt[1] & 0x1f) as u16) << 8) | pkt[2] as u16;
            let afc = (pkt[3] >> 4) & 3;
            let cc = pkt[3] & 0x0f;
            if pid != 0x1fff && (afc & 1) == 1 {
                if let Some(&prev) = self.last_cc.get(&pid) {
                    if cc != (prev + 1) & 0x0f && cc != prev { self.stats.cc_errors += 1; }
                }
                self.last_cc.insert(pid, cc);
            }
            self.stats.packets += 1;
            ts.extend_from_slice(pkt);
            self.tags.push(TspTag { frame: None, k: k as u32 });
        }
    }
}

fn demap(x: Complex32, w: f32, mbits: usize, out: &mut [f32; 6]) {
    let cl = |a: f32| if a.is_finite() { a.clamp(-16.0, 16.0) } else { 0.0 };
    if mbits == 2 {
        out[0] = cl(-x.re * w);
        out[1] = cl(-x.im * w);
        return;
    }
    let f = |a: f32| (-a, 4.0 - a.abs(), 2.0 - (a.abs() - 4.0).abs());
    let (i0, i2, i4) = f(x.re);
    let (q1, q3, q5) = f(x.im);
    out[0] = cl(i0 * w);
    out[1] = cl(q1 * w);
    out[2] = cl(i2 * w);
    out[3] = cl(q3 * w);
    out[4] = cl(i4 * w);
    out[5] = cl(q5 * w);
}
