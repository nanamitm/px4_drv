//! ITU-T J.83 Annex B (256QAM) の誤り訂正。
//! 設計と、検証環境の信号で確かめた仕様の解釈は winusb/docs/msi2500-j83b-design.md の 2.4 節と 5 節を参照。
//!
//! 受信側の順序: トレリス符号化変調の復号 → 差動の逆演算とフレーム同期 → 逆ランダム化 → インターリーブの解除
//! → 拡張 RS(128,122) → MPEG パケットのチェックサムによる同期。

use num_complex::Complex32;
use std::collections::VecDeque;
use std::sync::OnceLock;

/// 1 フレームのトレリス群の数 (88 RS ブロックと 40 ビットの同期トレーラー)
pub const GROUPS_PER_FRAME: usize = 2076;
/// フレームの末尾にある同期トレリス群の数
pub const SYNC_GROUPS: usize = 5;
pub const RS_N: usize = 128;
pub const RS_K: usize = 122;
pub const RS_BLOCKS_PER_FRAME: usize = 88;
/// 1 フレームの 7 ビットのシンボル数 (88 × 128)
pub const FRAME_SYMBOLS: usize = RS_BLOCKS_PER_FRAME * RS_N;
/// 1 フレームの MPEG のバイト数 (88 × 122 × 7 / 8)。フレームの先頭はバイトの境界に揃う
pub const FRAME_MPEG_BYTES: usize = RS_BLOCKS_PER_FRAME * RS_K * 7 / 8;
/// 同期トレーラーの先頭 32 ビット
pub const SYNC_WORD: u32 = 0x71E8_4DD4;

/// 図 B.19 の 256QAM の配置。MAP[I の番号][Q の番号] = C7..C0。番号はレベル -15 の側から 0..15。
/// C4 が I の番号の偶奇、C0 が Q の番号の偶奇に当たる (試験で確認する)。
pub const MAP: [[u8; 16]; 16] = [
    [0xee, 0xed, 0xea, 0xe9, 0xe6, 0xe5, 0xe2, 0xe1, 0x0e, 0x2f, 0x4e, 0x6f, 0x8e, 0xaf, 0xce, 0xef],
    [0xde, 0xdd, 0xda, 0xd9, 0xd6, 0xd5, 0xd2, 0xd1, 0x1c, 0x3d, 0x5c, 0x7d, 0x9c, 0xbd, 0xdc, 0xfd],
    [0xae, 0xad, 0xaa, 0xa9, 0xa6, 0xa5, 0xa2, 0xa1, 0x0a, 0x2b, 0x4a, 0x6b, 0x8a, 0xab, 0xca, 0xeb],
    [0x9e, 0x9d, 0x9a, 0x99, 0x96, 0x95, 0x92, 0x91, 0x18, 0x39, 0x58, 0x79, 0x98, 0xb9, 0xd8, 0xf9],
    [0x6e, 0x6d, 0x6a, 0x69, 0x66, 0x65, 0x62, 0x61, 0x06, 0x27, 0x46, 0x67, 0x86, 0xa7, 0xc6, 0xe7],
    [0x5e, 0x5d, 0x5a, 0x59, 0x56, 0x55, 0x52, 0x51, 0x14, 0x35, 0x54, 0x75, 0x94, 0xb5, 0xd4, 0xf5],
    [0x2e, 0x2d, 0x2a, 0x29, 0x26, 0x25, 0x22, 0x21, 0x02, 0x23, 0x42, 0x63, 0x82, 0xa3, 0xc2, 0xe3],
    [0x1e, 0x1d, 0x1a, 0x19, 0x16, 0x15, 0x12, 0x11, 0x10, 0x31, 0x50, 0x71, 0x90, 0xb1, 0xd0, 0xf1],
    [0xe0, 0xc1, 0xa0, 0x81, 0x60, 0x41, 0x20, 0x01, 0x00, 0x03, 0x04, 0x07, 0x08, 0x0b, 0x0c, 0x0f],
    [0xf2, 0xd3, 0xb2, 0x93, 0x72, 0x53, 0x32, 0x13, 0x30, 0x33, 0x34, 0x37, 0x38, 0x3b, 0x3c, 0x3f],
    [0xe4, 0xc5, 0xa4, 0x85, 0x64, 0x45, 0x24, 0x05, 0x40, 0x43, 0x44, 0x47, 0x48, 0x4b, 0x4c, 0x4f],
    [0xf6, 0xd7, 0xb6, 0x97, 0x76, 0x57, 0x36, 0x17, 0x70, 0x73, 0x74, 0x77, 0x78, 0x7b, 0x7c, 0x7f],
    [0xe8, 0xc9, 0xa8, 0x89, 0x68, 0x49, 0x28, 0x09, 0x80, 0x83, 0x84, 0x87, 0x88, 0x8b, 0x8c, 0x8f],
    [0xfa, 0xdb, 0xba, 0x9b, 0x7a, 0x5b, 0x3a, 0x1b, 0xb0, 0xb3, 0xb4, 0xb7, 0xb8, 0xbb, 0xbc, 0xbf],
    [0xec, 0xcd, 0xac, 0x8d, 0x6c, 0x4d, 0x2c, 0x0d, 0xc0, 0xc3, 0xc4, 0xc7, 0xc8, 0xcb, 0xcc, 0xcf],
    [0xfe, 0xdf, 0xbe, 0x9f, 0x7e, 0x5f, 0x3e, 0x1f, 0xf0, 0xf3, 0xf4, 0xf7, 0xf8, 0xfb, 0xfc, 0xff],
];

/// 256QAM の平均電力 (レベル ±1..±15) の平方根。単位電力のシンボルにこれを掛けるとレベルが奇数の整数になる
pub const LEVEL_SCALE: f32 = 13.038405; // sqrt(170)

// ---------------------------------------------------------------- GF(128) (p(x) = x^7 + x^3 + 1)

pub struct Gf {
    exp: [u8; 256],
    log: [u8; 128],
}

pub fn gf() -> &'static Gf {
    static G: OnceLock<Gf> = OnceLock::new();
    G.get_or_init(|| {
        let mut exp = [0u8; 256];
        let mut log = [0u8; 128];
        let mut v: u16 = 1;
        for i in 0..127 {
            exp[i] = v as u8;
            log[v as usize] = i as u8;
            v <<= 1;
            if v & 0x80 != 0 {
                v ^= 0x89;
            }
        }
        for i in 127..256 {
            exp[i] = exp[i - 127];
        }
        Gf { exp, log }
    })
}

impl Gf {
    #[inline]
    pub fn mul(&self, a: u8, b: u8) -> u8 {
        if a == 0 || b == 0 {
            0
        } else {
            self.exp[self.log[a as usize] as usize + self.log[b as usize] as usize]
        }
    }
    /// alpha^e
    #[inline]
    pub fn alpha(&self, e: usize) -> u8 {
        self.exp[e % 127]
    }
    pub fn inv(&self, a: u8) -> u8 {
        self.exp[(127 - self.log[a as usize] as usize) % 127]
    }
    pub fn pow(&self, a: u8, e: usize) -> u8 {
        if a == 0 {
            return 0;
        }
        self.exp[(self.log[a as usize] as usize * e) % 127]
    }
}

// ---------------------------------------------------------------- 拡張 RS(128,122) (B.5.1)

/// 生成多項式 g(x) = (x + a)(x + a^2)...(x + a^5) の係数 (x^5 の側から)
pub fn rs_generator() -> [u8; 6] {
    let g = gf();
    let mut p = vec![1u8];
    for i in 1..=5 {
        let r = g.alpha(i);
        let mut q = vec![0u8; p.len() + 1];
        for (k, &c) in p.iter().enumerate() {
            q[k] ^= c;
            q[k + 1] ^= g.mul(c, r);
        }
        p = q;
    }
    let mut out = [0u8; 6];
    out.copy_from_slice(&p);
    out
}

/// c(x) = block[0] x^(n-1) + ... + block[n-1] を alpha^power で評価する
fn poly_eval(block: &[u8], power: usize) -> u8 {
    let g = gf();
    let x = g.alpha(power);
    let mut acc = 0u8;
    for &c in block {
        acc = g.mul(acc, x) ^ c;
    }
    acc
}

/// 122 シンボルのメッセージを符号化する。並びは m121..m0 r4..r0 c_ (送出順)
pub fn rs_encode(msg: &[u8]) -> [u8; RS_N] {
    assert_eq!(msg.len(), RS_K);
    let g = gf();
    let gen = rs_generator();
    let mut rem = [0u8; 5];
    for &m in msg {
        let fb = m ^ rem[0];
        for k in 0..4 {
            rem[k] = rem[k + 1] ^ g.mul(fb, gen[k + 1]);
        }
        rem[4] = g.mul(fb, gen[5]);
    }
    let mut out = [0u8; RS_N];
    out[..RS_K].copy_from_slice(msg);
    out[RS_K..RS_K + 5].copy_from_slice(&rem);
    out[127] = poly_eval(&out[..127], 6);
    out
}

/// 127 シンボルの部分を alpha^1..alpha^6 で評価した値。定数倍の表で 1 回の走査にまとめる
fn syndromes(c: &[u8]) -> [u8; 6] {
    static T: OnceLock<[[u8; 128]; 6]> = OnceLock::new();
    let t = T.get_or_init(|| {
        let g = gf();
        let mut t = [[0u8; 128]; 6];
        for (i, row) in t.iter_mut().enumerate() {
            for (v, o) in row.iter_mut().enumerate() {
                *o = g.mul(v as u8, g.alpha(i + 1));
            }
        }
        t
    });
    let mut acc = [0u8; 6];
    for &x in c {
        for i in 0..6 {
            acc[i] = t[i][(acc[i] & 0x7f) as usize] ^ x;
        }
    }
    acc
}

/// 拡張 RS の符号語か (c(alpha^1..5) = 0、末尾 = c(alpha^6))
pub fn rs_check(block: &[u8]) -> bool {
    let s = syndromes(&block[..127]);
    s[..5].iter().all(|&v| v == 0) && s[5] == block[127]
}

fn berlekamp_massey(s: &[u8]) -> (Vec<u8>, usize) {
    let g = gf();
    let (mut c, mut b) = (vec![1u8], vec![1u8]);
    let (mut l, mut m, mut bb) = (0usize, 1usize, 1u8);
    for n in 0..s.len() {
        let mut d = s[n];
        for i in 1..=l {
            if i < c.len() {
                d ^= g.mul(c[i], s[n - i]);
            }
        }
        if d == 0 {
            m += 1;
            continue;
        }
        let t = c.clone();
        let coef = g.mul(d, g.inv(bb));
        if c.len() < b.len() + m {
            c.resize(b.len() + m, 0);
        }
        for i in 0..b.len() {
            c[i + m] ^= g.mul(coef, b[i]);
        }
        if 2 * l <= n {
            l = n + 1 - l;
            b = t;
            bb = d;
            m = 1;
        } else {
            m += 1;
        }
    }
    (c, l)
}

/// 127 シンボルの部分を、根 alpha^1..alpha^len(s) の符号として limit 個まで訂正する
fn rs_correct(c: &[u8], s: &[u8], limit: usize) -> Option<(Vec<u8>, usize)> {
    let g = gf();
    let (lam, l) = berlekamp_massey(s);
    if l > limit || lam.len() <= l {
        return None;
    }
    let mut pos = Vec::new();
    for j in 0..127 {
        // 位置 j は x^(126-j) の係数。X = alpha^(126-j) として、Lambda(X^-1) = 0 になる位置を探す
        let xinv = g.inv(g.alpha(126 - j));
        let mut acc = 0u8;
        for (i, &li) in lam.iter().take(l + 1).enumerate() {
            acc ^= g.mul(li, g.pow(xinv, i));
        }
        if acc == 0 {
            pos.push(j);
        }
    }
    if pos.len() != l {
        return None;
    }
    // Omega(x) = S(x) Lambda(x) mod x^len(s)、e = Omega(X^-1) / Lambda'(X^-1) (最初の根が alpha^1)
    let mut om = vec![0u8; s.len()];
    for i in 0..s.len() {
        for k in 0..=i.min(l) {
            if k < lam.len() {
                om[i] ^= g.mul(lam[k], s[i - k]);
            }
        }
    }
    let mut out = c.to_vec();
    for &j in &pos {
        let xinv = g.inv(g.alpha(126 - j));
        let mut o = 0u8;
        for (i, &oi) in om.iter().enumerate() {
            o ^= g.mul(oi, g.pow(xinv, i));
        }
        let mut d = 0u8;
        let mut i = 1;
        while i <= l && i < lam.len() {
            d ^= g.mul(lam[i], g.pow(xinv, i - 1));
            i += 2;
        }
        if d == 0 {
            return None;
        }
        out[j] ^= g.mul(o, g.inv(d));
    }
    Some((out, l))
}

/// 拡張 RS(128,122) の訂正 (t = 3)。訂正したシンボル数を返し、訂正できなければ None で block は変えない。
/// 127 シンボルの部分を根 alpha^1..alpha^6 の符号 (6 番目の値に拡張シンボルを加える) として 3 個まで訂正し、
/// 拡張シンボル自身の誤りは alpha^1..alpha^5 で 2 個まで訂正した後に再計算して扱う。
pub fn rs_decode(block: &mut [u8; RS_N]) -> Option<usize> {
    let mut s = syndromes(&block[..127]);
    s[5] ^= block[127];
    if s.iter().all(|&v| v == 0) {
        return Some(0);
    }
    let c = &block[..127];
    if let Some((fixed, n)) = rs_correct(c, &s, 3) {
        let mut cand = [0u8; RS_N];
        cand[..127].copy_from_slice(&fixed);
        cand[127] = block[127];
        if rs_check(&cand) {
            *block = cand;
            return Some(n);
        }
    }
    if let Some((fixed, n)) = rs_correct(c, &s[..5], 2) {
        let mut cand = [0u8; RS_N];
        cand[..127].copy_from_slice(&fixed);
        cand[127] = poly_eval(&fixed, 6);
        if rs_check(&cand) {
            *block = cand;
            return Some(n + 1);
        }
    }
    None
}

// ---------------------------------------------------------------- ランダム化 (B.5.4)

/// 1 フレーム分の PN シンボル列。f(x) = x^3 + x + alpha^3 の LFSR を全 1 で初期化し、
/// Galois 形 (s0 <- alpha^3 s2、s1 <- s0 + s2、s2 <- s1) の s2 を出力する。
/// 仕様の図 B.11 からは出力の位置が一意に読めず、検証環境の信号で RS 符号語が揃ったのはこの形だけだった。
pub fn pn_frame() -> &'static [u8; FRAME_SYMBOLS] {
    static PN: OnceLock<[u8; FRAME_SYMBOLS]> = OnceLock::new();
    PN.get_or_init(|| {
        let g = gf();
        let a3 = g.alpha(3);
        let mut s = [0x7fu8; 3];
        let mut out = [0u8; FRAME_SYMBOLS];
        for o in out.iter_mut() {
            *o = s[2];
            s = [g.mul(a3, s[2]), s[0] ^ s[2], s[1]];
        }
        out
    })
}

// ---------------------------------------------------------------- インターリーブ (B.5.2)

/// 同期トレーラーの制御語 (4 ビット) から (I, J) を求める (表 B.2)。予約の値は None
pub fn interleave_params(ctrl: u8) -> Option<(usize, usize)> {
    match ctrl & 15 {
        0b0000 | 0b0001 => Some((128, 1)),
        0b0010 => Some((128, 2)),
        0b0011 => Some((64, 2)),
        0b0100 => Some((128, 3)),
        0b0101 => Some((32, 4)),
        0b0110 => Some((128, 4)),
        0b0111 => Some((16, 8)),
        0b1000 => Some((128, 5)),
        0b1001 => Some((8, 16)),
        0b1010 => Some((128, 6)),
        0b1100 => Some((128, 7)),
        0b1110 => Some((128, 8)),
        _ => None,
    }
}

/// 畳み込みインターリーブの解除。枝 b の遅延は (I - 1 - b) × J 回の訪問で、全シンボルの遅延は (I - 1) × J × I。
/// 交換器はフレームの先頭で枝 0 にあり、1 フレームのシンボル数は I の倍数のため、フレームをまたいでも位置は揃う。
pub struct Deinterleaver {
    pub i: usize,
    pub j: usize,
    fifos: Vec<VecDeque<u8>>,
    k: usize,
    /// まだ有効な出力になっていないシンボル数
    warm: usize,
}

impl Deinterleaver {
    pub fn new(i: usize, j: usize) -> Self {
        let fifos = (0..i).map(|b| VecDeque::from(vec![0u8; (i - 1 - b) * j])).collect();
        Self { i, j, fifos, k: 0, warm: (i - 1) * j * i }
    }
    /// 1 シンボルを入れ、遅延を満たした後の出力を返す
    #[inline]
    pub fn push(&mut self, sym: u8) -> Option<u8> {
        let b = self.k % self.i;
        self.k += 1;
        let f = &mut self.fifos[b];
        f.push_back(sym);
        let out = f.pop_front().unwrap();
        if self.warm > 0 {
            self.warm -= 1;
            None
        } else {
            Some(out)
        }
    }
}

// ---------------------------------------------------------------- 畳み込み符号 (B.5.5.4)
// G1 = 1 + D^2 + D^4、G2 = 1 + D + D^2 + D^3 + D^4、16 状態。状態の bit0 が直前の入力。
// 1 トレリス群 (4 入力) の出力は U1 = G2(b1)、U2 = G2(b2)、U3 = G2(b3)、U4 = G1(b4)、U5 = G2(b4) (パンクチャ [0001;1111])

#[inline]
const fn bcc_out(state: u8, u: u8) -> (u8, u8) {
    let (u1, u2, u3, u4) = (state & 1, (state >> 1) & 1, (state >> 2) & 1, (state >> 3) & 1);
    (u ^ u2 ^ u4, u ^ u1 ^ u2 ^ u3 ^ u4)
}

/// 1 本の BCC の符号化器 (試験の送信側と、受信側の再符号化で使う)
#[derive(Clone, Default)]
pub struct BccEncoder {
    state: u8,
}

impl BccEncoder {
    pub fn group(&mut self, bits: [u8; 4]) -> [u8; 5] {
        let mut out = [0u8; 5];
        for (j, &u) in bits.iter().enumerate() {
            let (g1, g2) = bcc_out(self.state, u & 1);
            if j < 3 {
                out[j] = g2;
            } else {
                out[3] = g1;
                out[4] = g2;
            }
            self.state = ((self.state << 1) | (u & 1)) & 15;
        }
        out
    }
}

/// 枝の出力の符号のマスク [b][p][i]。前状態 i | (b << 3) から入力 p で状態 2i + p へ移るとき、
/// G1 (.0)、G2 (.1) の出力が 1 なら 0、0 なら -1。軟値 s に (s ^ m) - m で符号を付ける。
/// 32 ビット整数の SIMD 乗算は配布向けの基準命令 (SSE2) にないため、乗算の代わりにこの形を使う
type BranchMasks = ([[[i32; 8]; 2]; 2], [[[i32; 8]; 2]; 2]);

const fn branch_masks() -> BranchMasks {
    let mut m1 = [[[0i32; 8]; 2]; 2];
    let mut m2 = [[[0i32; 8]; 2]; 2];
    let mut b = 0;
    while b < 2 {
        let mut p = 0;
        while p < 2 {
            let mut i = 0;
            while i < 8 {
                let (o1, o2) = bcc_out((i | (b << 3)) as u8, p as u8);
                m1[b][p][i] = if o1 == 1 { 0 } else { -1 };
                m2[b][p][i] = if o2 == 1 { 0 } else { -1 };
                i += 1;
            }
            p += 1;
        }
        b += 1;
    }
    (m1, m2)
}

const MASKS: BranchMasks = branch_masks();

/// 逐次処理のビタビ。DEPTH ステップの打ち切りで、CHUNK ステップずつまとめて逆追跡して古いビットを出す。
pub struct Viterbi16 {
    metric: [i32; 16],
    dec: Vec<u16>,
}

const DEPTH: usize = 128;
const CHUNK: usize = 512;

impl Default for Viterbi16 {
    fn default() -> Self {
        Self::new()
    }
}

impl Viterbi16 {
    pub fn new() -> Self {
        Self { metric: [0; 16], dec: Vec::with_capacity(DEPTH + CHUNK + 8) }
    }

    #[inline]
    fn step(&mut self, s1: i32, s2: i32) {
        // s1: G1 の軟値 (送られていなければ 0)、s2: G2 の軟値
        // 状態 2i + p の前状態は i (入力の古いビット 0) と i | 8 (同 1)。偶数と奇数の状態を分けた
        // 8 要素の配列演算にして、前状態の間接参照をなくし、コンパイラーが SIMD 命令へ変換できる形にしている。
        // 判定ビットは状態 2i + p を位置 8p + i に置く
        #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
        // SAFETY: 読み書きはすべて metric (16 要素) と MASKS (各 8 要素) の範囲内。SSE2 は x86 と x86_64 で常に有効
        unsafe {
            #[cfg(target_arch = "x86")]
            use std::arch::x86::*;
            #[cfg(target_arch = "x86_64")]
            use std::arch::x86_64::*;
            let mp = self.metric.as_mut_ptr() as *mut __m128i;
            let lo = [_mm_loadu_si128(mp), _mm_loadu_si128(mp.add(1))];
            let hi = [_mm_loadu_si128(mp.add(2)), _mm_loadu_si128(mp.add(3))];
            let (v1, v2) = (_mm_set1_epi32(s1), _mm_set1_epi32(s2));
            // (v ^ m) - m: m = 0 なら v、m = -1 なら -v
            let sg = |v: __m128i, m: &[i32; 8], h: usize| {
                let mv = _mm_loadu_si128(m.as_ptr().add(4 * h) as *const __m128i);
                _mm_sub_epi32(_mm_xor_si128(v, mv), mv)
            };
            let (m1, m2) = &MASKS;
            let mut nm = [[_mm_setzero_si128(); 2]; 2];
            let mut d = 0u16;
            for p in 0..2 {
                for h in 0..2 {
                    let a = _mm_add_epi32(lo[h], _mm_add_epi32(sg(v1, &m1[0][p], h), sg(v2, &m2[0][p], h)));
                    let b = _mm_add_epi32(hi[h], _mm_add_epi32(sg(v1, &m1[1][p], h), sg(v2, &m2[1][p], h)));
                    let c = _mm_cmpgt_epi32(b, a);
                    nm[p][h] = _mm_or_si128(_mm_and_si128(c, b), _mm_andnot_si128(c, a));
                    d |= (_mm_movemask_ps(_mm_castsi128_ps(c)) as u16) << (8 * p + 4 * h);
                }
            }
            // 状態 2i + p の新しい値を、偶数と奇数の組から元の並びへ戻し、状態 0 を基準に引く
            let base = _mm_shuffle_epi32(nm[0][0], 0);
            _mm_storeu_si128(mp, _mm_sub_epi32(_mm_unpacklo_epi32(nm[0][0], nm[1][0]), base));
            _mm_storeu_si128(mp.add(1), _mm_sub_epi32(_mm_unpackhi_epi32(nm[0][0], nm[1][0]), base));
            _mm_storeu_si128(mp.add(2), _mm_sub_epi32(_mm_unpacklo_epi32(nm[0][1], nm[1][1]), base));
            _mm_storeu_si128(mp.add(3), _mm_sub_epi32(_mm_unpackhi_epi32(nm[0][1], nm[1][1]), base));
            self.dec.push(d);
            return;
        }
        #[allow(unreachable_code)]
        let (m1, m2) = &MASKS;
        let lo: [i32; 8] = self.metric[..8].try_into().unwrap();
        let hi: [i32; 8] = self.metric[8..].try_into().unwrap();
        let mut nm = [[0i32; 8]; 2];
        let mut d = 0u16;
        for p in 0..2 {
            let mut c = [false; 8];
            for i in 0..8 {
                let a = lo[i] + ((s1 ^ m1[0][p][i]) - m1[0][p][i]) + ((s2 ^ m2[0][p][i]) - m2[0][p][i]);
                let b = hi[i] + ((s1 ^ m1[1][p][i]) - m1[1][p][i]) + ((s2 ^ m2[1][p][i]) - m2[1][p][i]);
                c[i] = b > a;
                nm[p][i] = if b > a { b } else { a };
            }
            for (i, &ci) in c.iter().enumerate() {
                d |= (ci as u16) << (8 * p + i);
            }
        }
        // 状態間の差は有限のため、状態 0 を基準に引けば値は溢れない
        let base = nm[0][0];
        for i in 0..8 {
            self.metric[2 * i] = nm[0][i] - base;
            self.metric[2 * i + 1] = nm[1][i] - base;
        }
        self.dec.push(d);
    }

    /// 1 トレリス群の 5 個の軟値 (正で 1) を入れ、確定した入力ビットを out へ追加する
    pub fn push_group(&mut self, s: &[i32; 5], out: &mut Vec<u8>) {
        self.step(0, s[0]);
        self.step(0, s[1]);
        self.step(0, s[2]);
        self.step(s[3], s[4]);
        if self.dec.len() >= DEPTH + CHUNK {
            self.emit(CHUNK, out);
        }
    }

    fn emit(&mut self, count: usize, out: &mut Vec<u8>) {
        let mut st = (0..16).max_by_key(|&n| self.metric[n]).unwrap();
        let len = self.dec.len();
        let mut bits = vec![0u8; len];
        for k in (0..len).rev() {
            bits[k] = (st & 1) as u8;
            let b = ((self.dec[k] >> (((st & 1) << 3) | (st >> 1))) & 1) as usize;
            st = (st >> 1) | (b << 3);
        }
        out.extend_from_slice(&bits[..count]);
        self.dec.drain(..count);
    }

    /// 残りをすべて確定させる (オフラインの捕捉用)
    pub fn finish(&mut self, out: &mut Vec<u8>) {
        let n = self.dec.len();
        if n > 0 {
            self.emit(n, out);
        }
    }
}

// ---------------------------------------------------------------- MPEG パケットのチェックサム (B.4)

/// 187 バイトとチェックサムの 188 バイトに対する症候 S。正しいパケットでは 0x47。
/// パリティ検査行列の列は f(x) = [1 + x^1497 b(x)] / g(x) (g = 1 + x + x^5 + x^6 + x^8、b = 1 + x + x^3 + x^7) の
/// 1497 個の係数を逆順に並べた C を 1 ビットずつずらしたもので、S_j = sum_m R_{j+m} C_m (S0 が MSB)。
/// 検証環境の信号の 4,858 パケットすべてで 0x47 になることを確かめた。
pub struct Checksum {
    /// table[t][v]: 位置 t のバイトが v のときの S への寄与
    table: Vec<[u8; 256]>,
    /// 末尾 (チェックサム) のバイトの寄与から値への逆写像
    inv_last: [u8; 256],
}

pub fn checksum() -> &'static Checksum {
    static C: OnceLock<Checksum> = OnceLock::new();
    C.get_or_init(|| {
        let n = 1497 + 8;
        let series = |input: &dyn Fn(usize) -> u8| {
            let mut c = vec![0u8; n];
            for k in 0..n {
                let mut v = input(k);
                for t in [1usize, 5, 6, 8] {
                    if k >= t {
                        v ^= c[k - t];
                    }
                }
                c[k] = v;
            }
            c
        };
        let one = series(&|k| (k == 0) as u8);
        let xb = series(&|k| [1497usize, 1498, 1500, 1504].contains(&k) as u8);
        let f: Vec<u8> = one.iter().zip(&xb).map(|(a, b)| a ^ b).collect();
        assert!(f[1497..].iter().all(|&v| v == 0), "f(x) must be a polynomial of degree 1496");
        let crev: Vec<u8> = f[..1497].iter().rev().copied().collect();
        // ビット n (MSB 先頭の通し番号) が S_j に寄与するのは 0 <= n - j <= 1496 のとき
        let bit_contrib = |nbit: usize| -> u8 {
            let mut s = 0u8;
            for j in 0..8 {
                if nbit >= j && nbit - j <= 1496 && crev[nbit - j] == 1 {
                    s |= 0x80 >> j;
                }
            }
            s
        };
        let mut table = vec![[0u8; 256]; 188];
        for (t, row) in table.iter_mut().enumerate() {
            let bits: [u8; 8] = std::array::from_fn(|k| bit_contrib(8 * t + k));
            for v in 0..256usize {
                let mut s = 0u8;
                for k in 0..8 {
                    if v & (0x80 >> k) != 0 {
                        s ^= bits[k];
                    }
                }
                row[v] = s;
            }
        }
        let mut inv_last = [0u8; 256];
        let mut seen = [false; 256];
        for v in 0..256usize {
            let s = table[187][v];
            assert!(!seen[s as usize], "checksum byte map is not invertible");
            seen[s as usize] = true;
            inv_last[s as usize] = v as u8;
        }
        Checksum { table, inv_last }
    })
}

impl Checksum {
    /// 188 バイト (187 バイトとチェックサム) の症候
    pub fn syndrome(&self, w: &[u8]) -> u8 {
        let mut s = 0u8;
        for (t, &b) in w[..188].iter().enumerate() {
            s ^= self.table[t][b as usize];
        }
        s
    }
    /// 187 バイトに付けるチェックサム (送信側)
    pub fn encode(&self, payload: &[u8]) -> u8 {
        let mut s = 0u8;
        for (t, &b) in payload[..187].iter().enumerate() {
            s ^= self.table[t][b as usize];
        }
        self.inv_last[(s ^ 0x47) as usize]
    }
}

// ---------------------------------------------------------------- 受信側

/// 偶数番目のレベル (-15, -11, ..., 13) と奇数番目のレベル (-13, ..., 15) への最短距離の二乗の差 (正で偶奇 1)
#[inline]
fn soft_parity(x: f32) -> f32 {
    let near = |base: f32| {
        let k = ((x - base) / 4.0).round().clamp(0.0, 7.0);
        let d = x - (base + 4.0 * k);
        d * d
    };
    near(-15.0) - near(-13.0)
}

/// 偶奇 parity のレベルのうち x に最も近いものの番号 (0..15)
#[inline]
fn nearest_index(x: f32, parity: u8) -> usize {
    let base = -15.0 + 2.0 * parity as f32;
    let k = ((x - base) / 4.0).round().clamp(0.0, 7.0) as usize;
    2 * k + parity as usize
}

const SOFT_SCALE: f32 = 32.0;

#[inline]
fn soft_group(g: &[Complex32]) -> ([i32; 5], [i32; 5]) {
    (
        std::array::from_fn(|t| (soft_parity(g[t].re) * SOFT_SCALE) as i32),
        std::array::from_fn(|t| (soft_parity(g[t].im) * SOFT_SCALE) as i32),
    )
}

#[derive(Clone, Debug, Default)]
pub struct J83bStats {
    pub symbols: u64,
    pub acquisitions: u64,
    pub frames: u64,
    pub frame_sync_losses: u64,
    pub rs_blocks: u64,
    pub rs_clean: u64,
    pub rs_corrected: u64,
    pub rs_corrected_symbols: u64,
    pub rs_uncorrectable: u64,
    pub packets: u64,
    pub checksum_errors: u64,
    pub packet_sync_losses: u64,
    /// 配置の向き (共役か)
    pub conj: bool,
    /// 捕捉後の RS ブロックがほとんど訂正できず、向きを反転して捕捉し直した回数。
    /// 反転前の区間の RS ブロックも rs_uncorrectable へ数える (TS は出力していない)
    pub orientation_flips: u64,
    /// 同期トレーラーの制御語
    pub control_word: Option<u8>,
}

/// 受信した配置の 90 度単位の回転は、復号した X と Y の入れ替えと反転として現れる
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Rotation {
    swap: bool,
    cx: u8,
    cy: u8,
}

const ROTATIONS: [Rotation; 8] = [
    Rotation { swap: false, cx: 0, cy: 0 },
    Rotation { swap: false, cx: 0, cy: 1 },
    Rotation { swap: false, cx: 1, cy: 0 },
    Rotation { swap: false, cx: 1, cy: 1 },
    Rotation { swap: true, cx: 0, cy: 0 },
    Rotation { swap: true, cx: 0, cy: 1 },
    Rotation { swap: true, cx: 1, cy: 0 },
    Rotation { swap: true, cx: 1, cy: 1 },
];

/// 捕捉に使うトレリス群の数 (2 フレーム分の同期語を確かめられる長さ)
const ACQ_GROUPS: usize = 3 * GROUPS_PER_FRAME + 64;
/// フレーム同期を失ったとみなす、連続して同期語が合わなかったフレーム数
const FRAME_MISS_LIMIT: u32 = 3;
/// パケット同期を失ったとみなす、連続してチェックサムが合わなかったパケット数
const PACKET_MISS_LIMIT: u32 = 8;
/// 配置の向きを確かめる、捕捉後の最初の RS ブロック数と、向きが逆とみなす訂正不能の数
/// 捕捉直後に向きの誤りを判定する RS ブロック数
pub const ORIENT_BLOCKS: u32 = 64;
const ORIENT_BAD: u32 = 32;

/// 非同期群の 38 ビットの並び (図 B.15)。(B なら true、番号)
const NONSYNC_ORDER: [(bool, u8); 38] = [
    (false, 0), (true, 0), (false, 1), (false, 2), (false, 3), (true, 1), (true, 2), (true, 3),
    (false, 4), (true, 4), (false, 5), (false, 6), (false, 7), (true, 5), (true, 6), (true, 7),
    (false, 8), (true, 8), (false, 9), (false, 10), (false, 11), (true, 9), (true, 10), (true, 11),
    (false, 12), (true, 12), (false, 13), (false, 14), (false, 15), (true, 13), (true, 14), (true, 15),
    (false, 16), (false, 17), (false, 18), (true, 16), (true, 17), (true, 18),
];

/// 同期群で同期ビット (S0..S7) が入る位置は A0 B0 A4 B4 A8 B8 A12 B12
#[inline]
fn is_sync_pos(n: u8) -> bool {
    n % 4 == 0 && n <= 12
}

/// 図 B.14 と B.15: 群内の時刻 t のシンボルの C7 C6 C5 が A の (a+2, a+1, a)、C3 C2 C1 が B の同じ番号。
/// a は時刻ごとに 1, 5, 9, 13, 16
const UNCODED_BASE: [usize; 5] = [1, 5, 9, 13, 16];

struct Run {
    rot: Rotation,
    vit_x: Viterbi16,
    vit_y: Viterbi16,
    xbits: Vec<u8>,
    ybits: Vec<u8>,
    groups: VecDeque<[Complex32; 5]>,
    carry: Vec<Complex32>,
    enc_x: BccEncoder,
    enc_y: BccEncoder,
    /// 差動の逆演算の前回値 (回転を補正した X、Y)
    xp: u8,
    yp: u8,
    /// 最初のフレームの先頭までに読み捨てる群の数
    skip: usize,
    /// フレーム内の群の番号。最初のフレームの先頭までは None
    gi: Option<usize>,
    /// フレームのデータビットを 7 ビットずつまとめたシンボル (逆ランダム化の前)
    frame_syms: Vec<u8>,
    sym_acc: u8,
    sym_bits: u8,
    sync_bits: Vec<u8>,
    frame_misses: u32,
    deint: Option<Deinterleaver>,
    rs_buf: Vec<u8>,
    /// RS のメッセージのビットをバイトへまとめる途中の値と、そのビット数
    msg_acc: u32,
    msg_n: u32,
    bytes: Vec<u8>,
    bad: Vec<bool>,
    pkt_locked: bool,
    pkt_misses: u32,
    first_blocks: u32,
    first_bad: u32,
}

enum State {
    Acquire(Vec<Complex32>),
    Run(Box<Run>),
}

enum RunResult {
    Ok,
    FrameLost,
    WrongOrientation,
}

pub struct J83bDecoder {
    state: State,
    conj: bool,
    pub stats: J83bStats,
}

impl Default for J83bDecoder {
    fn default() -> Self {
        Self::new()
    }
}

impl J83bDecoder {
    /// 検証環境の MSi2500 ではスペクトルが反転して届くため、共役を既定にする
    pub fn new() -> Self {
        Self::with_conj(true)
    }

    pub fn with_conj(conj: bool) -> Self {
        Self { state: State::Acquire(Vec::new()), conj, stats: J83bStats { conj, ..Default::default() } }
    }

    /// フレーム同期を捕捉して誤り訂正を流しているか
    pub fn locked(&self) -> bool {
        matches!(self.state, State::Run(_))
    }

    /// 等化後のシンボル (単位電力) を入れ、得られた TS (188 バイト単位) を ts へ追加する
    pub fn push(&mut self, syms: &[Complex32], ts: &mut Vec<u8>) {
        self.stats.symbols += syms.len() as u64;
        let mut rest: Vec<Complex32> = syms.iter().map(|&z| self.orient(z)).collect();
        while !rest.is_empty() {
            match &mut self.state {
                State::Acquire(buf) => {
                    let need = ACQ_GROUPS * 5 + 5;
                    let take = (need - buf.len()).min(rest.len());
                    buf.extend_from_slice(&rest[..take]);
                    rest.drain(..take);
                    if buf.len() < need {
                        continue;
                    }
                    let buf = std::mem::take(buf);
                    match acquire(&buf) {
                        Some((off, rot, first_sync)) => {
                            self.stats.acquisitions += 1;
                            // 捕捉に使った区間も、群の境界から流し直して出力に含める
                            let mut replay = buf[off..].to_vec();
                            replay.extend_from_slice(&rest);
                            rest = replay;
                            self.state = State::Run(Box::new(Run::new(rot, first_sync + SYNC_GROUPS)));
                        }
                        None => {
                            // 同期が取れなければ、後半を残して次の区間と合わせて探し直す
                            self.state = State::Acquire(buf[buf.len() / 2..].to_vec());
                        }
                    }
                }
                State::Run(run) => {
                    let r = run.feed(&rest, &mut self.stats, ts);
                    rest.clear();
                    match r {
                        RunResult::Ok => {}
                        RunResult::FrameLost => {
                            self.stats.frame_sync_losses += 1;
                            self.state = State::Acquire(Vec::new());
                        }
                        RunResult::WrongOrientation => {
                            // 捕捉後の RS ブロックがほとんど訂正できない場合は、配置の向き (共役) を反転して捕捉し直す
                            self.conj = !self.conj;
                            self.stats.conj = self.conj;
                            self.stats.orientation_flips += 1;
                            self.state = State::Acquire(Vec::new());
                        }
                    }
                }
            }
        }
    }

    #[inline]
    fn orient(&self, z: Complex32) -> Complex32 {
        let z = z * LEVEL_SCALE;
        if self.conj {
            z.conj()
        } else {
            z
        }
    }
}

/// 捕捉: 群の境界 (5 通り) と回転 (8 通り) を決め、(境界のずれ、回転、最初の同期群の位置) を返す
fn acquire(buf: &[Complex32]) -> Option<(usize, Rotation, usize)> {
    let si: Vec<i32> = buf.iter().map(|z| (soft_parity(z.re) * SOFT_SCALE) as i32).collect();
    let sq: Vec<i32> = buf.iter().map(|z| (soft_parity(z.im) * SOFT_SCALE) as i32).collect();
    // 群の境界: 先頭 4000 群を復号して再符号化した符号化ビットと硬判定の不一致が最小のもの
    let probe = 4000usize;
    let mut best = (0usize, f64::MAX);
    for off in 0..5 {
        let mut v = Viterbi16::new();
        let mut bits = Vec::new();
        for g in 0..probe {
            let s: [i32; 5] = si[off + 5 * g..off + 5 * g + 5].try_into().unwrap();
            v.push_group(&s, &mut bits);
        }
        v.finish(&mut bits);
        let mut enc = BccEncoder::default();
        let mut mism = 0usize;
        for g in 0..probe {
            let u = enc.group(bits[4 * g..4 * g + 4].try_into().unwrap());
            for t in 0..5 {
                mism += (u[t] != (si[off + 5 * g + t] > 0) as u8) as usize;
            }
        }
        let r = mism as f64 / (probe * 5) as f64;
        if r < best.1 {
            best = (off, r);
        }
    }
    // 正しい境界では符号化ビットの誤り率程度、ほかの境界では約 5% (検証環境の信号)
    if best.1 > 0.03 {
        return None;
    }
    let off = best.0;
    let ng = (buf.len() - off) / 5;
    let decode = |s: &[i32]| {
        let mut v = Viterbi16::new();
        let mut bits = Vec::with_capacity(ng * 4);
        for g in 0..ng {
            let a: [i32; 5] = s[off + 5 * g..off + 5 * g + 5].try_into().unwrap();
            v.push_group(&a, &mut bits);
        }
        v.finish(&mut bits);
        bits
    };
    let x0 = decode(&si);
    let y0 = decode(&sq);
    for rot in ROTATIONS {
        let (x, y) = if rot.swap { (&y0, &x0) } else { (&x0, &y0) };
        let mut sbits = Vec::with_capacity(ng * 8);
        let (mut xp, mut yp) = (0u8, 0u8);
        for g in 0..ng {
            for k in 0..4 {
                let xj = x[4 * g + k] ^ rot.cx;
                let yj = y[4 * g + k] ^ rot.cy;
                let (w, z) = diff_decode(xj, yj, xp, yp);
                xp = xj;
                yp = yj;
                sbits.push(w);
                sbits.push(z);
            }
        }
        let word_at = |g: usize| -> u32 { sbits[8 * g..8 * g + 32].iter().fold(0u32, |a, &b| (a << 1) | b as u32) };
        // 32 ビットの同期語が 2,076 群の間隔で 2 回続けて現れる位置
        for g in 0..ng.saturating_sub(GROUPS_PER_FRAME + 4) {
            if word_at(g) == SYNC_WORD && word_at(g + GROUPS_PER_FRAME) == SYNC_WORD {
                return Some((off, rot, g));
            }
        }
    }
    None
}

/// 差動プリコーダーの逆 (図 B.16): a = X + X'、b = Y + Y'、s = X' + Y' として Z = a + b、W = a + Z s
#[inline]
fn diff_decode(x: u8, y: u8, xp: u8, yp: u8) -> (u8, u8) {
    let a = x ^ xp;
    let b = y ^ yp;
    let z = a ^ b;
    let w = a ^ (z & (xp ^ yp));
    (w, z)
}

impl Run {
    fn new(rot: Rotation, skip: usize) -> Self {
        Self {
            rot,
            vit_x: Viterbi16::new(),
            vit_y: Viterbi16::new(),
            xbits: Vec::new(),
            ybits: Vec::new(),
            groups: VecDeque::new(),
            carry: Vec::new(),
            enc_x: BccEncoder::default(),
            enc_y: BccEncoder::default(),
            xp: 0,
            yp: 0,
            skip,
            gi: if skip == 0 { Some(0) } else { None },
            frame_syms: Vec::with_capacity(FRAME_SYMBOLS),
            sym_acc: 0,
            sym_bits: 0,
            sync_bits: Vec::with_capacity(40),
            frame_misses: 0,
            deint: None,
            rs_buf: Vec::with_capacity(RS_N),
            msg_acc: 0,
            msg_n: 0,
            bytes: Vec::new(),
            bad: Vec::new(),
            pkt_locked: false,
            pkt_misses: 0,
            first_blocks: 0,
            first_bad: 0,
        }
    }

    /// 群の境界から続くシンボル列を処理する (群に満たない端数は次回へ持ち越す)
    fn feed(&mut self, syms: &[Complex32], stats: &mut J83bStats, ts: &mut Vec<u8>) -> RunResult {
        let mut pending = std::mem::take(&mut self.carry);
        pending.extend_from_slice(syms);
        let n = pending.len() / 5 * 5;
        let mut xs = Vec::new();
        let mut ys = Vec::new();
        for g in pending[..n].chunks_exact(5) {
            let (si, sq) = soft_group(g);
            self.vit_x.push_group(&si, &mut xs);
            self.vit_y.push_group(&sq, &mut ys);
            self.groups.push_back(g.try_into().unwrap());
        }
        self.carry = pending[n..].to_vec();
        self.xbits.extend_from_slice(&xs);
        self.ybits.extend_from_slice(&ys);
        let mut p = 0;
        let mut r = RunResult::Ok;
        while p + 4 <= self.xbits.len() && p + 4 <= self.ybits.len() && !self.groups.is_empty() {
            let x: [u8; 4] = self.xbits[p..p + 4].try_into().unwrap();
            let y: [u8; 4] = self.ybits[p..p + 4].try_into().unwrap();
            p += 4;
            let g = self.groups.pop_front().unwrap();
            r = self.group(x, y, &g, stats, ts);
            if !matches!(r, RunResult::Ok) {
                break;
            }
        }
        self.xbits.drain(..p);
        self.ybits.drain(..p);
        r
    }

    fn group(&mut self, x: [u8; 4], y: [u8; 4], syms: &[Complex32; 5], stats: &mut J83bStats, ts: &mut Vec<u8>) -> RunResult {
        // 符号化ビット (受信した座標の向きのまま) を再符号化で求め、偶奇の合う最も近いレベルから C7..C0 を得る
        let u = self.enc_x.group(x);
        let v = self.enc_y.group(y);
        let mut a = [0u8; 19];
        let mut b = [0u8; 19];
        for t in 0..5 {
            let c = MAP[nearest_index(syms[t].re, u[t])][nearest_index(syms[t].im, v[t])];
            let base = UNCODED_BASE[t];
            a[base] = (c >> 5) & 1;
            a[base + 1] = (c >> 6) & 1;
            a[base + 2] = (c >> 7) & 1;
            b[base] = (c >> 1) & 1;
            b[base + 1] = (c >> 2) & 1;
            b[base + 2] = (c >> 3) & 1;
        }
        let (xr, yr) = if self.rot.swap { (y, x) } else { (x, y) };
        for k in 0..4 {
            let xj = xr[k] ^ self.rot.cx;
            let yj = yr[k] ^ self.rot.cy;
            let (w, z) = diff_decode(xj, yj, self.xp, self.yp);
            self.xp = xj;
            self.yp = yj;
            a[4 * k] = w;
            b[4 * k] = z;
        }
        if self.skip > 0 {
            self.skip -= 1;
            if self.skip == 0 {
                self.gi = Some(0);
            }
            return RunResult::Ok;
        }
        let Some(gi) = self.gi else { return RunResult::Ok };
        let sync_group = gi >= GROUPS_PER_FRAME - SYNC_GROUPS;
        for &(is_b, n) in NONSYNC_ORDER.iter() {
            let bit = if is_b { b[n as usize] } else { a[n as usize] };
            if sync_group && is_sync_pos(n) {
                self.sync_bits.push(bit);
            } else {
                self.sym_acc = (self.sym_acc << 1) | bit;
                self.sym_bits += 1;
                if self.sym_bits == 7 {
                    self.frame_syms.push(self.sym_acc & 0x7f);
                    self.sym_acc = 0;
                    self.sym_bits = 0;
                }
            }
        }
        if gi + 1 < GROUPS_PER_FRAME {
            self.gi = Some(gi + 1);
            return RunResult::Ok;
        }
        self.gi = Some(0);
        self.end_of_frame(stats, ts)
    }

    fn end_of_frame(&mut self, stats: &mut J83bStats, ts: &mut Vec<u8>) -> RunResult {
        stats.frames += 1;
        debug_assert_eq!((self.frame_syms.len(), self.sym_bits), (FRAME_SYMBOLS, 0));
        debug_assert_eq!(self.sync_bits.len(), 40);
        let word = self.sync_bits[..32].iter().fold(0u32, |a, &b| (a << 1) | b as u32);
        let ctrl = self.sync_bits[32..36].iter().fold(0u8, |a, &b| (a << 1) | b);
        let frame_syms = std::mem::take(&mut self.frame_syms);
        self.frame_syms.reserve(FRAME_SYMBOLS);
        self.sync_bits.clear();
        if word != SYNC_WORD {
            self.frame_misses += 1;
            if self.frame_misses >= FRAME_MISS_LIMIT {
                return RunResult::FrameLost;
            }
        } else {
            self.frame_misses = 0;
            if stats.control_word != Some(ctrl) || self.deint.is_none() {
                // 制御語が変わったらインターリーブの解除を作り直す (遅延分のデータは失われる)
                stats.control_word = Some(ctrl);
                self.deint = interleave_params(ctrl).map(|(i, j)| Deinterleaver::new(i, j));
                self.rs_buf.clear();
                self.msg_acc = 0;
                self.msg_n = 0;
            }
        }
        let pn = pn_frame();
        for (k, &s7) in frame_syms.iter().enumerate() {
            let sym = s7 ^ pn[k];
            let Some(d) = self.deint.as_mut() else { continue };
            let Some(out) = d.push(sym) else { continue };
            self.rs_buf.push(out);
            if self.rs_buf.len() < RS_N {
                continue;
            }
            let mut blk: [u8; RS_N] = self.rs_buf[..].try_into().unwrap();
            self.rs_buf.clear();
            stats.rs_blocks += 1;
            let ok = match rs_decode(&mut blk) {
                Some(0) => {
                    stats.rs_clean += 1;
                    true
                }
                Some(n) => {
                    stats.rs_corrected += 1;
                    stats.rs_corrected_symbols += n as u64;
                    true
                }
                None => {
                    stats.rs_uncorrectable += 1;
                    false
                }
            };
            if self.first_blocks < ORIENT_BLOCKS {
                self.first_blocks += 1;
                self.first_bad += (!ok) as u32;
                if self.first_blocks == ORIENT_BLOCKS && self.first_bad > ORIENT_BAD {
                    return RunResult::WrongOrientation;
                }
            }
            for &s in &blk[..RS_K] {
                self.msg_acc = (self.msg_acc << 7) | s as u32;
                self.msg_n += 7;
                if self.msg_n >= 8 {
                    self.msg_n -= 8;
                    self.bytes.push((self.msg_acc >> self.msg_n) as u8);
                    self.bad.push(!ok);
                    self.msg_acc &= (1 << self.msg_n) - 1;
                }
            }
            self.packets(stats, ts);
        }
        RunResult::Ok
    }

    /// MPEG パケットの同期と出力。チェックサムは直前の 187 バイトに対する値で、次のパケットの先頭にある。
    /// bytes[0] を前のパケットのチェックサムとして、bytes[1..188] がこのパケット、bytes[188] がこのパケットのチェックサム。
    fn packets(&mut self, stats: &mut J83bStats, ts: &mut Vec<u8>) {
        let cs = checksum();
        loop {
            if !self.pkt_locked {
                // 3 パケット続けてチェックサムが合う位置を探す
                if self.bytes.len() < 188 * 4 + 1 {
                    return;
                }
                let found = (0..188).find(|&p| (0..3).all(|k| cs.syndrome(&self.bytes[p + 1 + 188 * k..]) == 0x47));
                match found {
                    Some(p) => {
                        self.bytes.drain(..p);
                        self.bad.drain(..p);
                        self.pkt_locked = true;
                        self.pkt_misses = 0;
                    }
                    None => {
                        let d = self.bytes.len() - 188 * 4;
                        self.bytes.drain(..d);
                        self.bad.drain(..d);
                        return;
                    }
                }
            }
            if self.bytes.len() < 189 {
                return;
            }
            let valid = cs.syndrome(&self.bytes[1..189]) == 0x47;
            let bad = self.bad[1..189].iter().any(|&b| b);
            stats.packets += 1;
            if valid {
                self.pkt_misses = 0;
            } else {
                stats.checksum_errors += 1;
                self.pkt_misses += 1;
            }
            let start = ts.len();
            ts.push(0x47);
            ts.extend_from_slice(&self.bytes[1..188]);
            if !valid || bad {
                // 訂正できなかった RS ブロックを含むか、チェックサムが合わないパケットは TEI を立てる
                ts[start + 1] |= 0x80;
            }
            self.bytes.drain(..188);
            self.bad.drain(..188);
            if self.pkt_misses >= PACKET_MISS_LIMIT {
                stats.packet_sync_losses += 1;
                self.pkt_locked = false;
            }
        }
    }
}
