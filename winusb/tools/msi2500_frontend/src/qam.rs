//! J.83 Annex B (256QAM、5.6 Msps) の復調。設計は winusb/docs/msi2500-j83b-design.md の 4 節を参照。
//!
//! 直流の除去 → ルートロールオフの整合フィルターを兼ねた多相補間 (8.127 MS/s から 2 サンプル/シンボル)
//! → Gardner 型のタイミング追従 → T/2 間隔の等化器 (CMA で収束させ、判定帰還 LMS へ切り替え) → 2 次 PLL。
//! 出力は単位電力のシンボルで、j83b::J83bDecoder へそのまま渡す。90 度単位の位相の不定とスペクトルの反転は
//! 誤り訂正側の同期で解く。

use num_complex::{Complex32, Complex64};
use rustfft::FftPlanner;
#[cfg(target_arch = "x86")]
use std::arch::x86::*;
#[cfg(target_arch = "x86_64")]
use std::arch::x86_64::*;
use std::f64::consts::PI;
use std::sync::Arc;

/// 日本のケーブルテレビの J.83 Annex B (256QAM) のシンボルレート
pub const SYMBOL_RATE: f64 = 5.6e6;
/// 表 B.3 のロールオフ率
pub const ROLLOFF: f64 = 0.12;

/// 整合フィルターの片側の入力サンプル数 (±12 サンプル ≒ ±8.3 シンボル)
const HALF: usize = 12;
const TAPS: usize = 2 * HALF;
/// 補間の位相の分割数。最寄りの位相を使い、時刻の量子化は ±1/512 入力サンプル (±0.0014 シンボル)。
/// 表 (約 25 KB) を L1 キャッシュに収めるため、これ以上は増やしていない
const PHASES: usize = 256;
/// 等化器のタップ数 (T/2 間隔、16 シンボル分)
const NT: usize = 32;
/// CMA で収束させるシンボル数。検証環境の録画では 8 万シンボルで判定帰還へ移れた
const CMA_SYMBOLS: u64 = 80_000;
/// 判定帰還へ切り替える直前の、4 乗法で搬送波を推定する区間 (FFT の長さ)
const FOURTH_N: usize = 32_768;
/// 判定帰還へ切り替えてから出力を始めるまでのシンボル数 (PLL と LMS の追い込み)
const SETTLE: u64 = 5_000;
/// MER の評価区間。区間の MER が RESTART_MER_DB を下回り続けたら CMA からやり直す
const MER_WINDOW: u32 = 20_000;
const RESTART_MER_DB: f32 = 18.0;
const RESTART_WINDOWS: u32 = 3;

const CMA_MU: f32 = 2e-4;
const DD_MU: f32 = 3e-5;
/// Gardner のループの比例と積分の利得 (入力サンプル単位)
const TIMING_KP: f64 = 1e-3;
const TIMING_KI: f64 = 2e-7;
/// 判定帰還 PLL の比例と積分の利得 (pll_scale で倍率を変えられる)
const PLL_KP: f32 = 5e-3;
const PLL_KF: f32 = 2e-6;

/// 256QAM の I、Q のレベルの間隔 (単位電力に正規化、2 / sqrt(170))
const LEVEL_STEP: f32 = 0.153_392_997;

#[derive(Debug, Clone, Default)]
pub struct QamStats {
    pub samples: u64,
    pub symbols: u64,
    /// 出力したシンボル数 (判定帰還へ切り替えて追い込んだ後)
    pub output_symbols: u64,
    /// MER の低下で CMA からやり直した回数
    pub restarts: u64,
    /// 判定帰還で追従しているか
    pub dd: bool,
    /// 直近の評価区間の MER (判定帰還中のみ)
    pub mer_db: f32,
    /// 公称シンボルレートに対するタイミングのずれ (ppm、正は入力のサンプル数で測ったシンボルの周期が長い)
    pub timing_ppm: f64,
    /// 搬送波の残留周波数 (Hz)
    pub carrier_hz: f64,
}

fn rrc(t: f64, a: f64) -> f64 {
    // t はシンボル単位
    if t.abs() < 1e-9 {
        return 1.0 - a + 4.0 * a / PI;
    }
    let q = 1.0 / (4.0 * a);
    if (t.abs() - q).abs() < 1e-9 {
        return a / 2f64.sqrt() * ((1.0 + 2.0 / PI) * (PI / (4.0 * a)).sin() + (1.0 - 2.0 / PI) * (PI / (4.0 * a)).cos());
    }
    ((PI * t * (1.0 - a)).sin() + 4.0 * a * t * (PI * t * (1.0 + a)).cos()) / (PI * t * (1.0 - (4.0 * a * t).powi(2)))
}

// ---------------------------------------------------------------- 積和の内側のループ
// 自動ベクトル化では、スカラーで読み込んで 2 要素ずつ組み合わせる遅い命令列になったため、SSE2 で明示的に書く。
// SSE2 は Rust の x86 と x86_64 の Windows 向けの対象で常に有効。

/// I、Q を交互に並べた 2 × TAPS 個の入力と、実数の係数 TAPS 個の積和
#[inline]
fn fir_interleaved(x: &[f32; 2 * TAPS], h: &[f32; TAPS]) -> Complex32 {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    // SAFETY: 読み込みはすべて配列の範囲内 (x は 8 × (TAPS / 4) 要素、h は 4 × (TAPS / 4) 要素)
    unsafe {
        let (xp, hp) = (x.as_ptr(), h.as_ptr());
        let mut a0 = _mm_setzero_ps();
        let mut a1 = _mm_setzero_ps();
        for c in 0..TAPS / 4 {
            // [h0, h1, h2, h3] を [h0, h0, h1, h1] と [h2, h2, h3, h3] へ複製して I、Q の両方に掛ける
            let hv = _mm_loadu_ps(hp.add(4 * c));
            let x0 = _mm_loadu_ps(xp.add(8 * c));
            let x1 = _mm_loadu_ps(xp.add(8 * c + 4));
            a0 = _mm_add_ps(a0, _mm_mul_ps(x0, _mm_unpacklo_ps(hv, hv)));
            a1 = _mm_add_ps(a1, _mm_mul_ps(x1, _mm_unpackhi_ps(hv, hv)));
        }
        let s = _mm_add_ps(a0, a1);
        let s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        let mut o = [0f32; 4];
        _mm_storeu_ps(o.as_mut_ptr(), s);
        Complex32::new(o[0], o[1])
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    {
        let (mut re, mut im) = (0f32, 0f32);
        for k in 0..TAPS {
            re += x[2 * k] * h[k];
            im += x[2 * k + 1] * h[k];
        }
        Complex32::new(re, im)
    }
}

/// 等化器の履歴 src を 2 要素ずらして dst へ書き、dst の添字 0 に newest、1 に second を入れる。
/// 4 バイトずつ書いた直後に 16 バイトで読むとストアからロードへの転送に失敗して待たされ、
/// 等化器の時間の大半を占めたため、16 バイト単位のシャッフルと書き込みで済ませる
#[inline]
fn shift_into(src: &[f32; NT], dst: &mut [f32; NT], newest: f32, second: f32) {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    // SAFETY: 読み書きはすべて長さ NT (4 の倍数) の配列の範囲内
    unsafe {
        let (s, d) = (src.as_ptr(), dst.as_mut_ptr());
        // 最初のベクトルの前には [-, -, newest, second] があるものとして扱う
        let mut prev = _mm_set_ps(second, newest, 0.0, 0.0);
        for c in 0..NT / 4 {
            let v = _mm_loadu_ps(s.add(4 * c));
            // [prev2, prev3, v0, v1]: 前のベクトルの古い 2 要素を先頭へ送り、v の新しい 2 要素を続ける
            let shifted = _mm_shuffle_ps::<0b0100_1110>(prev, v);
            _mm_storeu_ps(d.add(4 * c), shifted);
            prev = v;
        }
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    {
        dst[2..].copy_from_slice(&src[..NT - 2]);
        dst[0] = newest;
        dst[1] = second;
    }
}

/// 2 面の履歴のうち、cur 面を読み出し用、もう一方を書き込み用として借りる
#[inline]
fn hist_pair(h: &mut [[f32; NT]; 2], cur: usize) -> (&[f32; NT], &mut [f32; NT]) {
    let [a, b] = h;
    if cur == 0 { (a, b) } else { (b, a) }
}

/// 等化器の出力 Σ w[k] r[k] (複素数を実部と虚部の配列で持つ)
#[inline]
fn fse_dot(wr: &[f32; NT], wi: &[f32; NT], rr: &[f32; NT], ri: &[f32; NT]) -> Complex32 {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    // SAFETY: 読み込みはすべて長さ NT (4 の倍数) の配列の範囲内
    unsafe {
        let mut ar = _mm_setzero_ps();
        let mut ai = _mm_setzero_ps();
        for c in 0..NT / 4 {
            let a = _mm_loadu_ps(wr.as_ptr().add(4 * c));
            let b = _mm_loadu_ps(wi.as_ptr().add(4 * c));
            let x = _mm_loadu_ps(rr.as_ptr().add(4 * c));
            let y = _mm_loadu_ps(ri.as_ptr().add(4 * c));
            ar = _mm_add_ps(ar, _mm_sub_ps(_mm_mul_ps(a, x), _mm_mul_ps(b, y)));
            ai = _mm_add_ps(ai, _mm_add_ps(_mm_mul_ps(a, y), _mm_mul_ps(b, x)));
        }
        // [ar0+ar1+ar2+ar3, ai0+ai1+ai2+ai3] を求める
        let lo = _mm_unpacklo_ps(ar, ai); // ar0 ai0 ar1 ai1
        let hi = _mm_unpackhi_ps(ar, ai); // ar2 ai2 ar3 ai3
        let s = _mm_add_ps(lo, hi);
        let s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        let mut o = [0f32; 4];
        _mm_storeu_ps(o.as_mut_ptr(), s);
        Complex32::new(o[0], o[1])
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    {
        let mut y = Complex32::default();
        for k in 0..NT {
            y += Complex32::new(wr[k], wi[k]) * Complex32::new(rr[k], ri[k]);
        }
        y
    }
}

/// 等化器の更新 w -= g conj(r)
#[inline]
fn fse_update(wr: &mut [f32; NT], wi: &mut [f32; NT], rr: &[f32; NT], ri: &[f32; NT], g: Complex32) {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    // SAFETY: 読み書きはすべて長さ NT (4 の倍数) の配列の範囲内
    unsafe {
        let gr = _mm_set1_ps(g.re);
        let gi = _mm_set1_ps(g.im);
        for c in 0..NT / 4 {
            let x = _mm_loadu_ps(rr.as_ptr().add(4 * c));
            let y = _mm_loadu_ps(ri.as_ptr().add(4 * c));
            let pa = wr.as_mut_ptr().add(4 * c);
            let pb = wi.as_mut_ptr().add(4 * c);
            _mm_storeu_ps(pa, _mm_sub_ps(_mm_loadu_ps(pa), _mm_add_ps(_mm_mul_ps(gr, x), _mm_mul_ps(gi, y))));
            _mm_storeu_ps(pb, _mm_sub_ps(_mm_loadu_ps(pb), _mm_sub_ps(_mm_mul_ps(gi, x), _mm_mul_ps(gr, y))));
        }
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    for k in 0..NT {
        wr[k] -= g.re * rr[k] + g.im * ri[k];
        wi[k] -= g.im * rr[k] - g.re * ri[k];
    }
}

#[inline]
fn slice(v: f32) -> f32 {
    // round の代わりに、0.5 を足した値の整数への変換を使う (負の値は 0 へ丸められ、どのみち 0 に制限される)
    let k = ((v * (1.0 / LEVEL_STEP) + 8.0) as i32).clamp(0, 15) as f32;
    (k - 7.5) * LEVEL_STEP
}

/// 復調した単位電力のシンボルと、最寄りの 256QAM の点との距離の 2 乗
pub fn decision_error(z: Complex32) -> f32 {
    (z - Complex32::new(slice(z.re), slice(z.im))).norm_sqr()
}

/// 複製は、PX-S1UD のブロック間の欠落の長さを試行して選ぶ用途 (s1ud.rs) で使う。
/// 補間の係数表は共有し、複製の費用を状態の大きさに抑える
#[derive(Clone)]
pub struct QamDemod {
    fs: f64,
    pll_scale: f32,
    step0: f64,
    /// 位相 p (0..=PHASES) の係数 TAPS 個。table[p][j] = h(j - (HALF - 1) - p / PHASES)
    table: Arc<Vec<[f32; TAPS]>>,
    dc: Complex32,
    dc_init: bool,
    /// 直流を除いた入力 (I、Q を交互に並べる)
    buf: Vec<f32>,
    /// 次の中間点の時刻 (buf の先頭からの入力サンプル数)
    t: f64,
    step: f64,
    y_prev: Complex32,
    /// 1 シンボル前の Gardner の誤差 (次のシンボルで反映する)
    timing_e: f64,
    agc_p: f32,
    agc_n: u64,
    /// 等化器の入力の履歴 (実部と虚部、添字 0 が最新)。2 面を交互に使い、hist_cur 面が最新、
    /// もう一方が 1 シンボル前の履歴 (遅延 LMS の更新に使う)。1 シンボル前の履歴を毎回複製すると、
    /// x86 では複製が memcpy の呼び出しになり、等化器の時間の大半を占めた
    hist_re: [[f32; NT]; 2],
    hist_im: [[f32; NT]; 2],
    hist_cur: usize,
    w_re: [f32; NT],
    w_im: [f32; NT],
    /// 遅延 LMS の、次のシンボルで反映する更新の係数 (入力の履歴は hist の 1 シンボル前の面)
    pend_g: Complex32,
    r2: f32,
    cma_start: u64,
    dd_since: Option<u64>,
    fourth: Vec<Complex32>,
    /// 搬送波の位相の補正 e^{-jφ} (判定帰還 PLL が進める)
    rot: Complex32,
    freq: f32,
    /// 1 シンボル前の位相誤差 (次のシンボルで反映する)
    pll_pe: f32,
    pow_acc: f32,
    pow_n: u32,
    err_acc: f64,
    sig_acc: f64,
    mer_n: u32,
    bad_windows: u32,
    /// push_erased で受けた消失の区間 (buf の先頭からの入力サンプル数、[始め, 終わり))
    erasures: Vec<(f64, f64)>,
    /// 適応を止めておく残りのシンボル数と、直前のシンボルが消失にかかったか
    frozen: u32,
    prev_hit: bool,
    pub stats: QamStats,
}

impl QamDemod {
    /// fs は入力のサンプルレート、pll_scale は判定帰還 PLL の帯域の倍率 (1 が既定)
    pub fn new(fs: f64, pll_scale: f32) -> Self {
        let sps_in = fs / SYMBOL_RATE;
        let mut table = vec![[0f32; TAPS]; PHASES + 1];
        for (p, row) in table.iter_mut().enumerate() {
            let frac = p as f64 / PHASES as f64;
            for (j, c) in row.iter_mut().enumerate() {
                let d = j as f64 - (HALF as f64 - 1.0) - frac;
                *c = rrc(d / sps_in, ROLLOFF) as f32;
            }
        }
        // 256QAM (単位電力) の CMA の半径 E|s|^4 / E|s|^2
        let (mut m2, mut m4) = (0f64, 0f64);
        for a in 0..16 {
            for b in 0..16 {
                let (x, y) = ((a as f64 - 7.5) * LEVEL_STEP as f64, (b as f64 - 7.5) * LEVEL_STEP as f64);
                let p = x * x + y * y;
                m2 += p;
                m4 += p * p;
            }
        }
        let mut s = Self {
            fs,
            pll_scale,
            step0: sps_in / 2.0,
            table: Arc::new(table),
            dc: Complex32::default(),
            dc_init: false,
            buf: Vec::new(),
            t: HALF as f64,
            step: sps_in / 2.0,
            y_prev: Complex32::default(),
            timing_e: 0.0,
            agc_p: 0.0,
            agc_n: 0,
            hist_re: [[0.0; NT]; 2],
            hist_im: [[0.0; NT]; 2],
            hist_cur: 0,
            w_re: [0.0; NT],
            w_im: [0.0; NT],
            pend_g: Complex32::default(),
            r2: (m4 / m2) as f32,
            cma_start: 0,
            dd_since: None,
            fourth: Vec::with_capacity(FOURTH_N),
            rot: Complex32::new(1.0, 0.0),
            freq: 0.0,
            pll_pe: 0.0,
            pow_acc: 0.0,
            pow_n: 0,
            err_acc: 0.0,
            sig_acc: 0.0,
            mer_n: 0,
            bad_windows: 0,
            erasures: Vec::new(),
            frozen: 0,
            prev_hit: false,
            stats: QamStats::default(),
        };
        s.restart_equalizer();
        s
    }

    /// 生ブロックの欠落や選局の変更の後に、すべての状態を作り直す
    pub fn reset(&mut self) {
        let restarts = self.stats.restarts;
        *self = Self::new(self.fs, self.pll_scale);
        self.stats.restarts = restarts;
    }

    fn restart_equalizer(&mut self) {
        self.w_re = [0.0; NT];
        self.w_im = [0.0; NT];
        self.w_re[NT / 2] = 1.0;
        self.pend_g = Complex32::default();
        self.cma_start = self.stats.symbols;
        self.dd_since = None;
        self.stats.dd = false;
        self.fourth.clear();
        self.rot = Complex32::new(1.0, 0.0);
        self.freq = 0.0;
        self.pll_pe = 0.0;
        self.err_acc = 0.0;
        self.sig_acc = 0.0;
        self.mer_n = 0;
        self.bad_windows = 0;
    }

    /// 入力サンプル (任意の尺度) を入れ、復調したシンボル (単位電力) を out へ追加する
    pub fn push(&mut self, x: &[Complex32], out: &mut Vec<Complex32>) {
        self.push_erased(x, 0..0, out);
    }

    /// push と同じだが、x の erased の範囲のサンプルを消失 (受信できず推定や 0 で埋めた値) として扱う。
    /// 整合フィルターの窓が消失にかかるシンボルでは、直流と AGC の追従と Gardner の誤差を使わず、
    /// その後、等化器の履歴から消失が抜けるまで等化器と PLL の適応と MER の評価を止める。
    /// シンボルは出力し続け、誤り訂正から見たシンボルの並びを保つ。
    /// PX-S1UD (s1ud.rs) はブロック間で落ちたサンプルを埋めて渡す。埋めた値で適応すると、
    /// 落ちる長さが一定のときに同じ向きの乱れがブロックごとに積み重なった
    pub fn push_erased(&mut self, x: &[Complex32], erased: std::ops::Range<usize>, out: &mut Vec<Complex32>) {
        if x.is_empty() {
            return;
        }
        self.stats.samples += x.len() as u64;
        if !self.dc_init {
            let n = x.len() as f32;
            self.dc = x.iter().fold(Complex32::default(), |a, &v| a + v) / n;
            self.dc_init = true;
        }
        if erased.start < erased.end {
            let base = (self.buf.len() / 2) as f64;
            self.erasures.push((base + erased.start as f64, base + erased.end.min(x.len()) as f64));
        }
        // 直流の除去 (時定数 約 10 万サンプル = 12 ms)
        const DC_K: f32 = 1e-5;
        self.buf.reserve(2 * x.len());
        for (i, &v) in x.iter().enumerate() {
            if !erased.contains(&i) {
                self.dc += (v - self.dc) * DC_K;
            }
            let u = v - self.dc;
            self.buf.push(u.re);
            self.buf.push(u.im);
        }
        loop {
            // 中間点と次のシンボルの時刻の両方で、整合フィルターの窓が buf に収まるか
            let t_sym = self.t + self.step;
            if t_sym as usize + HALF + 1 >= self.buf.len() / 2 {
                break;
            }
            let m = self.interp(self.t);
            let y = self.interp(t_sym);
            let hit = !self.erasures.is_empty() && (self.erased_at(self.t) || self.erased_at(t_sym));
            let (m, y) = self.agc(m, y, hit);
            // Gardner: 遅れて標本化すると中間点が次のシンボル側へずれ、e が負になる。
            // 誤差は 1 シンボル遅らせて反映する。次の補間の時刻が今の補間の結果を待たなくなり、
            // 連続したシンボルの計算を重ねて実行できる (ループの帯域に対して 1 シンボルの遅れは無視できる)
            // 消失したシンボルと、その直後のシンボル (前のシンボルが消失) では Gardner の誤差を使わない
            let ted = if hit || self.prev_hit { 0.0 } else { ((self.y_prev - y) * m.conj()).re as f64 };
            self.prev_hit = hit;
            let e = std::mem::replace(&mut self.timing_e, ted);
            self.y_prev = y;
            self.step = (self.step + TIMING_KI * e).clamp(self.step0 * 0.999, self.step0 * 1.001);
            self.t = t_sym + self.step + TIMING_KP * e;
            if hit {
                // 等化器の履歴 (NT 個 = NT / 2 シンボル) から消失のサンプルが抜けるまで適応を止める
                self.frozen = (NT / 2) as u32 + 1;
            }
            let freeze = self.frozen > 0;
            self.frozen = self.frozen.saturating_sub(1);
            self.equalize(m, y, freeze, out);
        }
        // 窓の手前に必要な分だけを残す
        let keep_from = (self.t.floor() as usize).saturating_sub(HALF + 1);
        if keep_from > 0 {
            self.buf.drain(..2 * keep_from);
            self.t -= keep_from as f64;
            let k = keep_from as f64;
            self.erasures.retain_mut(|r| {
                r.0 -= k;
                r.1 -= k;
                r.1 > 0.0
            });
        }
        self.stats.timing_ppm = (self.step / self.step0 - 1.0) * 1e6;
        self.stats.carrier_hz = self.freq as f64 * SYMBOL_RATE / (2.0 * PI);
    }

    #[inline]
    fn interp(&self, t: f64) -> Complex32 {
        // t は常に正のため、整数への変換 (切り捨て) を floor の代わりに使う。
        // 配布向けの基準命令 (SSE2) では floor と round がライブラリ関数の呼び出しになり、補間より重かった
        let mut base = t as usize;
        let mut p = ((t - base as f64) * PHASES as f64 + 0.5) as usize;
        if p == PHASES {
            p = 0;
            base += 1;
        }
        let x: &[f32; 2 * TAPS] = self.buf[2 * (base + 1 - HALF)..2 * (base + 1 + HALF)].try_into().unwrap();
        fir_interleaved(x, &self.table[p])
    }

    /// 時刻 t の整合フィルターの窓 (interp と同じ範囲) が消失にかかるか
    #[inline]
    fn erased_at(&self, t: f64) -> bool {
        let (lo, hi) = (t - HALF as f64, t + HALF as f64 + 1.0);
        self.erasures.iter().any(|r| r.0 < hi && r.1 > lo)
    }

    /// 整合フィルター出力の電力を 1 へ揃える (始めの 4096 組は累積平均、以後は時定数 約 6 万組)。
    /// erased のときは電力の推定を更新しない
    #[inline]
    fn agc(&mut self, m: Complex32, y: Complex32, erased: bool) -> (Complex32, Complex32) {
        if erased && self.agc_n == 0 {
            // 電力の推定がまだないときは尺度を変えない (0 の推定から極端な利得を掛けない)
            return (m, y);
        }
        if !erased {
            let p = 0.5 * (m.norm_sqr() + y.norm_sqr());
            self.agc_n += 1;
            let k = if self.agc_n < 4096 { 1.0 / self.agc_n as f32 } else { 1.0 / 65536.0 };
            self.agc_p += (p - self.agc_p) * k;
        }
        let g = 1.0 / self.agc_p.max(1e-20).sqrt();
        (m * g, y * g)
    }

    /// freeze のときは等化器と PLL の適応、MER と出力電力の評価を止め、搬送波は推定済みの周波数で回し続ける
    fn equalize(&mut self, m: Complex32, y: Complex32, freeze: bool, out: &mut Vec<Complex32>) {
        let (prev_re, reg_re) = hist_pair(&mut self.hist_re, self.hist_cur);
        let (prev_im, reg_im) = hist_pair(&mut self.hist_im, self.hist_cur);
        shift_into(prev_re, reg_re, y.re, m.re);
        shift_into(prev_im, reg_im, y.im, m.im);
        let n = self.stats.symbols;
        self.stats.symbols += 1;
        let y0 = fse_dot(&self.w_re, &self.w_im, reg_re, reg_im);
        // 遅延 LMS: 1 シンボル前の更新を、今のシンボルの出力を求めた後に反映する。
        // 出力 → 判定 → 更新 → 次の出力の連鎖が毎シンボル直列になり等化器の時間の大半を占めたため、
        // 連鎖を 2 シンボルにまたがらせて計算を重ねられるようにした。ステップ幅が小さく、1 シンボルの遅れの影響は無視できる
        fse_update(&mut self.w_re, &mut self.w_im, prev_re, prev_im, self.pend_g);
        self.hist_cur ^= 1;
        let rot = self.rot;
        let z = y0 * rot;
        let mu_g;
        match self.dd_since {
            None => {
                // CMA は位相を区別しないため、CMA の間は位相を回さない
                mu_g = if freeze { Complex32::default() } else { z * (z.norm_sqr() - self.r2) * CMA_MU };
                let cma_n = n - self.cma_start;
                if cma_n + FOURTH_N as u64 >= CMA_SYMBOLS {
                    // 添字がシンボルの時刻を表すため、消失の間も 0 を入れて並びを保つ
                    let z2 = z * z;
                    self.fourth.push(if freeze { Complex32::default() } else { z2 * z2 });
                }
                if cma_n + 1 >= CMA_SYMBOLS {
                    self.start_dd(n + 1);
                }
            }
            Some(since) if freeze => {
                mu_g = Complex32::default();
                // 位相誤差は使わず、推定済みの周波数で回転子を進める
                let dphi = self.freq;
                let r = self.rot * Complex32::new(1.0 - 0.5 * dphi * dphi, -dphi);
                self.rot = r * (1.5 - 0.5 * r.norm_sqr());
                if n >= since + SETTLE {
                    out.push(z);
                    self.stats.output_symbols += 1;
                }
            }
            Some(since) => {
                let d = Complex32::new(slice(z.re), slice(z.im));
                let e = z - d;
                // 誤差を等化器の座標 (回転前) へ戻して更新する
                mu_g = e * rot.conj() * DD_MU;
                // 位相誤差は 1 シンボル遅らせて反映する。回転 → 判定 → 位相誤差 → 回転の連鎖を
                // 2 シンボルにまたがらせるためで、PLL の帯域 (1 シンボルあたり 5e-3 程度) に対して遅れは無視できる
                let pe = std::mem::replace(&mut self.pll_pe, (z * d.conj()).im / (d.norm_sqr() + 1e-9));
                self.freq += PLL_KF * self.pll_scale * self.pll_scale * pe;
                // 位相の増分は 1 シンボルあたり 0.01 rad に満たないため、sin と cos を毎回求めず、
                // 2 次までの近似で回転子を進め、振幅を 1 次の補正で 1 に保つ
                let dphi = PLL_KP * self.pll_scale * pe + self.freq;
                let r = self.rot * Complex32::new(1.0 - 0.5 * dphi * dphi, -dphi);
                self.rot = r * (1.5 - 0.5 * r.norm_sqr());
                self.err_acc += e.norm_sqr() as f64;
                self.sig_acc += d.norm_sqr() as f64;
                self.mer_n += 1;
                if self.mer_n == MER_WINDOW {
                    let mer = (10.0 * (self.sig_acc / self.err_acc.max(1e-30)).log10()) as f32;
                    self.stats.mer_db = mer;
                    self.err_acc = 0.0;
                    self.sig_acc = 0.0;
                    self.mer_n = 0;
                    self.bad_windows = if mer < RESTART_MER_DB { self.bad_windows + 1 } else { 0 };
                    if self.bad_windows >= RESTART_WINDOWS {
                        self.stats.restarts += 1;
                        self.restart_equalizer();
                        return;
                    }
                }
                if n >= since + SETTLE {
                    out.push(z);
                    self.stats.output_symbols += 1;
                }
            }
        }
        self.pend_g = mu_g;
        if freeze {
            return;
        }
        // 出力の電力を 1 に保つ
        self.pow_acc += y0.norm_sqr();
        self.pow_n += 1;
        if self.pow_n == 2000 {
            let s = (self.pow_acc / 2000.0).sqrt();
            if s > 1e-12 {
                let k = 1.0 / s;
                for v in self.w_re.iter_mut().chain(self.w_im.iter_mut()) {
                    *v *= k;
                }
                self.pend_g *= k;
            }
            self.pow_acc = 0.0;
            self.pow_n = 0;
        }
    }

    /// CMA の最後の FOURTH_N シンボルの 4 乗から搬送波の周波数と位相を推定し、判定帰還へ移る。
    /// CMA の出力は位相が不定のまま搬送波の残差で回るため、推定しないと 256QAM の格子で位相を取り違え、
    /// MER が約 20 dB で止まることがあった (設計資料 2.3 節)。
    fn start_dd(&mut self, n_next: u64) {
        let n = self.fourth.len();
        if n >= 1024 {
            let mut buf = self.fourth.clone();
            buf.resize(FOURTH_N, Complex32::default());
            FftPlanner::<f32>::new().plan_fft_forward(FOURTH_N).process(&mut buf);
            let mag = |k: usize| buf[k % FOURTH_N].norm();
            let k = (0..FOURTH_N).max_by(|&a, &b| mag(a).total_cmp(&mag(b))).unwrap();
            // 放物線補間で分解能より細かく求める
            let (a, b, c) = (mag(k + FOURTH_N - 1), mag(k), mag(k + 1));
            let den = a - 2.0 * b + c;
            let d = if den.abs() > 1e-20 { 0.5 * (a - c) / den } else { 0.0 };
            let kk = k as f64 + d as f64;
            let kk = if kk > FOURTH_N as f64 / 2.0 { kk - FOURTH_N as f64 } else { kk };
            // 4 倍の周波数 (rad/シンボル)
            let mut w4 = 2.0 * PI * kk / FOURTH_N as f64;
            let fourth = &self.fourth;
            let acc = |r: std::ops::Range<usize>, w4: f64| {
                let mut s = Complex64::default();
                for i in r {
                    let v = fourth[i];
                    s += Complex64::new(v.re as f64, v.im as f64) * Complex64::from_polar(1.0, -w4 * i as f64);
                }
                s
            };
            // 前半と後半の位相差で周波数を補正する
            let half = n / 2;
            let (a1, a2) = (acc(0..half, w4), acc(half..n, w4));
            w4 += (a2 * a1.conj()).arg() / half as f64;
            let a = acc(0..n, w4);
            // 正方形の QAM では E[s^4] が負の実数のため、arg - π が 4 倍の位相 (i = 0 の時点)
            let ph0 = (a.arg() - PI) / 4.0;
            let f = w4 / 4.0;
            self.freq = f as f32;
            self.rot = Complex32::from_polar(1.0, -((ph0 + f * n as f64).rem_euclid(2.0 * PI)) as f32);
        }
        self.fourth.clear();
        self.dd_since = Some(n_next);
        self.stats.dd = true;
    }
}
