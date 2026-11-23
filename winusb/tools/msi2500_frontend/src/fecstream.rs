//! 階層ごとのストリーム誤り訂正。OFDM のシンボル出力を受け取り、TS パケットを返す。
//!
//! 仕様の細部は一括版 (main.rs) で RS の成否により確かめた組み合わせに固定する:
//! ローテーション解除 (k - s)、セグメント間デインターリーブあり、ビット遅延は b0 が 120、
//! 64QAM は b2 が内側で 1、b4 が |x|=3,5 で 1。

use crate::fastvit::{best_isa, PoolDecoder, StreamDecoder};
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

/// 1 階層の誤り訂正。前段 (PreStage)、ビタビ (VitStage)、後段 (PostStage) を同じスレッドで順に呼ぶ。
/// live.rs の B 階層は、同じ 3 段を別々のスレッドで動かす (`stages`)
pub struct LayerPipe {
    pre: PreStage,
    vit: VitStage,
    pub post: PostStage,
    qbuf: Vec<i8>,
    bytes: Vec<u8>,
}

/// 前段: キャリアの並べ替え、時間デインターリーブ、デマップ、量子化、ビットデインターリーブ、パンクチャの復元
pub struct PreStage {
    nc: usize,
    mbits: usize,
    pat: &'static [u8],
    depth: usize,
    map: Vec<usize>, // 層内位置 → SymbolOut の z の添字
    /// 時間デインターリーブのリング。1 シンボルを 1 行 (nc 個) とし、行の中は遅延の等しいキャリアの組
    /// ((5c) mod 96 が等しいキャリア、96 組) ごとに連続して並べる (添字は スロット × nc + pos[c])。
    /// 振幅の正規化の係数 (scale) が決まるまでは複素数と重みを ring_z と ring_w に溜め、決まった時点で
    /// 量子化した軟判定値の ring_q へ変換する。以後は書き込み時にデマップと量子化を済ませる (読み出し時と同じ計算)。
    /// Intel N150 (4 コアで L2 2 MB を共有) では、キャリアごとに遅延の異なる行から 12 バイトの値を読むと
    /// 1 シンボルで約 9,000 本のキャッシュラインに触れ、OFDM とビタビと同時に動く間の前段が実時間の約 1.2 倍かかった。
    /// 組ごとに並べると、書き込みは 1 行の連続、読み出しは組ごとに連続した範囲になる
    ring_z: Vec<Complex32>,
    ring_w: Vec<f32>,
    ring_q: Vec<[i8; 8]>,
    t: usize,
    tdel: Vec<usize>,
    /// キャリア c の行の中の位置
    pos: Vec<usize>,
    delays: Vec<usize>,
    hist: [[i8; 8]; 128],
    hpos: usize,
    pp: usize,
    pow_acc: f64,
    pow_n: u64,
    scale: f32,
    qscale: f32,
}

/// ビタビ復号と、出力ビットのバイトへのまとめ
pub struct VitStage {
    vit: VitDecoder,
    acc: u8,
    nb: usize,
}

enum VitDecoder {
    Batch(StreamDecoder),
    Pool(PoolDecoder),
}

/// 後段: バイトデインターリーブ、エネルギー拡散の解除、RS 復号、TS パケットの検査とフレーム位置の付与
pub struct PostStage {
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

/// 1 階層の誤り訂正の 3 段を作る。segs: この階層の論理セグメント番号 (昇順)、threads: ビタビのスレッド数。
/// pool に名前を指定すると、ビタビを常駐の作業スレッド (PoolDecoder) で動かす
pub fn stages(p: &LayerParam, segs: &[usize], threads: usize, pool: Option<&str>) -> (PreStage, VitStage, PostStage) {
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
    let pre = PreStage {
        nc,
        mbits,
        pat: puncture_pattern(p.rate),
        depth,
        map,
        ring_z: vec![Complex32::new(0.0, 0.0); depth * nc],
        ring_w: vec![0.0; depth * nc],
        ring_q: Vec::new(),
        t: 0,
        tdel: (0..nc).map(|c| idepth * (95 - (5 * c) % 96)).collect(),
        // nc はセグメントあたり 384 (= 96 × 4) の倍数のため、各組は nc / 96 個のキャリアを持ち、組の中の順番は c / 96
        pos: (0..nc).map(|c| (5 * c) % 96 * (nc / 96) + c / 96).collect(),
        delays: (0..mbits).map(|b| 120 - 120 * b / (mbits - 1)).collect(),
        hist: [[0; 8]; 128],
        hpos: 0,
        pp: 0,
        pow_acc: 0.0,
        pow_n: 0,
        scale: 0.0,
        qscale: 12.0,
    };
    // AVX2 版と基本版は同じ入力に同じビットを返す (tests/fec.rs の avx2_matches_base)
    let vit = match pool {
        Some(name) => VitDecoder::Pool(PoolDecoder::new(8192, 192, threads, best_isa(), name)),
        None => VitDecoder::Batch(StreamDecoder::new(8192, 192, threads, best_isa())),
    };
    let vit = VitStage { vit, acc: 0, nb: 0 };
    let post = PostStage {
        stage: ByteStage::Acquire(Vec::new()),
        reset,
        masks,
        checker: rs::Checker::new(),
        last_cc: HashMap::new(),
        stats: FecStats::default(),
        tags: Vec::new(),
        lag_frames: interleave_lag_frames(idepth),
        cur_frame_symbol: None,
    };
    (pre, vit, post)
}

impl LayerPipe {
    /// segs: この階層の論理セグメント番号 (昇順)
    pub fn new(p: &LayerParam, segs: &[usize], threads: usize) -> Self {
        let (pre, vit, post) = stages(p, segs, threads, None);
        Self { pre, vit, post, qbuf: Vec::new(), bytes: Vec::new() }
    }

    /// 1 シンボルを投入し、得られた TS パケット (188 バイト単位) を ts に追加する。
    pub fn push(&mut self, s: &SymbolOut, ts: &mut Vec<u8>) {
        let produced = self.pre.push(s, &mut self.qbuf);
        if produced {
            self.vit.push(&self.qbuf, &mut self.bytes);
        }
        self.post.push(s.frame_symbol, produced, &self.bytes, ts);
    }
}

impl PreStage {
    /// 1 シンボルを投入し、ビタビへ渡す (r1, r2) の並びを q に入れる。
    /// 時間デインターリーブの遅延が埋まるまでは何も出さず false を返す
    pub fn push(&mut self, s: &SymbolOut, q: &mut Vec<i8>) -> bool {
        q.clear();
        let nc = self.nc;
        let depth = self.depth;
        let slot = self.t % depth;
        if self.ring_q.is_empty() {
            for (li, &src) in self.map.iter().enumerate() {
                self.ring_z[slot * nc + self.pos[li]] = s.z[src];
                self.ring_w[slot * nc + self.pos[li]] = s.w[src];
            }
        } else {
            for (li, &src) in self.map.iter().enumerate() {
                self.ring_q[slot * nc + self.pos[li]] = self.quantize(s.z[src], s.w[src]);
            }
        }
        let warm = self.depth - 1;
        if self.t < warm {
            for li in 0..nc {
                self.pow_acc += s.z[self.map[li]].norm_sqr() as f64;
            }
            self.pow_n += nc as u64;
            self.t += 1;
            return false;
        }
        if self.scale == 0.0 {
            if self.pow_n == 0 {
                for li in 0..nc { self.pow_acc += s.z[self.map[li]].norm_sqr() as f64; }
                self.pow_n = nc as u64;
            }
            let target = if self.mbits == 6 { 42.0 } else { 2.0 };
            self.scale = (target / (self.pow_acc / self.pow_n as f64)).sqrt() as f32;
            let ring_q = self.ring_z.iter().zip(&self.ring_w).map(|(&z, &w)| self.quantize(z, w)).collect();
            self.ring_q = ring_q;
            self.ring_z = Vec::new();
            self.ring_w = Vec::new();
        }
        for c in 0..nc {
            let ts_ = self.t - self.tdel[c];
            let sl = ts_ % self.depth;
            self.hist[self.hpos] = self.ring_q[sl * nc + self.pos[c]];
            for b in 0..self.mbits {
                let v = self.hist[(self.hpos + 128 - self.delays[b]) & 127][b];
                while self.pat[self.pp] == 0 {
                    q.push(0);
                    self.pp = (self.pp + 1) % self.pat.len();
                }
                q.push(v);
                self.pp = (self.pp + 1) % self.pat.len();
            }
            self.hpos = (self.hpos + 1) & 127;
        }
        self.t += 1;
        true
    }
}

impl PreStage {
    /// 1 キャリアの値をデマップして量子化する
    /// 6 ビット分を使い、要素が 8 バイト境界に揃うよう 8 バイトにする (6 バイトでは要素がキャッシュラインをまたぎ、
    /// Ryzen 9 5900X で録画 IQ の復調全体が約 1 割遅くなった)
    fn quantize(&self, z: Complex32, w: f32) -> [i8; 8] {
        let mut soft = [0f32; 6];
        demap(z * self.scale, w, self.mbits, &mut soft);
        let mut qs = [0i8; 8];
        for b in 0..self.mbits { qs[b] = (soft[b] * self.qscale).round().clamp(-127.0, 127.0) as i8; }
        qs
    }
}

impl VitStage {
    /// (r1, r2) の並びを復号し、まとまったバイトを bytes に入れる (端数のビットは次回へ持ち越す)
    pub fn push(&mut self, q: &[i8], bytes: &mut Vec<u8>) {
        let bits = match &mut self.vit {
            VitDecoder::Batch(d) => d.push(q),
            VitDecoder::Pool(d) => d.push(q),
        };
        self.pack(bits, bytes);
    }

    /// 常駐の作業スレッドで復号中のブロックを待ち、まとまったバイトを bytes に入れる。
    /// 入力の終わりと状態の作り直しの前に呼ぶ (PoolDecoder::drain)
    pub fn drain(&mut self, bytes: &mut Vec<u8>) {
        let bits = match &mut self.vit {
            VitDecoder::Batch(_) => Vec::new(),
            VitDecoder::Pool(d) => d.drain(),
        };
        self.pack(bits, bytes);
    }

    fn pack(&mut self, bits: Vec<u8>, bytes: &mut Vec<u8>) {
        bytes.clear();
        bytes.reserve(bits.len() / 8 + 1);
        for b in bits {
            self.acc = (self.acc << 1) | (b & 1);
            self.nb += 1;
            if self.nb == 8 {
                bytes.push(self.acc);
                self.acc = 0;
                self.nb = 0;
            }
        }
    }
}

impl PostStage {
    /// 1 シンボル分のバイトを処理し、得られた TS パケットを ts に追加する。
    /// frame_symbol と produced は同じシンボルの SymbolOut::frame_symbol と PreStage::push の戻り値。
    /// VitStage::drain のバイトは、直前のシンボルの frame_symbol と produced = false で渡す
    pub fn push(&mut self, frame_symbol: Option<u64>, produced: bool, bytes: &[u8], ts: &mut Vec<u8>) {
        self.tags.clear();
        self.cur_frame_symbol = frame_symbol;
        if produced {
            self.stats.symbols += 1;
        }
        if !bytes.is_empty() {
            self.push_bytes(bytes, ts);
        }
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
