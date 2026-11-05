//! ISDB-T フルセグ (Mode 3, GI 1/8) のストリーム OFDM 復調。
//!
//! 初期同期は試作 (demod_mer.py) と同じ手順: CP 相関でシンボル位置と小数部 CFO、
//! SP 電力と PRBS 符号相関で整数部 CFO と SP 位相、SP の4シンボル間位相差で標本化ずれと残留 CFO。
//! 追従では、32 シンボルごとに同じ位相差と伝送路の位相傾きから、窓の間隔・CFO・窓位置を補正する。
//! 伝送路は SP を時間方向 (4 シンボル間隔) に線形補間し、その後 3 キャリア間隔で周波数方向に補間する。
//! 時間補間のため、出力は入力より 3 シンボル遅れる。

use num_complex::{Complex32, Complex64};
use rustfft::{Fft, FftPlanner};
use std::collections::VecDeque;
use std::sync::Arc;

pub const FS: f64 = 512e6 / 63.0;
pub const N: usize = 8192;
pub const G: usize = 1024;
pub const L: usize = N + G;
pub const NC: usize = 5617;
pub const KC: usize = 2808;
pub const NDC: usize = 384;
pub const SEG_ORDER: [usize; 13] = [11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12];
const TMCC_K: [usize; 4] = [2693, 2723, 2878, 2941];
const SYNC_EVEN: [u8; 16] = [0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0];
const NCOL: usize = (NC - 1) / 3 + 1; // SP 列 (k = 3j) の数 = 1873
const TRACK_PERIOD: u64 = 32;
const CTRL_SYMBOLS: u64 = 256;

/// SP の PRBS 符号 (w=1 → -1)。isdbt-dsp pilots.rs と同一の系列。
pub fn prbs_signs() -> Vec<f32> {
    let mut reg: u32 = (1 << 11) - 1;
    (0..NC)
        .map(|_| {
            let w = reg & 1;
            let nb = ((reg >> 2) ^ reg) & 1;
            reg = (reg >> 1) | (nb << 10);
            1.0 - 2.0 * w as f32
        })
        .collect()
}

#[derive(Clone, Copy, Debug, Default)]
pub struct LayerParam {
    pub modulation: u8, // 0 DQPSK, 1 QPSK, 2 16QAM, 3 64QAM, 7 未使用
    pub rate: u8,       // 0..4 = 1/2, 2/3, 3/4, 5/6, 7/8
    pub interleave: u8, // Mode 3: 0,1,2,3 → I = 0,1,2,4
    pub nseg: u8,
}

impl LayerParam {
    pub fn mbits(&self) -> usize {
        match self.modulation { 1 => 2, 2 => 4, 3 => 6, _ => 0 }
    }
    pub fn idepth(&self) -> usize {
        match self.interleave { 0 => 0, 1 => 1, 2 => 2, 3 => 4, _ => 0 }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Tmcc {
    pub partial: bool,
    pub layers: [LayerParam; 3],
}

impl Tmcc {
    /// 論理セグメント番号 → 階層 (0=A, 1=B, 2=C)
    pub fn layer_of_segment(&self, s: usize) -> Option<usize> {
        let mut base = 0usize;
        for (i, l) in self.layers.iter().enumerate() {
            if l.nseg == 0 || l.nseg > 13 { continue; }
            if s < base + l.nseg as usize { return Some(i); }
            base += l.nseg as usize;
        }
        None
    }
}

/// 1 OFDM シンボル分の出力。データキャリアは論理セグメント番号順に 384 本ずつ、各セグメント内は周波数順。
pub struct SymbolOut {
    pub index: u64,
    pub z: Vec<Complex32>,
    pub w: Vec<f32>,
    pub tmcc: Option<Tmcc>,
    /// OFDM フレームの先頭 (TMCC の B0) を 0 とした通し番号。/204 がフレーム番号、%204 がフレーム内の位置。
    /// TMCC の同期語を確認するまでは None
    pub frame_symbol: Option<u64>,
}

#[derive(Clone, Debug, Default)]
pub struct Stats {
    pub acquired: bool,
    pub symbols_out: u64,
    pub integer_shift: i32,
    pub cfo_hz: f64,
    pub drift_ppm: f64,
    pub last_timing_residual: f64,
    pub last_cfo_residual: f64,
    pub last_pos_error: f64,
    pub mer_db: [f32; 3],
    pub tmcc: Option<Tmcc>,
    pub ctrl_matches_center: Option<bool>,
}

enum State {
    Acquire,
    Track,
}

pub struct Ofdm {
    fft: Arc<dyn Fft<f32>>,
    pr: Vec<f32>,
    buf: Vec<Complex32>,
    base: u64,
    state: State,
    dc: Complex32,
    // 追従状態
    pos: f64,        // 次のシンボルの CP 先頭 (絶対サンプル位置)
    delta: f64,      // シンボル間隔の L からの差 (サンプル)
    delta_extra: f64,
    f_hz: f64,
    nco_ref: f64,
    nco_phase: f64,
    sh: i32,
    q: usize,
    n: u64, // 追従開始からの FFT 済みシンボル数
    ring: VecDeque<(u64, Vec<Complex32>)>,
    col_prev: Vec<(i64, Complex32)>,
    col_last: Vec<(i64, Complex32)>,
    acc: Vec<Complex32>,
    slope_acc: Complex64,
    // 出力側
    prev_tmcc: [Complex32; 4],
    tmcc_bits: Vec<u8>,
    tmcc: Option<Tmcc>,
    ctrl_num: Vec<Complex32>,
    ctrl_den: Vec<f32>,
    data_idx: Option<Vec<[Vec<usize>; 4]>>, // [位置 pos][位相] → 384 本の絶対キャリア番号
    err_ema: Vec<f32>,
    layer_pow: [f32; 3],
    med_err: [f32; 3],
    out_count: u64,
    tmcc_at: Option<u64>,
    /// tmcc_bits の先頭から捨てたビット数 (tmcc_bits[i] は out_count が tmcc_drained + i のシンボル)
    tmcc_drained: u64,
    /// フレームの先頭に当たるシンボルの out_count
    frame_origin: Option<u64>,
    err_alpha: f32,
    pub stats: Stats,
    scratch: Vec<Complex32>,
}

impl Ofdm {
    pub fn new() -> Self {
        let mut planner = FftPlanner::new();
        Self {
            fft: planner.plan_fft_forward(N),
            pr: prbs_signs(),
            buf: Vec::new(),
            base: 0,
            state: State::Acquire,
            dc: Complex32::new(0.0, 0.0),
            pos: 0.0,
            delta: 0.0,
            delta_extra: 0.0,
            f_hz: 0.0,
            nco_ref: 0.0,
            nco_phase: 0.0,
            sh: 0,
            q: 0,
            n: 0,
            ring: VecDeque::new(),
            col_prev: vec![(-100, Complex32::new(0.0, 0.0)); NCOL],
            col_last: vec![(-100, Complex32::new(0.0, 0.0)); NCOL],
            acc: vec![Complex32::new(0.0, 0.0); NC],
            slope_acc: Complex64::new(0.0, 0.0),
            prev_tmcc: [Complex32::new(0.0, 0.0); 4],
            tmcc_bits: Vec::new(),
            tmcc: None,
            ctrl_num: vec![Complex32::new(0.0, 0.0); NC],
            ctrl_den: vec![0.0; NC],
            data_idx: None,
            err_ema: vec![0.0; NC],
            layer_pow: [0.0; 3],
            med_err: [1.0; 3],
            out_count: 0,
            tmcc_at: None,
            tmcc_drained: 0,
            frame_origin: None,
            err_alpha: 0.02,
            stats: Stats::default(),
            scratch: vec![Complex32::new(0.0, 0.0); N],
        }
    }

    fn sp_phase(&self, n: u64) -> usize {
        ((n + self.q as u64) % 4) as usize
    }

    pub fn push(&mut self, x: &[Complex32]) -> Vec<SymbolOut> {
        self.buf.extend_from_slice(x);
        let mut out = Vec::new();
        if let State::Acquire = self.state {
            if self.buf.len() < 212 * L + 2 * N {
                return out;
            }
            self.acquire();
        }
        loop {
            let start = self.pos.round();
            let need = (start as i64 + (G / 2 + N) as i64) - self.base as i64;
            if need > self.buf.len() as i64 {
                break;
            }
            if let Some(s) = self.symbol() {
                out.push(s);
            }
        }
        // 使い終わったサンプルを捨てる (次の窓より前)
        let keep_from = (self.pos.round() as i64 - 4 * L as i64).max(self.base as i64) as u64;
        if keep_from > self.base + 4 * 1024 * 1024 {
            let d = (keep_from - self.base) as usize;
            self.buf.drain(0..d);
            self.base = keep_from;
        }
        out
    }

    /// 初期同期。self.buf の先頭から 212 シンボル余りを使う。
    fn acquire(&mut self) {
        let n0 = self.buf.len();
        let mut dc = Complex64::new(0.0, 0.0);
        for v in &self.buf { dc += Complex64::new(v.re as f64, v.im as f64); }
        dc /= n0 as f64;
        self.dc = Complex32::new(dc.re as f32, dc.im as f32);
        let x: Vec<Complex32> = self.buf.iter().map(|v| v - self.dc).collect();
        // 1. CP 相関
        let span = 100 * L;
        let mut fold = vec![Complex64::new(0.0, 0.0); L];
        let mut run = Complex64::new(0.0, 0.0);
        let prod = |i: usize| -> Complex64 {
            let p = x[i] * x[i + N].conj();
            Complex64::new(p.re as f64, p.im as f64)
        };
        for i in 0..G { run += prod(i); }
        for t in 0..span {
            fold[t % L] += run;
            run += prod(t + G) - prod(t);
        }
        let (t0, _) = fold.iter().enumerate().fold((0usize, 0f64), |b, (i, v)| if v.norm() > b.1 { (i, v.norm()) } else { b });
        let cfo_frac = -fold[t0].arg() * FS / (2.0 * std::f64::consts::PI * N as f64);
        // 2. 200 シンボルを FFT
        let nsym = 200usize;
        let w0 = t0 + G / 2;
        let mut yf: Vec<Vec<Complex32>> = Vec::with_capacity(nsym);
        for n in 0..nsym {
            let s = w0 + n * L;
            let mut v: Vec<Complex32> = (0..N).map(|i| {
                let ph = -2.0 * std::f64::consts::PI * cfo_frac * (s + i) as f64 / FS;
                x[s + i] * Complex32::new(ph.cos() as f32, ph.sin() as f32)
            }).collect();
            self.fft.process(&mut v);
            yf.push(v);
        }
        // 3. 整数部 CFO と SP 位相: 電力比が最大に近い候補を PRBS 相関で選ぶ
        let mut cands = Vec::new();
        let mut best_r = 0f64;
        for sh in -30i32..=30 {
            for q in 0..4usize {
                let (mut ps, mut pa, mut ns, mut na) = (0f64, 0f64, 0usize, 0usize);
                for (n, y) in yf.iter().enumerate() {
                    let ph = (n + q) % 4;
                    for k in 0..NC {
                        let b = ((k as i64 - KC as i64 + sh as i64).rem_euclid(N as i64)) as usize;
                        let p = y[b].norm_sqr() as f64;
                        pa += p; na += 1;
                        if k % 12 == 3 * ph { ps += p; ns += 1; }
                    }
                }
                let r = (ps / ns as f64) / (pa / na as f64);
                best_r = best_r.max(r);
                cands.push((r, sh, q));
            }
        }
        let mut best: Option<(f64, i32, usize)> = None;
        for &(r, sh, q) in &cands {
            if r < best_r * 0.95 { continue; }
            let mut c = Complex64::new(0.0, 0.0);
            for (n, y) in yf.iter().enumerate() {
                let ph = (n + q) % 4;
                let mut k = 3 * ph;
                while k + 12 < NC {
                    let b0 = ((k as i64 - KC as i64 + sh as i64).rem_euclid(N as i64)) as usize;
                    let b1 = ((k as i64 + 12 - KC as i64 + sh as i64).rem_euclid(N as i64)) as usize;
                    let v = (y[b1] * self.pr[k + 12]) * (y[b0] * self.pr[k]).conj();
                    c += Complex64::new(v.re as f64, v.im as f64);
                    k += 12;
                }
            }
            if best.map_or(true, |b| c.norm() > b.0) { best = Some((c.norm(), sh, q)); }
        }
        let (_, sh, q) = best.unwrap();
        self.sh = sh;
        self.q = q;
        // 4. SP の 4 シンボル間位相差 (符号は打ち消し合う) から窓のずれの傾きと残留 CFO
        let mut acc = vec![Complex32::new(0.0, 0.0); NC];
        for n in 0..nsym - 4 {
            let ph = (n + q) % 4;
            let mut k = 3 * ph;
            while k < NC {
                let b = ((k as i64 - KC as i64 + sh as i64).rem_euclid(N as i64)) as usize;
                acc[k] += yf[n + 4][b] * yf[n][b].conj();
                k += 12;
            }
        }
        let (a, c) = fit_phase(&acc, sh);
        let d4 = a * N as f64 / (2.0 * std::f64::consts::PI);
        let f_res = c / (2.0 * std::f64::consts::PI * 4.0 * L as f64 / FS);
        // 窓の遅れが 4 シンボルで d4 増える → 実際のシンボル間隔は L - d4/4
        self.delta = -d4 / 4.0;
        self.f_hz = cfo_frac + f_res;
        self.pos = self.base as f64 + t0 as f64;
        self.nco_ref = self.pos;
        self.nco_phase = 0.0;
        self.n = 0;
        self.state = State::Track;
        self.stats.acquired = true;
        self.stats.integer_shift = sh;
        self.stats.cfo_hz = self.f_hz;
        self.stats.drift_ppm = -self.delta / L as f64 * 1e6;
        eprintln!("[ofdm] acquired: cp_start={t0} cfo_frac={cfo_frac:+.1}Hz integer_shift={sh} sp_phase={q} d4={d4:+.4} residual_cfo={f_res:+.2}Hz drift={:+.3}ppm",
                  self.stats.drift_ppm);
    }

    /// 1 シンボルを FFT し、3 シンボル前のシンボルを等化して返す。
    fn symbol(&mut self) -> Option<SymbolOut> {
        let start = self.pos.round();
        let frac = self.pos - start;
        let ws = start as i64 + (G / 2) as i64;
        let off = (ws - self.base as i64) as usize;
        // NCO の位相を窓の先頭まで進める (周波数を変えても位相は連続)
        let step = -2.0 * std::f64::consts::PI * self.f_hz / FS;
        let ph0 = self.nco_phase + step * (ws as f64 - self.nco_ref);
        let ph0 = ph0.rem_euclid(2.0 * std::f64::consts::PI);
        self.nco_phase = ph0;
        self.nco_ref = ws as f64;
        let mut rot = Complex64::new(ph0.cos(), ph0.sin());
        let inc = Complex64::new(step.cos(), step.sin());
        let mut mean = Complex32::new(0.0, 0.0);
        for i in 0..N {
            let v = self.buf[off + i];
            mean += v;
            let r = Complex32::new(rot.re as f32, rot.im as f32);
            self.scratch[i] = (v - self.dc) * r;
            rot *= inc;
        }
        {
            self.dc = self.dc * 0.98 + (mean / N as f32) * 0.02;
        }
        self.fft.process(&mut self.scratch);
        // 有効キャリアの取り出しと、窓の前倒し (G/2 + frac) による直線位相の補償
        let adv = (G / 2) as f64 + frac;
        let mut y = vec![Complex32::new(0.0, 0.0); NC];
        let dphi = 2.0 * std::f64::consts::PI * adv / N as f64;
        let b0 = -(KC as i64) + self.sh as i64;
        let mut r = Complex64::from_polar(1.0, dphi * b0 as f64);
        let ri = Complex64::from_polar(1.0, dphi);
        for k in 0..NC {
            let b = ((k as i64 + b0).rem_euclid(N as i64)) as usize;
            y[k] = self.scratch[b] * Complex32::new(r.re as f32, r.im as f32);
            r *= ri;
        }
        let n = self.n;
        let ph = self.sp_phase(n);
        // 追従用: 4 シンボル前との SP 位相差
        if let Some((_, y4)) = self.ring.iter().find(|(m, _)| *m + 4 == n) {
            let mut k = 3 * ph;
            while k < NC {
                self.acc[k] += y[k] * y4[k].conj();
                k += 12;
            }
        }
        // SP 列の観測を更新 (PRBS の符号を外して伝送路にする)
        let mut k = 3 * ph;
        while k < NC {
            let j = k / 3;
            self.col_prev[j] = self.col_last[j];
            self.col_last[j] = (n as i64, y[k] * self.pr[k]);
            k += 12;
        }
        self.ring.push_back((n, y));
        if self.ring.len() > 8 { self.ring.pop_front(); }
        self.n += 1;
        self.pos += L as f64 + self.delta + self.delta_extra;
        if self.n % TRACK_PERIOD == 0 && self.n >= 8 {
            self.track_update();
        }
        if n < 7 {
            return None;
        }
        self.equalize(n - 3)
    }

    fn track_update(&mut self) {
        let (a, c) = fit_phase(&self.acc, self.sh);
        let d4 = a * N as f64 / (2.0 * std::f64::consts::PI);
        let f_res = c / (2.0 * std::f64::consts::PI * 4.0 * L as f64 / FS);
        let g = 0.5;
        // 測った遅れの増分には、窓位置を戻すために意図的に加えた delta_extra の分が含まれるため除く
        self.delta -= g * (d4 / 4.0 - self.delta_extra);
        self.f_hz += g * f_res;
        for v in self.acc.iter_mut() { *v = Complex32::new(0.0, 0.0); }
        // 伝送路の位相傾きから窓位置の誤差 (遅れ) を求め、次の周期で少しずつ戻す
        let s = self.slope_acc.arg() / 3.0;
        let tau = s * N as f64 / (2.0 * std::f64::consts::PI);
        self.slope_acc = Complex64::new(0.0, 0.0);
        self.delta_extra = -0.3 * tau / TRACK_PERIOD as f64;
        self.stats.cfo_hz = self.f_hz;
        self.stats.drift_ppm = -self.delta / L as f64 * 1e6;
        self.stats.last_timing_residual = d4;
        self.stats.last_cfo_residual = f_res;
        self.stats.last_pos_error = tau;
    }

    fn equalize(&mut self, m: u64) -> Option<SymbolOut> {
        let y = &self.ring.iter().find(|(i, _)| *i == m)?.1;
        let mi = m as i64;
        let mut hc = vec![Complex32::new(0.0, 0.0); NCOL];
        for j in 0..NCOL {
            let (np, vp) = self.col_prev[j];
            let (nl, vl) = self.col_last[j];
            hc[j] = if nl == mi { vl } else if np <= mi && mi < nl {
                vp + (vl - vp) * ((mi - np) as f32 / (nl - np) as f32)
            } else {
                vl
            };
        }
        let mut sl = Complex64::new(0.0, 0.0);
        for j in 0..NCOL - 1 {
            let v = hc[j + 1] * hc[j].conj();
            sl += Complex64::new(v.re as f64, v.im as f64);
        }
        self.slope_acc += sl;
        let mut z = vec![Complex32::new(0.0, 0.0); NC];
        for k in 0..NC {
            let j = k / 3;
            let h = if j + 1 < NCOL {
                let t = (k - 3 * j) as f32 / 3.0;
                hc[j] + (hc[j + 1] - hc[j]) * t
            } else {
                hc[j]
            };
            z[k] = y[k] / h;
        }
        let ph = self.sp_phase(m);
        // TMCC (中央セグメントの 4 本の DBPSK を多数決)
        let mut accb = 0f32;
        for (i, &k) in TMCC_K.iter().enumerate() {
            accb += (z[k] * self.prev_tmcc[i].conj()).re;
            self.prev_tmcc[i] = z[k];
        }
        self.tmcc_bits.push(u8::from(accb < 0.0));
        if self.tmcc.is_none() { self.try_tmcc(); }
        self.out_count += 1;
        // 制御キャリア (TMCC 4 + AC 8 /セグメント) の検出
        if self.data_idx.is_none() {
            for k in 0..NC {
                if k % 12 == 3 * ph { continue; }
                self.ctrl_num[k] += z[k] * z[k];
                self.ctrl_den[k] += z[k].norm_sqr();
            }
            if self.out_count >= CTRL_SYMBOLS {
                self.build_data_idx();
            }
            return None;
        }
        let di = self.data_idx.as_ref().unwrap();
        let mut zo = vec![Complex32::new(0.0, 0.0); 13 * NDC];
        let mut wo = vec![1f32; 13 * NDC];
        let tm = self.tmcc;
        for pos in 0..13 {
            let s = SEG_ORDER[pos];
            for (i, &k) in di[pos][ph].iter().enumerate() {
                zo[s * NDC + i] = z[k];
            }
        }
        // 信頼度: キャリアごとの判定誤差分散の指数平均と、階層ごとの中央値の比
        if let Some(t) = tm {
            // 階層の平均電力は、このシンボルの階層全体の平均で更新してから使う。
            // 1 サンプルの電力で初期化すると起動直後の正規化が大きく狂い、誤差統計と重みを数秒間汚す
            let mut psum = [0f64; 3];
            let mut pcnt = [0usize; 3];
            for pos in 0..13 {
                let s = SEG_ORDER[pos];
                let Some(li) = t.layer_of_segment(s) else { continue };
                for i in 0..NDC {
                    psum[li] += zo[s * NDC + i].norm_sqr() as f64;
                }
                pcnt[li] += NDC;
            }
            for li in 0..3 {
                if pcnt[li] == 0 { continue; }
                let m = (psum[li] / pcnt[li] as f64) as f32;
                self.layer_pow[li] = if self.layer_pow[li] == 0.0 { m } else { self.layer_pow[li] * 0.99 + m * 0.01 };
            }
            for pos in 0..13 {
                let s = SEG_ORDER[pos];
                let Some(li) = t.layer_of_segment(s) else { continue };
                let mb = t.layers[li].mbits();
                if mb == 0 { continue; }
                let target = if mb == 6 { 42.0 } else if mb == 4 { 10.0 } else { 2.0 };
                let p = self.layer_pow[li].max(1e-12);
                let sc = (target / p).sqrt();
                let lim = (1i32 << (mb / 2)) - 1; // 64QAM: ±7, 16QAM: ±3, QPSK: ±1
                for (i, &k) in di[pos][ph].iter().enumerate() {
                    let v = zo[s * NDC + i];
                    let x = v * sc;
                    let qd = |a: f32| -> f32 { (((a + lim as f32) / 2.0).round() * 2.0 - lim as f32).clamp(-(lim as f32), lim as f32) };
                    let e = (x.re - qd(x.re)).powi(2) + (x.im - qd(x.im)).powi(2);
                    let al = self.err_alpha;
                    self.err_ema[k] = if self.err_ema[k] == 0.0 { e } else { self.err_ema[k] * (1.0 - al) + e * al };
                    wo[s * NDC + i] = (self.med_err[li] / self.err_ema[k].max(1e-6)).clamp(0.0, 2.0);
                }
            }
            if self.out_count % 64 == 0 { self.update_layer_stats(&t); }
            // 誤差統計が揃う前の重みは不正確で、深い時間インターリーブを通して長い範囲に影響するため、
            // TMCC の確定から 128 シンボルは誤り訂正へ渡さない
            let since = self.out_count - *self.tmcc_at.get_or_insert(self.out_count);
            if since < 128 {
                if since == 127 { self.update_layer_stats(&t); }
                return None;
            }
        }
        self.stats.symbols_out = self.out_count;
        // この関数の冒頭で out_count を進めたため、このシンボルの番号は out_count - 1
        let frame_symbol = self.frame_origin.map(|o| self.out_count - 1 - o);
        Some(SymbolOut { index: m, z: zo, w: wo, tmcc: tm, frame_symbol })
    }

    fn update_layer_stats(&mut self, t: &Tmcc) {
        let di = self.data_idx.as_ref().unwrap();
        for li in 0..3 {
            let mb = t.layers[li].mbits();
            if mb == 0 { continue; }
            let mut v: Vec<f32> = Vec::new();
            for pos in 0..13 {
                if t.layer_of_segment(SEG_ORDER[pos]) != Some(li) { continue; }
                for ph in 0..4 { for &k in &di[pos][ph] { v.push(self.err_ema[k]); } }
            }
            if v.is_empty() { continue; }
            let mean = v.iter().sum::<f32>() / v.len() as f32;
            let mid = v.len() / 2;
            v.select_nth_unstable_by(mid, |a, b| a.partial_cmp(b).unwrap());
            self.med_err[li] = v[mid].max(1e-6);
            let target = if mb == 6 { 42.0 } else if mb == 4 { 10.0 } else { 2.0 };
            self.stats.mer_db[li] = 10.0 * (target / mean.max(1e-9)).log10();
        }
    }

    fn try_tmcc(&mut self) {
        let b = &self.tmcc_bits;
        if b.len() < 2 * 204 + 20 { return; }
        // 直近の 2 フレームで同期語が揃う位相を探す (偶奇は交互)
        let end = b.len();
        for p in (end - 2 * 204 - 17)..(end - 204 - 17) {
            let m = |st: usize| -> (usize, usize) {
                let w = &b[st + 1..st + 17];
                let me = w.iter().zip(SYNC_EVEN.iter()).filter(|(x, y)| x == y).count();
                (me, 16 - me)
            };
            let (e1, o1) = m(p);
            let (e2, o2) = m(p + 204);
            let ok = (e1 >= 15 && o2 >= 15) || (o1 >= 15 && e2 >= 15);
            if !ok { continue; }
            let fr = &b[p..p + 204];
            let be = |s: usize, n: usize| -> u8 { (0..n).fold(0u8, |v, i| (v << 1) | fr[s + i]) };
            let lay = |base: usize| LayerParam { modulation: be(base, 3), rate: be(base + 3, 3), interleave: be(base + 6, 3), nseg: be(base + 9, 4) };
            let t = Tmcc { partial: fr[27] == 1, layers: [lay(28), lay(41), lay(54)] };
            eprintln!("[ofdm] TMCC: partial={} A={:?} B={:?} C={:?}", t.partial, t.layers[0], t.layers[1], t.layers[2]);
            self.tmcc = Some(t);
            self.stats.tmcc = Some(t);
            // p は B0 (差動復調の基準) のビットで、フレームの先頭のシンボルに当たる
            self.frame_origin = Some(self.tmcc_drained + p as u64);
            return;
        }
        if b.len() > 4096 {
            self.tmcc_bits.drain(0..2048);
            self.tmcc_drained += 2048;
        }
    }

    fn build_data_idx(&mut self) {
        let mut out: Vec<[Vec<usize>; 4]> = Vec::with_capacity(13);
        let mut center_ok = None;
        for pos in 0..13 {
            let mut sc: Vec<(f32, usize)> = (0..432).map(|l| {
                let k = pos * 432 + l;
                (self.ctrl_num[k].norm() / self.ctrl_den[k].max(1e-12), l)
            }).collect();
            sc.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap());
            let mut ctrl: Vec<usize> = sc[..12].iter().map(|v| v.1).collect();
            ctrl.sort();
            if SEG_ORDER[pos] == 0 {
                let exp = [7, 89, 101, 131, 206, 209, 226, 244, 286, 349, 377, 407];
                center_ok = Some(ctrl == exp);
            }
            let mut per: [Vec<usize>; 4] = Default::default();
            for ph in 0..4 {
                per[ph] = (0..432).filter(|&l| l % 12 != 3 * ph && !ctrl.contains(&l)).map(|l| pos * 432 + l).collect();
                if per[ph].len() != NDC {
                    eprintln!("[ofdm] control carrier detection failed at segment pos {pos} phase {ph}: {} data carriers", per[ph].len());
                }
            }
            out.push(per);
        }
        eprintln!("[ofdm] control carriers detected; center segment matches known table: {:?}", center_ok);
        self.stats.ctrl_matches_center = center_ok;
        self.data_idx = Some(out);
    }
}

/// SP の位相差の累積 acc[k] から、位相 = a·b + c の重み付き直線近似 (b はビン番号) を求める。
fn fit_phase(acc: &[Complex32], sh: i32) -> (f64, f64) {
    let mut pts: Vec<(f64, f64, f64)> = Vec::new();
    let mut prev: Option<f64> = None;
    for (k, v) in acc.iter().enumerate() {
        let w = v.norm() as f64;
        if w == 0.0 { continue; }
        let mut ph = v.arg() as f64;
        if let Some(p) = prev {
            while ph - p > std::f64::consts::PI { ph -= 2.0 * std::f64::consts::PI; }
            while ph - p < -std::f64::consts::PI { ph += 2.0 * std::f64::consts::PI; }
        }
        prev = Some(ph);
        let b = k as f64 - KC as f64 + sh as f64;
        pts.push((b, ph, w));
    }
    let (mut sw, mut sx, mut sy, mut sxx, mut sxy) = (0.0, 0.0, 0.0, 0.0, 0.0);
    for &(x, y, w) in &pts {
        sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
    }
    let den = sw * sxx - sx * sx;
    if den.abs() < 1e-12 { return (0.0, 0.0); }
    let a = (sw * sxy - sx * sy) / den;
    let c = (sy - a * sx) / sw;
    let c = (c + std::f64::consts::PI).rem_euclid(2.0 * std::f64::consts::PI) - std::f64::consts::PI;
    (a, c)
}
