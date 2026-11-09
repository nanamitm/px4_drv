//! 移植したレジスター計算が、libmirisdr-5 が実機へ送った書き込み列と一致することを確かめる。
//!
//! tests/data/regs/f<周波数>_bw<帯域幅>_g<ゲイン>.trace は、トレースを出力するよう改造した
//! libmirisdr-5 (コミット 6392af06) を次の順に呼んで記録した (標本化 8,126,984 S/s、384 形式、既定の GPIO 表):
//! mirisdr_open → set_hw_flavour(DEFAULT) → set_transfer → set_sample_format("384_S16")
//! → set_sample_rate → set_center_freq → set_if_freq(0) → set_bandwidth → set_tuner_gain_mode(1)
//! → set_tuner_gain → reset_buffer → read_async (ALT 1, START)。

use msi2500_frontend::regs::{Format, FrontEnd, IfMode, Op, Tuner};
use std::path::PathBuf;

fn render(op: &Op) -> String {
    match op {
        Op::Write(r, v) => format!("TRACE W {:02x} {:06x}", r, v & 0xffffff),
        Op::Stop => "TRACE STOP".into(),
        Op::Start => "TRACE START".into(),
    }
}

fn parse_name(name: &str) -> (u32, u32, i32) {
    // f473142857_bw6000000_g40.trace
    let stem = name.trim_end_matches(".trace");
    let mut it = stem.split('_');
    let f = it.next().unwrap()[1..].parse().unwrap();
    let bw = it.next().unwrap()[2..].parse().unwrap();
    let g = it.next().unwrap()[1..].parse().unwrap();
    (f, bw, g)
}

fn emulate(freq: u32, bw: u32, gain: i32) -> Vec<String> {
    let mut t = Tuner::new();
    t.front_end = FrontEnd::Default;
    let mut ops = t.open_sequence();
    ops.extend(t.set_sample_format(Some(Format::S384)));
    ops.extend(t.set_sample_rate(8_126_984));
    ops.extend(t.set_center_freq(freq));
    ops.extend(t.set_if_mode(IfMode::Zero));
    ops.extend(t.set_bandwidth(bw));
    ops.extend(t.set_tuner_gain(gain));
    ops.iter().map(render).collect()
}

#[test]
fn register_sequences_match_libmirisdr_traces() {
    let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/data/regs");
    let mut n = 0;
    for e in std::fs::read_dir(&dir).unwrap() {
        let p = e.unwrap().path();
        let name = p.file_name().unwrap().to_str().unwrap().to_string();
        if !name.ends_with(".trace") {
            continue;
        }
        let text = std::fs::read_to_string(&p).unwrap();
        // 記録の ALT 以降 (等時転送の設定と開始) はレジスター計算の対象外
        let want: Vec<&str> = text.lines().map(|l| l.trim_end()).take_while(|l| !l.starts_with("TRACE ALT")).collect();
        let (f, bw, g) = parse_name(&name);
        let got = emulate(f, bw, g);
        assert_eq!(got.len(), want.len(), "{name}: 書き込み数が違う");
        for (i, (a, b)) in got.iter().zip(want.iter()).enumerate() {
            assert_eq!(a, b, "{name}: {i} 番目の書き込みが違う");
        }
        n += 1;
    }
    assert!(n >= 150, "期待値ファイルが少ない: {n}");
}

#[test]
fn write_setup_packs_value_and_index_like_libmirisdr() {
    use msi2500_frontend::regs::write_setup;
    // reg.c: value = (val & 0xff) << 8 | reg, index = (val >> 8) & 0xffff
    assert_eq!(write_setup(0x09, 0x2001f6), (0xf609, 0x2001));
    assert_eq!(write_setup(0x07, 0x0000a5), (0xa507, 0x0000));
}

#[test]
fn configure_sequence_ends_in_same_register_state_as_call_sequence() {
    // 製品で使う1回ずつの設定列が、libmirisdr の呼び出し順と同じ最終値をレジスターごとに残すこと
    for &(f, bw, g) in &[(473_142_857u32, 6_000_000u32, 40i32), (767_142_857, 6_000_000, 25), (473_142_857, 8_000_000, 60)] {
        let full = emulate(f, bw, g);
        let mut t = Tuner::new();
        t.format_auto = false;
        t.format = Format::S384;
        t.rate = 8_126_984;
        t.freq = f;
        t.if_mode = IfMode::Zero;
        let _ = t.set_bandwidth(bw);
        let _ = t.set_tuner_gain(g);
        let cfg: Vec<String> = t.configure_sequence().iter().map(render).collect();
        // MSi001 のレジスターは値の下位 4 ビットで区別される。0x09 以外はレジスター番号で区別する
        let last = |seq: &[String]| {
            let mut m = std::collections::BTreeMap::new();
            for l in seq {
                if let Some(rest) = l.strip_prefix("TRACE W ") {
                    let (r, v) = rest.split_at(2);
                    let v = u32::from_str_radix(v.trim(), 16).unwrap();
                    let key = if r == "09" { format!("09:{:x}", v & 0xf) } else { r.to_string() };
                    m.insert(key, v);
                }
            }
            m
        };
        assert_eq!(last(&cfg), last(&full), "f={f} bw={bw} g={g}");
    }
}
