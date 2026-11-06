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

/// 送信シンボルを RRC で波形にし、MSi2500 と同じ 8.127 MS/s で標本化する。
/// シンボルレートのずれ (ppm)、搬送波のずれ、直流、雑音を加え、検証環境と同じくスペクトルを反転する
fn synthesize(syms: &[Complex32], ppm: f64, cfo_hz: f64, snr_db: f64, rng: &mut Rng) -> Vec<Complex32> {
    let fs = 512e6 / 63.0;
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
    // 検証環境のバンド III の MER (約 30 dB) に近い雑音、受信機の時計のずれ、搬送波のずれを与える
    let mut rng = Rng(0x0dd_ba11);
    let ts = make_ts(3000, &mut rng);
    let mut tx = J83bTx::new(0b0101);
    let mut levels = Vec::new();
    tx.push_packets(&ts, &mut levels);
    let syms: Vec<Complex32> = levels.iter().map(|&z| z / LEVEL_SCALE).collect();
    let x = synthesize(&syms, -25.0, 1500.0, 31.0, &mut rng);
    let mut demod = QamDemod::new(512e6 / 63.0, 1.0);
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
