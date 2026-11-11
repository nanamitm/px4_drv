//! K=7 (G1=0o171, G2=0o133) レート 1/2 のビタビ。整数メトリックとブロック並列。
//!
//! 状態遷移は isdbt-dsp の viterbi.rs と同じ: reg=(u<<6)|s, 次状態 ns=reg>>1。
//! したがって次状態 ns の前状態は s=((ns<<1)&0x3f)|b (b=0,1)、入力ビットは u=ns>>5。
//! 出力は「前後 overlap ステップの助走を付けたブロック」を独立に復号して得る。
//! 助走の長さが十分なら、通しの復号と同じビット列になる (検証は呼び出し側の試験で行う)。

const G1: u16 = 0o171;
const G2: u16 = 0o133;

fn parity(x: u16) -> i16 {
    (x.count_ones() & 1) as i16
}

/// 前状態 2j+b から入力 u で遷移するときの出力 (X, Y) を ±1 で表した表。[u][b][j]
pub struct Tables {
    sx: [[[i16; 32]; 2]; 2],
    sy: [[[i16; 32]; 2]; 2],
}

impl Tables {
    pub fn new() -> Self {
        let mut sx = [[[0i16; 32]; 2]; 2];
        let mut sy = [[[0i16; 32]; 2]; 2];
        for u in 0..2usize {
            for b in 0..2usize {
                for j in 0..32usize {
                    let s = (2 * j + b) as u16;
                    let reg = ((u as u16) << 6) | s;
                    sx[u][b][j] = if parity(reg & G1) == 1 { 1 } else { -1 };
                    sy[u][b][j] = if parity(reg & G2) == 1 { 1 } else { -1 };
                }
            }
        }
        Self { sx, sy }
    }
}

/// 軟判定値 (正で bit=1) を i8 へ量子化する。scale は 1.0 の軟値に対応する整数値。
pub fn quantize(soft: &[f32], scale: f32) -> Vec<i8> {
    soft.iter().map(|&x| (x * scale).round().clamp(-127.0, 127.0) as i8).collect()
}

/// 前進計算の本体。steps 個の (r1, r2) から各ステップの判定 (次状態ごとに 0/1) を dec に書き、最終メトリックを返す。
#[inline(always)]
fn forward_body(t: &Tables, q: &[i8], dec: &mut [u8]) -> [i16; 64] {
    let steps = q.len() / 2;
    let mut m = [0i16; 64];
    for st in 0..steps {
        let r1 = q[2 * st] as i16;
        let r2 = q[2 * st + 1] as i16;
        let mut me = [0i16; 32];
        let mut mo = [0i16; 32];
        for j in 0..32 {
            me[j] = m[2 * j];
            mo[j] = m[2 * j + 1];
        }
        let d = &mut dec[st * 64..st * 64 + 64];
        let mut nm = [0i16; 64];
        for u in 0..2 {
            let sx0 = &t.sx[u][0];
            let sy0 = &t.sy[u][0];
            let sx1 = &t.sx[u][1];
            let sy1 = &t.sy[u][1];
            for j in 0..32 {
                let c0 = me[j].wrapping_add(sx0[j] * r1).wrapping_add(sy0[j] * r2);
                let c1 = mo[j].wrapping_add(sx1[j] * r1).wrapping_add(sy1[j] * r2);
                let pick = c1 > c0;
                nm[u * 32 + j] = if pick { c1 } else { c0 };
                d[u * 32 + j] = pick as u8;
            }
        }
        // 1ステップの増分は最大 254。16 ステップごとに状態 0 のメトリックを引いて i16 の範囲に保つ
        if st % 16 == 15 {
            let base = nm[0];
            for v in nm.iter_mut() {
                *v = v.wrapping_sub(base);
            }
        }
        m = nm;
    }
    m
}

#[target_feature(enable = "avx2")]
unsafe fn forward_avx2(t: &Tables, q: &[i8], dec: &mut [u8]) -> [i16; 64] {
    forward_body(t, q, dec)
}

fn forward_base(t: &Tables, q: &[i8], dec: &mut [u8]) -> [i16; 64] {
    forward_body(t, q, dec)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Isa {
    Base,
    Avx2,
}

pub fn best_isa() -> Isa {
    if is_x86_feature_detected!("avx2") { Isa::Avx2 } else { Isa::Base }
}

/// 1区間を前進計算し、末尾で最良の状態から逆追跡して入力ビットを返す。
fn decode_span(t: &Tables, q: &[i8], isa: Isa, dec: &mut Vec<u8>) -> Vec<u8> {
    let steps = q.len() / 2;
    dec.resize(steps * 64, 0);
    let m = match isa {
        Isa::Avx2 => unsafe { forward_avx2(t, q, dec) },
        Isa::Base => forward_base(t, q, dec),
    };
    // メトリックの比較は wrapping 演算の差で行う (範囲は正規化で保たれている)
    let mut ns = 0usize;
    for s in 1..64 {
        if m[s].wrapping_sub(m[ns]) > 0 {
            ns = s;
        }
    }
    let mut bits = vec![0u8; steps];
    for st in (0..steps).rev() {
        bits[st] = (ns >> 5) as u8;
        let b = dec[st * 64 + ns] as usize;
        ns = ((ns << 1) & 0x3f) | b;
    }
    bits
}

/// ブロック並列で復号する。q は (r1, r2) の並び。block は出力ステップ数、overlap は前後の助走。
pub fn decode_parallel(q: &[i8], block: usize, overlap: usize, threads: usize, isa: Isa) -> Vec<u8> {
    let t = Tables::new();
    decode_region(&t, q, 0, q.len() / 2, block, overlap, threads, isa)
}

/// q の中の出力区間 [out_start, out_end) を、out_start から block ごとに区切って復号する。
/// 各ブロックの助走は q の範囲 [0, q.len()/2) で切り詰める。
fn decode_region(t: &Tables, q: &[i8], out_start: usize, out_end: usize, block: usize, overlap: usize,
                 threads: usize, isa: Isa) -> Vec<u8> {
    let steps = q.len() / 2;
    let n = out_end - out_start;
    let mut out = vec![0u8; n];
    let nblk = n.div_ceil(block);
    let chunks: Vec<&mut [u8]> = out.chunks_mut(block).collect();
    let per = nblk.div_ceil(threads.max(1));
    std::thread::scope(|sc| {
        let mut it = chunks.into_iter().enumerate();
        for _ in 0..threads.max(1) {
            let mine: Vec<(usize, &mut [u8])> = it.by_ref().take(per).collect();
            if mine.is_empty() {
                break;
            }
            sc.spawn(move || {
                let mut dec = Vec::new();
                for (bi, dst) in mine {
                    let s0 = out_start + bi * block;
                    let a = s0.saturating_sub(overlap);
                    let e = (s0 + dst.len() + overlap).min(steps);
                    let bits = decode_span(t, &q[2 * a..2 * e], isa, &mut dec);
                    dst.copy_from_slice(&bits[s0 - a..s0 - a + dst.len()]);
                }
            });
        }
    });
    out
}

/// 逐次投入用の復号器。後ろの助走まで揃ったブロックを、スレッド数×4 個ずつまとめて並列に復号する。
/// ブロック境界と助走の切り詰めは decode_parallel と同じため、同じ入力なら出力も一致する。
pub struct StreamDecoder {
    t: Tables,
    block: usize,
    overlap: usize,
    threads: usize,
    isa: Isa,
    batch: usize,
    /// buf[0] に当たる全体のステップ番号
    base: usize,
    /// 次に出力するステップ番号 (常に block の倍数)
    next_out: usize,
    buf: Vec<i8>,
    max_buf: usize,
}

impl StreamDecoder {
    pub fn new(block: usize, overlap: usize, threads: usize, isa: Isa) -> Self {
        let threads = threads.max(1);
        Self { t: Tables::new(), block, overlap, threads, isa, batch: threads * 4, base: 0, next_out: 0, buf: Vec::new(), max_buf: 0 }
    }
    pub fn batch_blocks(&self) -> usize {
        self.batch
    }
    pub fn max_buffered(&self) -> usize {
        self.max_buf
    }
    fn have(&self) -> usize {
        self.base + self.buf.len() / 2
    }
    fn run(&mut self, out_end: usize) -> Vec<u8> {
        let lo = self.next_out - self.base;
        let hi = out_end - self.base;
        let bits = decode_region(&self.t, &self.buf, lo, hi, self.block, self.overlap, self.threads, self.isa);
        self.next_out = out_end;
        // 次のブロックの前の助走だけ残して捨てる
        let keep_from = self.next_out.saturating_sub(self.overlap).max(self.base);
        let drop_steps = keep_from - self.base;
        self.buf.drain(0..2 * drop_steps);
        self.base = keep_from;
        bits
    }
    /// (r1, r2) の並びを追加し、復号できた分のビットを返す。
    pub fn push(&mut self, q: &[i8]) -> Vec<u8> {
        self.buf.extend_from_slice(q);
        self.max_buf = self.max_buf.max(self.buf.len() / 2);
        let mut out = Vec::new();
        loop {
            let span = self.batch * self.block;
            if self.have() < self.next_out + span + self.overlap {
                break;
            }
            let end = self.next_out + span;
            out.extend(self.run(end));
        }
        out
    }
    /// 入力の終わり。残りを後ろの助走なしで復号する。
    pub fn finish(&mut self) -> Vec<u8> {
        let end = self.have();
        if end <= self.next_out {
            return Vec::new();
        }
        self.run(end)
    }
}
