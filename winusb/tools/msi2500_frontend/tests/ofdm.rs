//! OFDM 復調の試験。
//!
//! 録画 IQ はリポジトリに含めないため、録画を使う試験は環境変数 MSI2500_TEST_IQ に
//! capture の生ブロックファイル (ISDB-T Mode 3、A 階層 QPSK 2/3 1 セグ、B 階層 64QAM 3/4 12 セグの局) を
//! 指定したときだけ実行する。

use msi2500_frontend::blocks::{BlockCounter, BlockState};
use msi2500_frontend::ofdm::{prbs_signs, LayerParam, Ofdm, Tmcc};
use msi2500_frontend::raw384::decode_block;

#[test]
fn prbs_matches_isdbt_pilot_sequence() {
    // 11 段 LFSR、初期値オール 1: w0..w10 = 1 (-1)、w11 = 0 (+1)、周期 2047
    let p = prbs_signs();
    assert!(p[..11].iter().all(|&v| v == -1.0));
    assert_eq!(p[11], 1.0);
    for k in 0..32 {
        assert_eq!(p[k], p[k + 2047]);
    }
}

#[test]
fn segments_are_assigned_to_layers_in_order() {
    let lay = |nseg| LayerParam { modulation: 3, rate: 2, interleave: 2, nseg };
    let t = Tmcc { partial: true, layers: [lay(1), lay(12), LayerParam { nseg: 15, ..lay(0) }] };
    assert_eq!(t.layer_of_segment(0), Some(0));
    assert_eq!(t.layer_of_segment(1), Some(1));
    assert_eq!(t.layer_of_segment(12), Some(1));
    assert_eq!(t.layer_of_segment(13), None);
}

#[test]
fn recorded_iq_locks_and_decodes_tmcc() {
    let Some(path) = std::env::var_os("MSI2500_TEST_IQ") else {
        eprintln!("MSI2500_TEST_IQ is not set; skipping");
        return;
    };
    let data = std::fs::read(path).unwrap();
    let mut counter = BlockCounter::with_startup_run(384, 1);
    let mut ofdm = Ofdm::new();
    let mut samples = Vec::new();
    let mut out = 0usize;
    for blk in data.chunks_exact(1024) {
        if let BlockState::Gap(_) | BlockState::Backward = counter.push(blk) {
            ofdm = Ofdm::new();
            samples.clear();
            out = 0;
        }
        decode_block(blk, &mut samples);
        if samples.len() >= 256 * 384 {
            out += ofdm.push(&samples).len();
            samples.clear();
        }
    }
    let s = &ofdm.stats;
    assert!(s.acquired);
    assert_eq!(s.ctrl_matches_center, Some(true));
    let t = s.tmcc.expect("TMCC");
    assert_eq!((t.layers[0].modulation, t.layers[0].rate, t.layers[0].interleave, t.layers[0].nseg), (1, 1, 3, 1));
    assert_eq!((t.layers[1].modulation, t.layers[1].rate, t.layers[1].interleave, t.layers[1].nseg), (3, 2, 2, 12));
    assert!(out > 1000, "symbols out: {out}");
    assert!(s.mer_db[1] > 24.0, "B layer MER {}", s.mer_db[1]);
    assert!(s.last_pos_error.abs() < 1.0, "window position error {}", s.last_pos_error);
}
