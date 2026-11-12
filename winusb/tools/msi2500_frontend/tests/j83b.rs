//! J.83 Annex B (256QAM) の誤り訂正の試験。
//!
//! 送信側 (j83b_tx) で符号化したシンボルに回転・スペクトルの反転・雑音を加えて受信側へ流し、TS が一致することを確かめる。
//! 録画 IQ はリポジトリに含めないため、録画を使う試験は環境変数 MSI2500_TEST_J83B に capture の生ブロックファイル
//! (J.83 Annex B 256QAM の局) を指定したときだけ実行する。MSI2500_TEST_J83B_TSID を指定すると PAT の TSID も照合する。
//! 仕様の解釈そのもの (配置、ランダム化の出力位置、チェックサム) は、検証環境の実信号で確かめた結果に基づく
//! (winusb/docs/msi2500-j83b-design.md の 2.4 節)。

use msi2500_frontend::j83b::{
    checksum, gf, rs_check, rs_decode, rs_encode, rs_generator, J83bDecoder, LEVEL_SCALE, MAP, ORIENT_BLOCKS, RS_K, RS_N,
};
use msi2500_frontend::blocks::{BlockCounter, BlockState};
use msi2500_frontend::j83b_tx::J83bTx;
use msi2500_frontend::qam::{QamDemod, ROLLOFF, SYMBOL_RATE};
use msi2500_frontend::raw384::decode_block;
use num_complex::Complex32;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn byte(&mut self) -> u8 {
        (self.next() >> 24) as u8
    }
    fn gauss(&mut self) -> f32 {
        let u1 = ((self.next() >> 11) as f64 + 0.5) / (1u64 << 53) as f64;
        let u2 = ((self.next() >> 11) as f64 + 0.5) / (1u64 << 53) as f64;
        ((-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()) as f32
    }
}

#[test]
fn constellation_map_is_bijective_with_coded_bits_as_level_parity() {
    let mut seen = [false; 256];
    for (ii, row) in MAP.iter().enumerate() {
        for (qi, &c) in row.iter().enumerate() {
            assert!(!seen[c as usize], "duplicate 0x{c:02x}");
            seen[c as usize] = true;
            assert_eq!((c >> 4) & 1, (ii & 1) as u8, "C4 is the parity of the I index");
            assert_eq!(c & 1, (qi & 1) as u8, "C0 is the parity of the Q index");
        }
    }
}

#[test]
fn rs_generator_matches_specification() {
    // B.5.1: g(x) = x^5 + α^52 x^4 + α^116 x^3 + α^119 x^2 + α^61 x + α^15
    // 仕様の係数は α のべき指数で書かれているため、α^e へ変換して比べる
    let a = |e| gf().alpha(e);
    assert_eq!(rs_generator(), [1, a(52), a(116), a(119), a(61), a(15)]);
}

#[test]
fn rs_corrects_up_to_three_symbols() {
    let mut rng = Rng(0x1234_5678_9abc_def1);
    for trial in 0..300 {
        let msg: Vec<u8> = (0..RS_K).map(|_| rng.byte() & 0x7f).collect();
        let cw = rs_encode(&msg);
        assert!(rs_check(&cw));
        let nerr = trial % 4;
        let mut rx = cw;
        let mut pos = Vec::new();
        while pos.len() < nerr {
            let p = (rng.next() % RS_N as u64) as usize;
            if !pos.contains(&p) {
                pos.push(p);
            }
        }
        // 拡張シンボル (位置 127) の誤りも一定の割合で含める
        if trial % 7 == 0 && nerr > 0 && !pos.contains(&127) {
            pos[0] = 127;
        }
        for &p in &pos {
            rx[p] ^= ((rng.next() % 127) + 1) as u8;
        }
        let r = rs_decode(&mut rx);
        assert_eq!(r, Some(nerr), "trial {trial} errors at {pos:?}");
        assert_eq!(rx, cw);
    }
}

#[test]
fn checksum_marks_valid_packets_with_0x47() {
    let cs = checksum();
    let mut rng = Rng(0xfeed_beef_1234_5678);
    for _ in 0..50 {
        let payload: Vec<u8> = (0..187).map(|_| rng.byte()).collect();
        let c = cs.encode(&payload);
        let mut w = payload.clone();
        w.push(c);
        assert_eq!(cs.syndrome(&w), 0x47);
        w[(rng.next() % 187) as usize] ^= 1 << (rng.next() % 8);
        assert_ne!(cs.syndrome(&w), 0x47, "a single bit error must be detected");
    }
}

/// 連続カウンター付きの TS パケット列 (PID 0x100、ペイロードは乱数)
fn make_ts(n: usize, rng: &mut Rng) -> Vec<u8> {
    let mut ts = Vec::with_capacity(n * 188);
    for k in 0..n {
        ts.extend_from_slice(&[0x47, 0x01, 0x00, 0x10 | (k as u8 & 15)]);
        ts.extend((0..184).map(|_| rng.byte()));
    }
    ts
}

/// 受信側の出力が、送信した TS の連続した一部と一致し、TEI が立っていないことを確かめる
fn check_output(sent: &[u8], got: &[u8]) -> usize {
    assert!(!got.is_empty(), "no TS output");
    assert_eq!(got.len() % 188, 0);
    let first = &got[..188];
    let start = sent.chunks_exact(188).position(|p| p == first).expect("first output packet must be one of the sent packets");
    let n = got.len() / 188;
    assert_eq!(&sent[start * 188..(start + n) * 188], got, "output must equal the sent packets");
    n
}

fn run(rotation: u32, inverted: bool, sigma: f32, seed: u64) -> usize {
    let mut rng = Rng(seed);
    let ts = make_ts(420, &mut rng);
    let mut tx = J83bTx::new(0b0101);
    let mut syms = Vec::new();
    tx.push_packets(&ts, &mut syms);
    // 単位電力へ正規化し、90 度単位の回転、スペクトルの反転 (共役)、雑音を加える
    let rot = Complex32::new(0.0, 1.0).powu(rotation);
    let rx: Vec<Complex32> = syms
        .iter()
        .map(|&z| {
            let mut s = z / LEVEL_SCALE * rot;
            if inverted {
                s = s.conj();
            }
            s + Complex32::new(rng.gauss(), rng.gauss()) * sigma
        })
        .collect();
    let mut dec = J83bDecoder::new();
    let mut out = Vec::new();
    for chunk in rx.chunks(7777) {
        dec.push(chunk, &mut out);
    }
    let n = check_output(&ts, &out);
    let s = &dec.stats;
    // 向きを誤った捕捉では、判定に使った RS ブロックだけが訂正不能になる
    let flips = if inverted { 0 } else { 1 };
    assert_eq!(s.orientation_flips, flips, "{s:?}");
    assert!(s.rs_uncorrectable <= flips * ORIENT_BLOCKS as u64, "{s:?}");
    assert_eq!(s.checksum_errors, 0, "{s:?}");
    assert_eq!(s.control_word, Some(0b0101));
    n
}

#[test]
fn round_trip_with_rotations_and_spectral_inversion() {
    // 検証環境の MSi2500 と同じくスペクトルが反転した入力で、4 通りの回転を確かめる (雑音は MER 約 33 dB)
    for rot in 0..4 {
        let n = run(rot, true, 0.016, 0x5eed + rot as u64);
        assert!(n > 100, "rotation {rot}: only {n} packets");
    }
}

#[test]
fn single_corrupted_control_word_does_not_break_the_stream() {
    // 同期語が一致したフレームで制御語だけが誤っても、インターリーブの解除を作り直さずに TS を出し続ける
    let mut rng = Rng(0xc0de);
    let ts = make_ts(600, &mut rng);
    let mut tx = J83bTx::new(0b0101);
    let mut levels = Vec::new();
    tx.push_packets(&ts[..300 * 188], &mut levels);
    tx.corrupt_next_control_word(0b0111);
    tx.push_packets(&ts[300 * 188..], &mut levels);
    let syms: Vec<Complex32> = levels.iter().map(|&z| (z / LEVEL_SCALE).conj()).collect();
    let mut dec = J83bDecoder::new();
    let mut out = Vec::new();
    for chunk in syms.chunks(7777) {
        dec.push(chunk, &mut out);
    }
    let n = check_output(&ts, &out);
    let s = &dec.stats;
    assert!(n > 450, "only {n} packets: {s:?}");
    assert_eq!((s.rs_uncorrectable, s.checksum_errors, s.packet_sync_losses), (0, 0, 0), "{s:?}");
    assert_eq!(s.control_word, Some(0b0101));
    assert_eq!(s.control_word_rejects, 1);
}

#[test]
fn round_trip_recovers_from_wrong_initial_orientation() {
    // 反転していない入力では、受信側が最初の RS ブロックから向きの誤りを検出し、共役を外して捕捉し直す
    let n = run(1, false, 0.016, 0xabcdef);
    assert!(n > 50, "only {n} packets");
}

fn rrc(t: f64, a: f64) -> f64 {
    use std::f64::consts::PI;
    if t.abs() < 1e-9 {
        return 1.0 - a + 4.0 * a / PI;
    }
    if (t.abs() - 1.0 / (4.0 * a)).abs() < 1e-9 {
        return a / 2f64.sqrt() * ((1.0 + 2.0 / PI) * (PI / (4.0 * a)).sin() + (1.0 - 2.0 / PI) * (PI / (4.0 * a)).cos());
    }
    ((PI * t * (1.0 - a)).sin() + 4.0 * a * t * (PI * t * (1.0 + a)).cos()) / (PI * t * (1.0 - (4.0 * a * t).powi(2)))
}

/// 送信シンボルを RRC で波形にし、指定されたレートで標本化する。
/// シンボルレートのずれ (ppm)、搬送波のずれ、直流、雑音を加え、検証環境と同じくスペクトルを反転する
fn synthesize(syms: &[Complex32], fs: f64, ppm: f64, cfo_hz: f64, snr_db: f64, rng: &mut Rng) -> Vec<Complex32> {
    let rs = SYMBOL_RATE * (1.0 + ppm * 1e-6);
    let n_out = ((syms.len() as f64 - 20.0) / rs * fs) as usize;
    let delay = 0.37; // シンボル単位の端数の遅延
    let span = 10i64;
    let mut out = Vec::with_capacity(n_out);
    let sigma = (10f64.powf(-snr_db / 10.0) / 2.0).sqrt() as f32;
    for m in 0..n_out {
        let ts = m as f64 / fs * rs + 10.0 + delay;
        let k0 = ts.floor() as i64;
        let mut v = Complex32::default();
        for k in (k0 - span)..=(k0 + span) {
            if k < 0 || k as usize >= syms.len() {
                continue;
            }
            v += syms[k as usize] * rrc(ts - k as f64, ROLLOFF) as f32;
        }
        let ph = 2.0 * std::f64::consts::PI * cfo_hz * m as f64 / fs;
        let v = v * Complex32::from_polar(1.0, ph as f32);
        // 信号の電力 (約 1) はシンボルレートの帯域にあるため、標本あたりの雑音の分散を fs / rs 倍にすると帯域内の SNR が snr_db になる
        let n = Complex32::new(rng.gauss(), rng.gauss()) * sigma * ((fs / rs) as f32).sqrt();
        out.push((v + n).conj() * 300.0 + Complex32::new(4.0, -3.0));
    }
    out
}

#[test]
fn qam_demodulator_and_fec_recover_synthetic_signal() {
    check_qam_sample_rate(512e6 / 63.0, false);
}

#[test]
fn qam_demodulator_and_fec_recover_synthetic_signal_at_12_msps() {
    check_qam_sample_rate(12e6, false);
}

#[test]
fn qam_demodulator_and_fec_recover_synthetic_signal_with_periodic_gaps() {
    check_qam_sample_rate(12e6, true);
}

fn check_qam_sample_rate(fs: f64, periodic_gaps: bool) {
    // 検証環境のバンド III の MER (約 30 dB) に近い雑音、受信機の時計のずれ、搬送波のずれを与える
    let mut rng = Rng(0x0dd_ba11);
    let ts = make_ts(3000, &mut rng);
    let mut tx = J83bTx::new(0b0101);
    let mut levels = Vec::new();
    tx.push_packets(&ts, &mut levels);
    let syms: Vec<Complex32> = levels.iter().map(|&z| z / LEVEL_SCALE).collect();
    let mut x = synthesize(&syms, fs, -25.0, 1500.0, 31.0, &mut rng);
    if periodic_gaps {
        // Model the six sample erasures reported for each PX-S1UD USB block.
        for block in x.chunks_exact_mut(7366) {
            block[1..7].fill(Complex32::default());
        }
    }
    let mut demod = QamDemod::new(fs, 1.0);
    let mut dec = J83bDecoder::new();
    let (mut s, mut out) = (Vec::new(), Vec::new());
    for chunk in x.chunks(384 * 16) {
        s.clear();
        demod.push(chunk, &mut s);
        dec.push(&s, &mut out);
    }
    let q = &demod.stats;
    let st = &dec.stats;
    eprintln!("{q:?}\n{st:?}");
    assert!(q.dd && q.restarts == 0, "{q:?}");
    assert!(q.mer_db > 28.0, "{q:?}");
    let n = check_output(&ts, &out);
    assert!(n > 2000, "only {n} packets");
    assert_eq!(st.rs_uncorrectable, 0, "{st:?}");
    assert_eq!(st.checksum_errors, 0, "{st:?}");
}

/// Blackman 窓の FIR による低域通過 (遅延を補正し、出力の長さは入力から taps - 1 を引いた値)
fn lowpass(x: &[Complex32], cut_hz: f64, fs: f64, taps: usize) -> Vec<Complex32> {
    use std::f64::consts::PI;
    let m = (taps - 1) as f64 / 2.0;
    let h: Vec<f32> = (0..taps)
        .map(|n| {
            let t = n as f64 - m;
            let s = if t == 0.0 { 2.0 * cut_hz / fs } else { (2.0 * PI * cut_hz / fs * t).sin() / (PI * t) };
            let a = 2.0 * PI * n as f64 / (taps - 1) as f64;
            (s * (0.42 - 0.5 * a.cos() + 0.08 * (2.0 * a).cos())) as f32
        })
        .collect();
    x.windows(taps).map(|w| w.iter().zip(&h).map(|(v, c)| v * c).sum()).collect()
}

/// PX-S1UD のメッセージ本体 (s1ud.rs) の形へ、連続した 12 MS/s のサンプル列を切り出す。
/// 各ブロックは先頭のサンプルの直後で gaps[k] サンプルを落とし、int8 の I0, I1, Q0, Q1 の組 (Q は符号を反転) と
/// 20 ビットのブロック番号で表す
fn s1ud_blocks(x: &[Complex32], gaps: &[usize], scale: f32) -> Vec<u8> {
    use msi2500_frontend::s1ud::{BLOCK_BYTES, BLOCK_SAMPLES};
    let q = |v: f32| (v * scale * 128.0).round().clamp(-128.0, 127.0) as i8 as u8;
    let mut out = Vec::new();
    let mut p = 0;
    for (k, &g) in gaps.iter().enumerate() {
        if p + 1 + g + BLOCK_SAMPLES > x.len() {
            break;
        }
        let mut s = vec![x[p]];
        s.extend_from_slice(&x[p + 1 + g..p + g + BLOCK_SAMPLES]);
        p += g + BLOCK_SAMPLES;
        let start = out.len();
        for pair in s.chunks_exact(2) {
            out.extend_from_slice(&[q(pair[0].re), q(pair[1].re), q(-pair[0].im), q(-pair[1].im)]);
        }
        out.extend_from_slice(&(k as u32 & 0xF_FFFF).to_le_bytes());
        assert_eq!(out.len() - start, BLOCK_BYTES);
    }
    out
}

/// PX-S1UD の J.83B の IQ では、ブロック間で落ちるサンプル数が 5 と 6 を中心に不規則に変わる。
/// 落ちた長さをブロックごとに判定して時間軸を戻し、I と Q の利得のずれ (約 0.5 dB) を補正して、TS を誤りなく復元する
#[test]
fn s1ud_frontend_recovers_variable_gaps() {
    check_s1ud(|rng| match rng.next() % 100 {
        0 => 4,
        1 => 7,
        2..=40 => 5,
        _ => 6,
    }, None);
}

/// チップがブロックを落としても、8 ブロックまでは落ちた時間を消失で埋め、欠落の長さを試行で決めて追従を続ける。
/// 落ちたブロックのシンボルは誤り訂正で訂正できないが、フレーム同期と復調器の状態を保つ
#[test]
fn s1ud_frontend_bridges_lost_blocks() {
    check_s1ud(|rng| if rng.next() % 100 < 40 { 5 } else { 6 }, Some((170, 3)));
}

/// gap は各ブロックの欠落の長さを決める。lost = (始めのブロック, 数) のブロックを、番号を飛ばして取り除く
fn check_s1ud(gap: impl Fn(&mut Rng) -> usize, lost: Option<(usize, usize)>) {
    use msi2500_frontend::s1ud::{S1udFrontend, BLOCK_BYTES};
    let fs = 12e6;
    let mut rng = Rng(0x51_0d);
    let ts = make_ts(4000, &mut rng);
    let mut tx = J83bTx::new(0b0101);
    let mut levels = Vec::new();
    tx.push_packets(&ts, &mut levels);
    let syms: Vec<Complex32> = levels.iter().map(|&z| z / LEVEL_SCALE).collect();
    let x = synthesize(&syms, fs, -2.0, -250.0, 33.0, &mut rng);
    // PX-S1UD のチューナーの通過帯域 (約 ±2.85 MHz) で、J.83B のロールオフの裾 (±3.136 MHz) を削る。
    // 尤度による欠落の判定は、この帯域制限による信号の冗長さを使う
    let mut x = lowpass(&x, 2.85e6, fs, 255);
    // I の利得を 0.5 dB 上げる (スペクトルの鏡像が約 -31 dB で重なる)
    for v in x.iter_mut() {
        v.re *= 1.059;
    }
    let gaps: Vec<usize> = (0..400).map(|_| gap(&mut rng)).collect();
    let rms = (x.iter().map(|v| v.norm_sqr()).sum::<f32>() / x.len() as f32).sqrt();
    let data = s1ud_blocks(&x, &gaps, 0.2 / rms);
    let mut front = S1udFrontend::new(fs, 1.0);
    let mut dec = J83bDecoder::new();
    let (mut s, mut out) = (Vec::new(), Vec::new());
    let mut judged_gaps = Vec::new();
    for (k, blk) in data.chunks_exact(BLOCK_BYTES).enumerate() {
        if lost.is_some_and(|(a, n)| (a..a + n).contains(&k)) {
            continue;
        }
        let (before, trials) = (front.stats.gaps, front.stats.trial_decisions);
        s.clear();
        assert!(!front.push_block(blk, &mut s), "{:?}", front.stats);
        if let Some(g) = (0..before.len()).find(|&g| front.stats.gaps[g] != before[g]) {
            judged_gaps.push((k, g, front.stats.trial_decisions != trials));
        }
        dec.push(&s, &mut out);
    }
    let (f, q, st) = (&front.stats, front.demod_stats(), &dec.stats);
    eprintln!("{f:?}
{q:?}
{st:?}");
    // 判定帰還での試行は長さを誤らない。判定帰還の前の尤度の判定は、138.750 MHz の収録で約 6% 誤ったため、
    // 数回までの誤りを許す (始めの 64 ブロックは自己相関の推定に使い、判定しない)
    let wrong: Vec<_> = judged_gaps.iter().filter(|&&(k, g, _)| gaps[k] != g).collect();
    assert!(wrong.iter().all(|w| !w.2) && wrong.len() <= 3, "wrong gaps (block, judged, by trial) {wrong:?}");
    if let Some((_, n)) = lost {
        assert_eq!((f.lost_blocks, f.bridged, f.resets), (n as u64, 1, 0), "{f:?}");
    }
    assert!(q.dd && q.restarts == 0, "{q:?}");
    assert!(q.mer_db > 28.0, "{q:?}");
    let n = check_output_with_gaps(&ts, &out);
    assert!(n > 1500, "only {n} packets");
    // ブロックを落とした区間のシンボルは訂正できないため、訂正の誤りがないことは欠落のないときだけ確かめる。
    // 欠落があってもフレーム同期は保つ
    assert_eq!(st.frame_sync_losses, 0, "{st:?}");
    if lost.is_none() {
        assert_eq!(st.rs_uncorrectable, 0, "{st:?}");
        assert_eq!(st.checksum_errors, 0, "{st:?}");
    }
}

/// TEI のない出力パケットがすべて送信した TS のパケットであり、送信した順に並ぶことを確かめ、その数を返す。
/// 欠落で抜けたパケットと、訂正できず TEI を立てたパケットは許す
fn check_output_with_gaps(sent: &[u8], got: &[u8]) -> usize {
    assert!(!got.is_empty(), "no TS output");
    assert_eq!(got.len() % 188, 0);
    let sent: Vec<&[u8]> = sent.chunks_exact(188).collect();
    let (mut pos, mut n) = (0, 0);
    for p in got.chunks_exact(188).filter(|p| p[1] & 0x80 == 0) {
        let i = sent[pos..].iter().position(|s| *s == p).expect("output packet without TEI must be a sent packet in order");
        pos += i + 1;
        n += 1;
    }
    n
}

/// PAT (PID 0) の section の先頭から transport_stream_id を読む
fn pat_tsid(ts: &[u8]) -> Option<u16> {
    for p in ts.chunks_exact(188) {
        let pid = ((p[1] as u16 & 0x1f) << 8) | p[2] as u16;
        if pid != 0 || p[1] & 0x40 == 0 || p[1] & 0x80 != 0 {
            continue;
        }
        let mut i = 4;
        if p[3] & 0x20 != 0 {
            i += 1 + p[4] as usize;
        }
        i += 1 + *p.get(i)? as usize;
        if *p.get(i)? == 0x00 {
            return Some(((*p.get(i + 3)? as u16) << 8) | *p.get(i + 4)? as u16);
        }
    }
    None
}

#[test]
fn recorded_iq_decodes_without_uncorrectable_blocks() {
    let Some(path) = std::env::var_os("MSI2500_TEST_J83B") else {
        eprintln!("MSI2500_TEST_J83B is not set; skipping");
        return;
    };
    let data = std::fs::read(path).unwrap();
    // MSI2500_TEST_J83B_PLL で判定帰還 PLL の帯域の倍率を変えられる (位相雑音の多い UHF の調査用)
    let pll: f32 = std::env::var("MSI2500_TEST_J83B_PLL").ok().and_then(|v| v.parse().ok()).unwrap_or(1.0);
    let mut counter = BlockCounter::with_startup_run(384, 1);
    let mut demod = QamDemod::new(512e6 / 63.0, pll);
    let mut dec = J83bDecoder::new();
    let (mut samples, mut syms, mut out) = (Vec::new(), Vec::new(), Vec::new());
    let (mut t_demod, mut t_fec) = (0f64, 0f64);
    for blk in data.chunks_exact(1024) {
        if let BlockState::Gap(_) | BlockState::Backward = counter.push(blk) {
            // 欠落の前後は別の区間として扱い、復調と誤り訂正を作り直す
            demod = QamDemod::new(512e6 / 63.0, pll);
            dec = J83bDecoder::new();
            samples.clear();
            out.clear();
        }
        decode_block(blk, &mut samples);
        if samples.len() >= 16 * 384 {
            syms.clear();
            let t0 = std::time::Instant::now();
            demod.push(&samples, &mut syms);
            let t1 = std::time::Instant::now();
            dec.push(&syms, &mut out);
            t_demod += (t1 - t0).as_secs_f64();
            t_fec += t1.elapsed().as_secs_f64();
            samples.clear();
        }
    }
    let q = &demod.stats;
    let st = &dec.stats;
    let tsid = pat_tsid(&out);
    eprintln!(
        "{q:?}\n{st:?}\nTSID {tsid:04x?}, demod {t_demod:.2} s + FEC {t_fec:.2} s for {:.2} s of signal",
        q.samples as f64 / (512e6 / 63.0)
    );
    assert!(q.dd && q.mer_db > 26.0, "{q:?}");
    assert!(st.packets > 1000, "{st:?}");
    assert_eq!(st.rs_uncorrectable, 0, "{st:?}");
    assert_eq!(st.checksum_errors, 0, "{st:?}");
    // TEI の立ったパケットがないこと
    assert!(out.chunks_exact(188).all(|p| p[0] == 0x47 && p[1] & 0x80 == 0));
    if let Some(want) = std::env::var_os("MSI2500_TEST_J83B_TSID") {
        let want = u16::from_str_radix(want.to_str().unwrap().trim_start_matches("0x"), 16).unwrap();
        assert_eq!(tsid, Some(want));
    }
}
