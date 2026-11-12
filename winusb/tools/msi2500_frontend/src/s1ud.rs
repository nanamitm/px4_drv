//! PX-S1UD (Siano SMS2270) の J.83B 用 IQ ストリームを、時間の連続した 12 MS/s のサンプル列へ戻して
//! QamDemod へ渡す。
//!
//! ファームウェアは型 0x037C のメッセージごとに 7360 サンプル (int8 の I0, I1, Q0, Q1 の組) と
//! 20 ビットのブロック番号を送る。各ブロックの先頭のサンプルは前のブロックの末尾から連続し、
//! 先頭と 2 番目のサンプルの間でサンプルが落ちる。落ちる数は一定ではなく、
//! 138.750 MHz の収録では 5 と 6 が不規則に混ざった (約 4 割が 5)。
//! 6 で固定して埋めると、ブロックの境界ごとに約 0.47 シンボルの時刻のずれが残る。
//! チューナーの通過帯域が J.83B のロールオフの裾を削るため Gardner の誤差がほとんど得られず、
//! そのずれをブロック内で取り戻せない。そのため、欠落の長さをブロックごとに判定する。
//!
//! 判定は 2 段で行う。判定帰還で追従している間は、候補の長さごとに復調器を複製して
//! ブロックの先頭を復調し、判定誤差の小さい候補を選ぶ。長さを誤ると時刻が約半シンボルずれて
//! 誤差が大きく増えるため、確実に区別できる。判定帰還へ移る前は、帯域制限された信号の
//! 自己相関から作った共分散で、欠落の前後のサンプルの尤度を候補ごとに比べる
//! (138.750 MHz の収録で、BonDriver_S1UD.dll のタイミング追従から求めた長さと 94% 一致)。
//! 欠落した区間は、同じ共分散から求めた条件付き平均で埋める。
//!
//! 埋めた区間は QamDemod へ消失として渡し、追従と適応に使わせない。
//!
//! あわせて、ゼロ IF の I と Q の利得のずれを補正する (IqBalance)。チップがブロックを落としたときは、
//! 8 ブロックまでは落ちた時間を 0 の消失で埋め、欠落の長さを試行で決めて追従を続ける。

use crate::qam::{decision_error, QamDemod, QamStats, SYMBOL_RATE};
use num_complex::{Complex32, Complex64};
use rustfft::{Fft, FftPlanner};
#[cfg(target_arch = "x86")]
use std::arch::x86::*;
#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::*;
use std::sync::Arc;

/// 1 ブロックのサンプル数
pub const BLOCK_SAMPLES: usize = 7360;
/// 1 ブロックのバイト数 (サンプルと 4 バイトのブロック番号)
pub const BLOCK_BYTES: usize = BLOCK_SAMPLES * 2 + 4;
const SEQ_MASK: u32 = 0xF_FFFF;
/// 試行の候補とする欠落の長さ (サンプル数)。138.750 MHz の収録 (47856 ブロック) では 5 と 6 が大半で、
/// 4 も 387 ブロック (0.8%) で現れた。4 を候補から外すと、そのブロックで約 1 シンボルずれて
/// フレーム同期を失った。7 はこの収録で確認していないが、5 と 6 の両側を対称に試す
const GAP_MIN: usize = 4;
const GAP_MAX: usize = 7;
/// 尤度で判定する候補と、判定できないときの長さ。判定帰還の前は 5 と 6 だけを比べ、
/// まれな長さを尤度の誤りで選ぶことを避ける
const LIK_MIN: usize = 5;
const LIK_MAX: usize = 6;
const GAP_DEFAULT: usize = 6;
/// 0 の消失で埋めて追従を続ける、チップが落としたブロックの最大数。BonDriver_S1UD.dll と同じ値
const MAX_FILLED_BLOCKS: u32 = 8;
/// 試行で復調するブロックの先頭のサンプル数と、誤差の評価から除くシンボル数。
/// 埋めた区間の推定誤差は整合フィルター (±6 シンボル) と等化器 (16 シンボル) を通じて前後へ広がるため除く
const TRIAL_SAMPLES: usize = 640;
const TRIAL_SKIP: usize = 32;
const TRIAL_MIN_SYMBOLS: usize = 64;
/// 試行を採用する条件。復調器の MER の下限と、最小の誤差と 2 番目の誤差の比の上限。
/// 判定帰還へ移った直後で MER が低い間に試行を採用すると、138.750 MHz の収録では誤った判定が
/// 追従を崩し、復号できたパケットが約 9% 減った
const TRIAL_MIN_MER_DB: f32 = 24.0;
const TRIAL_RATIO: f32 = 0.7;
/// 最良の候補の誤差が、MER から期待される誤差のこの倍を超えたら 4 と 7 も試す。
/// 138.750 MHz の収録では、正しい長さで約 1 倍、長さが 1 違うと約 6 倍以上だった
const TRIAL_ESCALATE: f32 = 3.0;
/// 尤度の判定に使う、欠落の前後それぞれのサンプル数
const LIK_SIDE: usize = 128;
/// 自己相関の推定に使うブロック数と、推定し直す間隔 (ブロック数)
const ACF_BLOCKS: u64 = 64;
const ACF_REFRESH: u64 = 4096;
/// 自己相関の推定に使う、ブロックごとのサンプル数
const ACF_SAMPLES: usize = 2048;
/// 共分散の対角へ加える量 (0 遅延の自己相関に対する比)。帯域外の雑音の床に相当する
const LIK_REG: f64 = 1e-4;
/// 推定し直した自己相関からコレスキー分解と補間の重みを作る計算の、1 ブロックあたりの量 (複素数の積和の回数)。
/// 一度に作ると 1 ブロックに約 16 ms (26 ブロック分の実時間) かかったため、約 200 ブロック (0.12 秒) に分ける。
/// 作り終えるまでは前の分解を使う
const BUILD_BUDGET: usize = 65_536;

/// ブロックの int8 の組 (I0, I1, Q0, Q1) をサンプルへ展開する。Q は符号を反転する
pub fn decode_block(block: &[u8], out: &mut Vec<Complex32>) {
    for pair in block[..BLOCK_SAMPLES * 2].chunks_exact(4) {
        for i in 0..2 {
            let re = f32::from(pair[i] as i8) / 128.0;
            let im = -f32::from(pair[i + 2] as i8) / 128.0;
            out.push(Complex32::new(re, im));
        }
    }
}

/// ブロック番号 (下位 20 ビット)
pub fn block_seq(block: &[u8]) -> u32 {
    u32::from_le_bytes(block[BLOCK_SAMPLES * 2..BLOCK_BYTES].try_into().unwrap()) & SEQ_MASK
}

#[derive(Debug, Clone, Default)]
pub struct S1udStats {
    pub blocks: u64,
    /// 判定した欠落の長さごとのブロック数 (添字が長さ)
    pub gaps: [u64; GAP_MAX + 1],
    /// 試行で選んだ回数と、尤度で選んだ回数
    pub trial_decisions: u64,
    pub likelihood_decisions: u64,
    /// 5 と 6 で決まらず 4 と 7 も試行した回数
    pub escalations: u64,
    /// チップが落としたブロックの数、消失で埋めて追従を続けた回数、作り直した回数
    pub lost_blocks: u64,
    pub bridged: u64,
    pub resets: u64,
}

/// 直流と、I と Q の利得と位相のずれの適応補正。
/// PX-S1UD のゼロ IF の I と Q には約 0.5 dB の利得差があり、スペクトルの鏡像が信号に重なって
/// QamDemod (線形の等化器) の MER を約 27.5 dB で頭打ちにした。y = u + w conj(u) とし、
/// 補正後の信号の非円性 E[y^2] が 0 になるよう w を適応させる (BonDriver_S1UD.dll の前処理と同じ形)
#[derive(Clone)]
struct IqBalance {
    dc: Complex32,
    w: Complex32,
    p: f32,
    init: bool,
}

/// 直流の追従の係数 (時定数 約 10 万サンプル) と、w の適応の係数 (時定数 約 5 万サンプル)
const IQ_DC_K: f32 = 1e-5;
const IQ_MU: f32 = 2e-5;
const IQ_P_K: f32 = 1e-4;

impl IqBalance {
    fn new() -> Self {
        Self { dc: Complex32::default(), w: Complex32::default(), p: 0.0, init: false }
    }

    fn apply(&mut self, x: &mut [Complex32]) {
        if !self.init {
            let n = x.len() as f32;
            self.dc = x.iter().sum::<Complex32>() / n;
            self.p = x.iter().map(|v| (v - self.dc).norm_sqr()).sum::<f32>() / n;
            self.init = true;
        }
        // 直流、電力、w の更新は 64 サンプルごとにまとめて反映する。毎サンプル更新すると更新の連鎖が直列になり、
        // 1 ブロックの処理が復調の 4 分の 1 に達した。64 サンプルの間の変化は時定数に対して 0.1 % 程度で、
        // まとめても追従の速さは変わらない
        for chunk in x.chunks_mut(64) {
            let n = chunk.len() as f32;
            let (sum, y2, p) = iq_chunk(chunk, self.dc, self.w);
            self.dc += (sum - self.dc * n) * IQ_DC_K;
            self.p += (p - self.p * n) * IQ_P_K;
            self.w -= y2 * (IQ_MU / self.p.max(1e-12));
        }
    }
}

/// x を y = (x - dc) + w conj(x - dc) へ置き換え、(Σ x, Σ y^2, Σ |y|^2) を返す
fn iq_chunk(x: &mut [Complex32], dc: Complex32, w: Complex32) -> (Complex32, Complex32, f32) {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    // SAFETY: 読み書きは x の範囲内 (2 サンプルずつ 4 要素)。Complex32 は re、im の順の repr(C)
    unsafe {
        let pairs = x.len() / 2;
        let p = x.as_mut_ptr() as *mut f32;
        let d = _mm_set_ps(dc.im, dc.re, dc.im, dc.re);
        // yr = (1 + wr) ur + wi ui、yi = wi ur + (1 - wr) ui を、u と re、im を入れ替えた u の積和で求める
        let a = _mm_set_ps(1.0 - w.re, 1.0 + w.re, 1.0 - w.re, 1.0 + w.re);
        let b = _mm_set1_ps(w.im);
        let (mut s, mut q, mut c) = (_mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps());
        for i in 0..pairs {
            let v = _mm_loadu_ps(p.add(4 * i));
            s = _mm_add_ps(s, v);
            let u = _mm_sub_ps(v, d);
            let y = _mm_add_ps(_mm_mul_ps(u, a), _mm_mul_ps(_mm_shuffle_ps::<0b1011_0001>(u, u), b));
            _mm_storeu_ps(p.add(4 * i), y);
            // [yr^2, yi^2] と [yr yi, yi yr]
            q = _mm_add_ps(q, _mm_mul_ps(y, y));
            c = _mm_add_ps(c, _mm_mul_ps(y, _mm_shuffle_ps::<0b1011_0001>(y, y)));
        }
        let mut o = [[0f32; 4]; 3];
        _mm_storeu_ps(o[0].as_mut_ptr(), s);
        _mm_storeu_ps(o[1].as_mut_ptr(), q);
        _mm_storeu_ps(o[2].as_mut_ptr(), c);
        let mut sum = Complex32::new(o[0][0] + o[0][2], o[0][1] + o[0][3]);
        let (mut rr, mut ii, mut ri) = (o[1][0] + o[1][2], o[1][1] + o[1][3], o[2][0] + o[2][2]);
        if x.len() % 2 == 1 {
            let v = &mut x[2 * pairs];
            sum += *v;
            let u = *v - dc;
            let y = u + w * u.conj();
            *v = y;
            rr += y.re * y.re;
            ii += y.im * y.im;
            ri += y.re * y.im;
        }
        (sum, Complex32::new(rr - ii, 2.0 * ri), rr + ii)
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    {
        let (mut sum, mut y2, mut p) = (Complex32::default(), Complex32::default(), 0f32);
        for v in x.iter_mut() {
            sum += *v;
            let u = *v - dc;
            let y = u + w * u.conj();
            *v = y;
            y2 += y * y;
            p += y.norm_sqr();
        }
        (sum, y2, p)
    }
}

/// 自己相関から作った、欠落の長さごとの共分散のコレスキー分解
struct Likelihood {
    /// 推定中の自己相関と平均の積算
    acc: Vec<Complex64>,
    acc_mean: Complex64,
    acc_blocks: u64,
    /// 判定に使う平均と、候補ごとの下三角行列 (行優先) と対数行列式
    mean: Complex64,
    chol: Vec<(Vec<Complex64>, f64)>,
    /// 候補ごとの補間の重み (g 行 × 2 LIK_SIDE 列)。欠落の r 番目の推定は重みの r 行と観測の積和
    interp: Vec<Vec<Complex32>>,
    /// 作りかけの分解と重み
    build: Option<Build>,
    /// 自己相関の推定に使う長さ ACF_SAMPLES の FFT
    fft: Arc<dyn Fft<f64>>,
    ifft: Arc<dyn Fft<f64>>,
}

/// 自己相関から、候補の長さごとのコレスキー分解と補間の重みを、列や行の単位で少しずつ作る
struct Build {
    acf: Vec<Complex64>,
    mean: Complex64,
    /// 作っている候補の長さ、その共分散と分解、次に求める列と、次に求める補間の重みの行
    g: usize,
    c: Vec<Complex64>,
    l: Vec<Complex64>,
    logdet: f64,
    col: usize,
    row: usize,
    weights: Vec<Complex32>,
    chol: Vec<(Vec<Complex64>, f64)>,
    interp: Vec<Vec<Complex32>>,
}

impl Build {
    fn new(acf: Vec<Complex64>, mean: Complex64) -> Self {
        let n = 2 * LIK_SIDE;
        let c = covariance(&acf, GAP_MIN);
        Self {
            acf,
            mean,
            g: GAP_MIN,
            c,
            l: vec![Complex64::default(); n * n],
            logdet: 0.0,
            col: 0,
            row: 0,
            weights: Vec::new(),
            chol: Vec::new(),
            interp: Vec::new(),
        }
    }

    /// 積和の回数が budget を超えるまで進め、すべての候補を作り終えたら true を返す
    fn step(&mut self, mut budget: usize) -> bool {
        let n = 2 * LIK_SIDE;
        while self.g <= GAP_MAX {
            if self.col < n {
                // コレスキー分解の 1 列
                let j = self.col;
                self.logdet += cholesky_column(&self.c, &mut self.l, n, j);
                self.col += 1;
                budget = budget.saturating_sub((j + 1) * (n - j));
            } else if self.row < self.g {
                // 条件付き平均 C_mo C_oo^-1 v の重みの 1 行 (ブロックごとの計算が積和だけで済むよう前もって求める)
                let cross = cross_covariance(&self.acf, self.g);
                let r = self.row;
                let row: Vec<Complex64> = cross[r * n..(r + 1) * n].iter().map(|c| c.conj()).collect();
                self.weights.extend(solve(&self.l, &row).iter().map(|b| Complex32::new(b.re as f32, -b.im as f32)));
                self.row += 1;
                budget = budget.saturating_sub(n * n);
            } else {
                // 次の候補へ
                let l = std::mem::replace(&mut self.l, vec![Complex64::default(); n * n]);
                self.chol.push((l, std::mem::take(&mut self.logdet)));
                self.interp.push(std::mem::take(&mut self.weights));
                self.g += 1;
                self.col = 0;
                self.row = 0;
                if self.g <= GAP_MAX {
                    self.c = covariance(&self.acf, self.g);
                }
            }
            if budget == 0 {
                break;
            }
        }
        self.g > GAP_MAX
    }
}

impl Likelihood {
    fn new() -> Self {
        Self {
            acc: vec![Complex64::default(); 2 * LIK_SIDE + GAP_MAX + 1],
            acc_mean: Complex64::default(),
            acc_blocks: 0,
            mean: Complex64::default(),
            chol: Vec::new(),
            interp: Vec::new(),
            build: None,
            fft: FftPlanner::new().plan_fft_forward(ACF_SAMPLES),
            ifft: FftPlanner::new().plan_fft_inverse(ACF_SAMPLES),
        }
    }

    fn estimating(&self) -> bool {
        self.acc_blocks < ACF_BLOCKS
    }

    fn restart_estimate(&mut self) {
        self.acc.iter_mut().for_each(|v| *v = Complex64::default());
        self.acc_mean = Complex64::default();
        self.acc_blocks = 0;
        self.build = None;
    }

    /// ブロックの内部 (欠落を含まない) から自己相関を積算し、揃ったら共分散の分解を作り始める
    fn observe(&mut self, x: &[Complex32]) {
        let mut b: Vec<Complex64> = x[1..1 + ACF_SAMPLES].iter().map(|v| Complex64::new(v.re as f64, v.im as f64)).collect();
        let m = b.iter().sum::<Complex64>() / ACF_SAMPLES as f64;
        b.iter_mut().for_each(|v| *v -= m);
        // Σ_{i<n} x[i+k] conj(x[i]) を、先頭 n サンプルだけを残した列との巡回相関として FFT で求める。
        // i + k は ACF_SAMPLES 未満に収まるため巡回の折り返しはない。直接の積和は 1 ブロックに約 0.5 ms かかった
        let n = ACF_SAMPLES - self.acc.len();
        let mut a = b.clone();
        a[n..].iter_mut().for_each(|v| *v = Complex64::default());
        self.fft.process(&mut a);
        self.fft.process(&mut b);
        b.iter_mut().zip(&a).for_each(|(v, a)| *v *= a.conj());
        self.ifft.process(&mut b);
        let scale = 1.0 / (ACF_SAMPLES * n) as f64;
        for (acc, r) in self.acc.iter_mut().zip(&b) {
            *acc += r * scale;
        }
        self.acc_mean += m;
        self.acc_blocks += 1;
        if self.acc_blocks == ACF_BLOCKS {
            let acf: Vec<Complex64> = self.acc.iter().map(|v| v / ACF_BLOCKS as f64).collect();
            self.build = Some(Build::new(acf, self.acc_mean / ACF_BLOCKS as f64));
        }
    }

    /// 作りかけの分解を budget の量だけ進め、作り終えたら判定に使う分解と入れ替える
    fn advance(&mut self, budget: usize) {
        let Some(b) = self.build.as_mut() else {
            return;
        };
        if b.step(budget) {
            let b = self.build.take().unwrap();
            self.chol = b.chol;
            self.interp = b.interp;
            self.mean = b.mean;
        }
    }

    fn ready(&self) -> bool {
        !self.chol.is_empty()
    }

    /// v は欠落の前の LIK_SIDE サンプルと後の LIK_SIDE サンプル。最も尤度の高い長さを返す
    fn decide(&self, v: &[Complex32]) -> usize {
        let v: Vec<Complex64> = v.iter().map(|z| Complex64::new(z.re as f64, z.im as f64) - self.mean).collect();
        let mut best = (f64::NEG_INFINITY, GAP_DEFAULT);
        for g in LIK_MIN..=LIK_MAX {
            let (l, logdet) = &self.chol[g - GAP_MIN];
            let ll = -quad_form(l, &v) - logdet;
            if ll > best.0 {
                best = (ll, g);
            }
        }
        best.1
    }

    /// 欠落の長さ g のときの、欠落したサンプルの条件付き平均 (前後の観測からの線形最小 2 乗誤差の推定)。
    /// 0 で埋めると整合フィルターと等化器を通じて前後のシンボルを乱すため、帯域制限された信号の
    /// 冗長さ (12 MS/s に対して占有帯域は約 5.7 MHz) を使って埋める
    fn interpolate(&self, v: &[Complex32], g: usize) -> Vec<Complex32> {
        let w = &self.interp[g - GAP_MIN];
        let n = v.len();
        let mean = Complex32::new(self.mean.re as f32, self.mean.im as f32);
        (0..g)
            .map(|r| w[r * n..(r + 1) * n].iter().zip(v).map(|(a, b)| a * (b - mean)).sum::<Complex32>() + mean)
            .collect()
    }
}

/// 欠落した g サンプル (前の LIK_SIDE サンプルの直後) と、観測した前後 LIK_SIDE サンプルずつの共分散
fn cross_covariance(acf: &[Complex64], g: usize) -> Vec<Complex64> {
    let n = 2 * LIK_SIDE;
    let pos = |i: usize| if i < LIK_SIDE { i } else { i + g };
    let mut c = vec![Complex64::default(); g * n];
    for r in 0..g {
        let a = LIK_SIDE + r;
        for j in 0..n {
            let b = pos(j);
            c[r * n + j] = if a >= b { acf[a - b] } else { acf[b - a].conj() };
        }
    }
    c
}

/// L L^H a = v を解く
fn solve(l: &[Complex64], v: &[Complex64]) -> Vec<Complex64> {
    let n = v.len();
    let mut y = vec![Complex64::default(); n];
    for i in 0..n {
        let mut s = v[i];
        for k in 0..i {
            s -= l[i * n + k] * y[k];
        }
        y[i] = s / l[i * n + i].re;
    }
    let mut a = vec![Complex64::default(); n];
    for i in (0..n).rev() {
        let mut s = y[i];
        for k in i + 1..n {
            s -= l[k * n + i].conj() * a[k];
        }
        a[i] = s / l[i * n + i].re;
    }
    a
}

/// 欠落の長さ g のときの、前後 LIK_SIDE サンプルずつの共分散
fn covariance(acf: &[Complex64], g: usize) -> Vec<Complex64> {
    let n = 2 * LIK_SIDE;
    let pos = |i: usize| if i < LIK_SIDE { i } else { i + g };
    let mut c = vec![Complex64::default(); n * n];
    for i in 0..n {
        for j in 0..n {
            let (a, b) = (pos(i), pos(j));
            // E[x_a conj(x_b)] = acf(a - b)。acf(k) = E[x_{n+k} conj(x_n)]、acf(-k) = conj(acf(k))
            c[i * n + j] = if a >= b { acf[a - b] } else { acf[b - a].conj() };
        }
        c[i * n + i] += acf[0].re * LIK_REG;
    }
    c
}

/// エルミート正定値行列 c (n × n) のコレスキー分解 (下三角、行優先) の j 列目を l へ書き、対数行列式への寄与を返す。
/// 0..j 列は求め終えている必要がある
fn cholesky_column(c: &[Complex64], l: &mut [Complex64], n: usize, j: usize) -> f64 {
    let mut d = c[j * n + j].re;
    for k in 0..j {
        d -= l[j * n + k].norm_sqr();
    }
    let d = d.max(1e-30).sqrt();
    l[j * n + j] = Complex64::new(d, 0.0);
    for i in j + 1..n {
        let mut s = c[i * n + j];
        for k in 0..j {
            s -= l[i * n + k] * l[j * n + k].conj();
        }
        l[i * n + j] = s / d;
    }
    2.0 * d.ln()
}

/// v^H C^-1 v = |L^-1 v|^2
fn quad_form(l: &[Complex64], v: &[Complex64]) -> f64 {
    let n = v.len();
    let mut y = vec![Complex64::default(); n];
    let mut q = 0.0;
    for i in 0..n {
        let mut s = v[i];
        for k in 0..i {
            s -= l[i * n + k] * y[k];
        }
        y[i] = s / l[i * n + i].re;
        q += y[i].norm_sqr();
    }
    q
}

/// ブロック番号付きの PX-S1UD のサンプルを、欠落の長さを判定しながら QamDemod へ渡す
pub struct S1udFrontend {
    demod: QamDemod,
    fs: f64,
    pll_scale: f32,
    prev_seq: Option<u32>,
    /// 前のブロックの末尾 (LIK_SIDE - 1 サンプル)
    tail: Vec<Complex32>,
    lik: Likelihood,
    iq: IqBalance,
    samples: Vec<Complex32>,
    /// 復調を始めたか (欠落の長さを判定できるまで待つ)
    started: bool,
    pub stats: S1udStats,
}

impl S1udFrontend {
    pub fn new(fs: f64, pll_scale: f32) -> Self {
        Self {
            demod: QamDemod::new(fs, pll_scale),
            fs,
            pll_scale,
            prev_seq: None,
            tail: Vec::new(),
            lik: Likelihood::new(),
            iq: IqBalance::new(),
            samples: Vec::with_capacity(BLOCK_SAMPLES),
            started: false,
            stats: S1udStats::default(),
        }
    }

    pub fn demod_stats(&self) -> &QamStats {
        &self.demod.stats
    }

    /// 14724 バイトのブロックを入れ、復調したシンボルを out へ追加する。
    /// 復調器を作り直したとき (長い欠落) は true を返す。呼び出し側は誤り訂正の状態も作り直す
    pub fn push_block(&mut self, block: &[u8], out: &mut Vec<Complex32>) -> bool {
        let seq = block_seq(block);
        let lost = self.prev_seq.map(|p| seq.wrapping_sub(p).wrapping_sub(1) & SEQ_MASK);
        self.prev_seq = Some(seq);
        self.stats.blocks += 1;
        let mut x = std::mem::take(&mut self.samples);
        x.clear();
        decode_block(block, &mut x);
        self.iq.apply(&mut x);
        if self.stats.blocks % ACF_REFRESH == 0 {
            self.lik.restart_estimate();
        }
        if self.lik.estimating() {
            self.lik.observe(&x);
        }
        // 始めの分解は復調を始める前のため、一度に作る
        self.lik.advance(if self.lik.ready() { BUILD_BUDGET } else { usize::MAX });
        let mut reset = false;
        if !self.lik.ready() {
            // 欠落の長さを判定できるまで復調を始めない。6 で固定して始めると、判定帰還の前に
            // 約 4 割のブロックで時刻がずれ、CMA が収束しない
            self.save_tail(x);
            return false;
        }
        let lost = if self.started { lost } else { None };
        self.started = true;
        match lost {
            None => self.demod.push(&x, out),
            Some(l) if l > 0 => {
                self.stats.lost_blocks += u64::from(l);
                if !(l <= MAX_FILLED_BLOCKS && self.bridge(&x, l, out)) {
                    // 欠落が長いか、落ちた時間の長さを決められないときは、前後をつながずに作り直す
                    self.demod = QamDemod::new(self.fs, self.pll_scale);
                    self.stats.resets += 1;
                    reset = true;
                    self.demod.push(&x, out);
                }
            }
            Some(_) => {
                // 欠落の前後の観測
                let ctx = (self.tail.len() == LIK_SIDE - 1).then(|| {
                    let mut v = Vec::with_capacity(2 * LIK_SIDE);
                    v.extend_from_slice(&self.tail);
                    v.extend_from_slice(&x[..LIK_SIDE + 1]);
                    v
                });
                // 候補の長さの欠落を埋める内容。前後の観測があれば条件付き平均、なければ 0 (始めのブロックのみ)
                let content = |lik: &Likelihood, n: usize| match &ctx {
                    Some(v) => lik.interpolate(v, n),
                    None => vec![Complex32::default(); n],
                };
                // 試行の候補。まず大半を占める 5 と 6 を試す
                let mut cands: Vec<(usize, Vec<Complex32>)> = (LIK_MIN..=LIK_MAX).map(|n| (n, content(&self.lik, n))).collect();
                let mut trials = self.trial(&x, &cands, false);
                // 最良の候補でも誤差が今の MER から期待される値を大きく超えるときは、まれな 4 と 7 も試す。
                // 真の長さが 4 か 7 のとき、5 と 6 の一方は約半シンボルのずれで誤差の比だけでは区別できない
                {
                    if let Some(t) = trials.as_mut() {
                        let best = t.iter().map(|c| c.0).fold(f32::INFINITY, f32::min);
                        let expect = 10f32.powf(-self.demod.stats.mer_db / 10.0);
                        if best > TRIAL_ESCALATE * expect {
                            let extra: Vec<(usize, Vec<Complex32>)> = (GAP_MIN..=GAP_MAX)
                                .filter(|n| !(LIK_MIN..=LIK_MAX).contains(n))
                                .map(|n| (n, content(&self.lik, n)))
                                .collect();
                            self.stats.escalations += 1;
                            if let Some(t2) = self.trial(&x, &extra, false) {
                                t.extend(t2);
                                cands.extend(extra);
                            }
                        }
                    }
                }
                // 試行は、判定帰還で十分な MER に達し、候補の誤差がはっきり分かれたときだけ採用する。
                // 追従が不十分な間は候補の誤差が近く、試行の誤りがさらに追従を崩すため、尤度の判定を使う
                let decisive = trials.as_ref().and_then(|t| {
                    let mut e: Vec<(f32, usize)> = t.iter().enumerate().map(|(i, c)| (c.0, i)).collect();
                    e.sort_by(|a, b| a.0.total_cmp(&b.0));
                    let good = self.demod.stats.mer_db >= TRIAL_MIN_MER_DB;
                    (good && (e.len() == 1 || e[0].0 <= TRIAL_RATIO * e[1].0)).then_some(cands[e[0].1].0)
                });
                let choice = match decisive {
                    Some(n) => {
                        self.stats.trial_decisions += 1;
                        n
                    }
                    None => match ctx.as_ref() {
                        Some(v) => {
                            self.stats.likelihood_decisions += 1;
                            self.lik.decide(v)
                        }
                        None => GAP_DEFAULT,
                    },
                };
                let pos = cands.iter().position(|c| c.0 == choice);
                let fill = match (trials, pos) {
                    // 試行した複製のうち選んだ候補を使い、ブロックの残りを続けて入れる
                    (Some(mut t), Some(i)) => {
                        let (_, d, sym) = t.swap_remove(i);
                        self.demod = d;
                        out.extend_from_slice(&sym);
                        self.demod.push(&x[1 + TRIAL_SAMPLES..], out);
                        choice
                    }
                    _ => {
                        let c = match pos {
                            Some(i) => std::mem::take(&mut cands[i].1),
                            None => content(&self.lik, choice),
                        };
                        self.demod.push_erased(&with_gap(&x, &c, x.len()), 1..1 + c.len(), out);
                        choice
                    }
                };
                self.stats.gaps[fill] += 1;
            }
        }
        self.save_tail(x);
        reset
    }

    fn save_tail(&mut self, x: Vec<Complex32>) {
        self.tail.clear();
        self.tail.extend_from_slice(&x[x.len() - (LIK_SIDE - 1)..]);
        self.samples = x;
    }

    /// チップが l ブロック (1..=MAX_FILLED_BLOCKS) を落としたとき、落ちた時間を 0 の消失で埋めて追従を続ける。
    /// 落ちた時間は l ブロック分のサンプルと l + 1 か所の欠落で、欠落の長さごとに候補を試行して選ぶ。
    /// 判定帰還で追従していないか、候補がはっきり分かれないときは false を返す (呼び出し側が作り直す)
    fn bridge(&mut self, x: &[Complex32], l: u32, out: &mut Vec<Complex32>) -> bool {
        let (l, n) = (l as usize, l as usize + 1);
        let cands: Vec<(usize, Vec<Complex32>)> = (l * BLOCK_SAMPLES + n * GAP_MIN..=l * BLOCK_SAMPLES + n * GAP_MAX)
            .map(|z| (z, vec![Complex32::default(); z]))
            .collect();
        let Some(mut t) = self.trial(x, &cands, true) else {
            return false;
        };
        let mut e: Vec<(f32, usize)> = t.iter().enumerate().map(|(i, c)| (c.0, i)).collect();
        e.sort_by(|a, b| a.0.total_cmp(&b.0));
        if !(self.demod.stats.mer_db >= TRIAL_MIN_MER_DB && e[0].0 <= TRIAL_RATIO * e[1].0) {
            return false;
        }
        let (_, d, sym) = t.swap_remove(e[0].1);
        self.demod = d;
        self.stats.bridged += 1;
        out.extend_from_slice(&sym);
        self.demod.push(&x[1 + TRIAL_SAMPLES..], out);
        true
    }

    /// 判定帰還で追従しているときに、欠落の候補 (長さ, 埋める内容) ごとに復調器を複製してブロックの先頭を試行し、
    /// 候補の順に (判定誤差, 復調器, 出力したシンボル) を返す。判定に足るシンボルが得られない候補があれば None。
    /// 埋めた区間は消失として渡す。erase_head のときは先頭のサンプルも消失とする
    /// (ブロックを落としたあとは、先頭のサンプルと前のサンプルの間の時間が分からないため)
    fn trial(&mut self, x: &[Complex32], fills: &[(usize, Vec<Complex32>)], erase_head: bool) -> Option<Vec<(f32, QamDemod, Vec<Complex32>)>> {
        if !self.demod.stats.dd {
            return None;
        }
        // 試行の末尾 TRIAL_SAMPLES に相当するシンボル数。欠落の影響が残る先頭の TRIAL_SKIP を除いて評価する
        let tail_syms = (TRIAL_SAMPLES as f64 * SYMBOL_RATE / self.fs) as usize;
        let mut res = Vec::with_capacity(fills.len());
        for (_, content) in fills {
            let mut d = self.demod.clone();
            let mut sym = Vec::new();
            let from = if erase_head { 0 } else { 1 };
            d.push_erased(&with_gap(x, content, 1 + TRIAL_SAMPLES), from..1 + content.len(), &mut sym);
            // 試行の途中で判定帰還からやり直した候補があると、比較の前提が崩れる
            if !d.stats.dd || d.stats.restarts != self.demod.stats.restarts {
                return None;
            }
            let eval = &sym[sym.len().saturating_sub(tail_syms)..];
            let eval = eval.get(TRIAL_SKIP..).unwrap_or(&[]);
            if eval.len() < TRIAL_MIN_SYMBOLS {
                return None;
            }
            let e = eval.iter().map(|&z| decision_error(z)).sum::<f32>() / eval.len() as f32;
            res.push((e, d, sym));
        }
        Some(res)
    }
}

/// ブロックの先頭のサンプル、欠落を埋める内容、2 番目以降のサンプルを end 個目まで並べる
fn with_gap(x: &[Complex32], fill: &[Complex32], end: usize) -> Vec<Complex32> {
    let mut s = Vec::with_capacity(fill.len() + end);
    s.push(x[0]);
    s.extend_from_slice(fill);
    s.extend_from_slice(&x[1..end]);
    s
}
