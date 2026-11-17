//! 取り込みの短い欠落を、復調の状態を作り直さずに埋める処理の試験。
//!
//! 録画 IQ を使う試験は、環境変数 MSI2500_TEST_IQ に capture の生ブロックファイル
//! (ISDB-T Mode 3 の局を 20 秒以上) を指定したときだけ実行する。

use msi2500_frontend::live::{Live, LiveConfig, MAX_BRIDGE_SAMPLES};
use msi2500_frontend::ofdm::{Ofdm, L};
use num_complex::Complex32;
use std::io::Write;
use std::sync::{Arc, Mutex};

/// 再現できる白色雑音 (xorshift)
fn noise(n: usize, seed: &mut u64) -> Vec<Complex32> {
    let mut next = || {
        *seed ^= *seed << 13;
        *seed ^= *seed >> 7;
        *seed ^= *seed << 17;
        (*seed >> 40) as f32 / (1u64 << 24) as f32 - 0.5
    };
    (0..n).map(|_| Complex32::new(next(), next())).collect()
}

/// 雑音だけを入力しても、制御キャリアの検出を過ぎて出力を続け、埋めた区間のシンボルを消失として出す
#[test]
fn gap_symbols_are_erased_after_control_carrier_detection_on_noise() {
    let mut seed = 0x0fed_cba9_8765_4321u64;
    let mut ofdm = Ofdm::new();
    let mut out = Vec::new();
    // 初期同期 (212 シンボル余り) と制御キャリアの検出 (出力 256 シンボル) を終える分
    for _ in 0..40 {
        out.extend(ofdm.push(&noise(16 * L, &mut seed)));
    }
    assert!(!out.is_empty(), "no symbols after control carrier detection");
    assert!(out.iter().all(|s| s.z.len() == 13 * 384 && s.w.len() == 13 * 384));
    out.clear();
    out.extend(ofdm.push_gap(3 * L));
    for _ in 0..4 {
        out.extend(ofdm.push(&noise(16 * L, &mut seed)));
    }
    let erased = ofdm.stats.erased_symbols;
    assert!((3..=4).contains(&erased), "erased symbols {erased}");
    let zero: Vec<_> = out.iter().filter(|s| s.w.iter().all(|&w| w == 0.0)).collect();
    assert_eq!(zero.len() as u64, erased);
    assert!(zero.iter().all(|s| s.z.iter().all(|z| *z == Complex32::new(0.0, 0.0))));
    // 消失の前後で番号が連続する
    assert!(out.windows(2).all(|w| w[1].index == w[0].index + 1));
    assert!(out.iter().all(|s| s.w.iter().all(|w| w.is_finite())));
    assert!(ofdm.tracking());
}

/// 制御キャリアの検出の前までで、埋めた区間に窓が掛かるシンボルの数と、追従が続くことを確かめる
#[test]
fn gap_symbols_are_erased_and_tracking_continues() {
    let mut seed = 0x1234_5678_9abc_def0u64;
    let mut ofdm = Ofdm::new();
    // 初期同期 (212 シンボル余り) を終える分
    for _ in 0..14 {
        assert!(ofdm.push(&noise(16 * L, &mut seed)).is_empty());
    }
    assert!(ofdm.tracking());
    assert_eq!(ofdm.stats.erased_symbols, 0);
    // 3 シンボル分の欠落。窓 (N < L) が掛かるのは 3〜4 シンボルで、時刻は欠落の分だけ進む
    assert!(ofdm.push_gap(3 * L).is_empty());
    for _ in 0..2 {
        assert!(ofdm.push(&noise(8 * L, &mut seed)).is_empty());
    }
    let erased = ofdm.stats.erased_symbols;
    assert!((3..=4).contains(&erased), "erased symbols {erased}");
    assert!(ofdm.tracking());
    // 埋めた区間を過ぎた後は消失として扱わない
    assert!(ofdm.push(&noise(4 * L, &mut seed)).is_empty());
    assert_eq!(ofdm.stats.erased_symbols, erased);
}

#[derive(Clone, Default)]
struct Shared(Arc<Mutex<Vec<u8>>>);
impl Write for Shared {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        self.0.lock().unwrap().extend_from_slice(b);
        Ok(b.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

/// 録画を live の処理系へ流し、再多重した TS と統計を返す。drop は落とすブロックの範囲
fn run_live(data: &[u8], drop: Option<(usize, usize)>) -> (Vec<u8>, msi2500_frontend::live::LiveStats) {
    let ts = Shared::default();
    let (live, mut feeder) = Live::start(LiveConfig {
        viterbi_threads: 2,
        raw_queue: 64,
        symbol_queue: 512,
        ts_out: [None, None],
        ts_mux: Some(Box::new(ts.clone())),
    });
    for (i, blk) in data.chunks_exact(1024).enumerate() {
        if drop.is_some_and(|(a, b)| a <= i && i < b) {
            continue;
        }
        feeder.push_wait(blk);
    }
    let stats = live.finish(feeder);
    let out = ts.0.lock().unwrap().clone();
    (out, stats)
}

/// J.83B の録画を処理系へ流し、出力した TS と統計を返す。drop は落とすブロックの範囲
fn run_j83b(data: &[u8], drop: Option<(usize, usize)>) -> (Vec<u8>, msi2500_frontend::live_j83b::J83bLiveStats) {
    use msi2500_frontend::live_j83b::{J83bLive, J83bLiveConfig};
    let ts = Shared::default();
    let (live, mut feeder) = J83bLive::start(J83bLiveConfig {
        raw_queue: 64,
        symbol_queue: 512,
        pll_scale: 1.0,
        ts_out: Some(Box::new(ts.clone())),
    });
    for (i, blk) in data.chunks_exact(1024).enumerate() {
        if drop.is_some_and(|(a, b)| a <= i && i < b) {
            continue;
        }
        feeder.push_wait(blk);
    }
    let stats = live.finish(feeder);
    let out = ts.0.lock().unwrap().clone();
    (out, stats)
}

/// J.83B の録画 (MSI2500_TEST_J83B、20 秒以上) から実機で観測した長さ (171 ブロック、約 8 ms) の欠落を抜き、
/// 作り直さずに埋めて、失う TS が欠落の長さの分 (約 220 パケット) と小さな上乗せに収まることを確かめる。
/// 作り直す扱いでは、同じ欠落で約 700 パケットを失った
#[test]
fn recorded_j83b_survives_short_gap_without_restart() {
    let Some(path) = std::env::var_os("MSI2500_TEST_J83B") else {
        eprintln!("MSI2500_TEST_J83B is not set; skipping");
        return;
    };
    use msi2500_frontend::live_j83b::MAX_BRIDGE_SAMPLES as J83B_MAX;
    let data = std::fs::read(path).unwrap();
    let blocks_per_s = 8_126_984 / 384;
    assert!(data.len() / 1024 > 20 * blocks_per_s, "recording shorter than 20 s");
    let at = 12 * blocks_per_s;
    let (base, s0) = run_j83b(&data, None);
    let (gap, s1) = run_j83b(&data, Some((at, at + 171)));
    assert_eq!((s0.gaps, s0.segments, s0.bridged), (0, 1, 0));
    assert_eq!((s1.gaps, s1.bridged, s1.segments), (1, 1, 1));
    assert!(171 * 384 <= J83B_MAX);
    assert_eq!(s0.fec_total().rs_uncorrectable, 0, "baseline must decode cleanly");
    let lost = (base.len() - gap.len()) / 188;
    // 8.08 ms 分は約 220 パケット。作り直すと約 700 パケットになる
    assert!(lost < 400, "lost {lost} packets across an 8 ms gap");
}

#[test]
fn recorded_iq_survives_short_gap_without_restart() {
    let Some(path) = std::env::var_os("MSI2500_TEST_IQ") else {
        eprintln!("MSI2500_TEST_IQ is not set; skipping");
        return;
    };
    let data = std::fs::read(path).unwrap();
    let blocks_per_s = 8_126_984 / 384;
    assert!(data.len() / 1024 > 20 * blocks_per_s, "recording shorter than 20 s");
    // 実機で観測した 1 回分の欠落 (65,664 サンプル = 171 ブロック、約 8 ms) を 12 秒の位置で抜く
    let at = 12 * blocks_per_s;
    let (base, s0) = run_live(&data, None);
    let (gap, s1) = run_live(&data, Some((at, at + 171)));
    assert_eq!((s0.gaps, s0.segments), (0, 1));
    assert_eq!((s1.gaps, s1.bridged, s1.segments), (1, 1, 1));
    assert_eq!(s1.lost_samples, 171 * 384);
    assert!(171 * 384 <= MAX_BRIDGE_SAMPLES);
    assert!(s1.ofdm.erased_symbols > 0);
    assert_eq!(s1.layer_total(1).uncorrectable, s0.layer_total(1).uncorrectable);
    assert!(!base.is_empty());
    assert!(base == gap, "TS differs after an 8 ms gap ({} vs {} bytes)", base.len(), gap.len());
}
