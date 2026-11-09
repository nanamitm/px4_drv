//! 誤り訂正の試験。
//!
//! ブロック並列の整数 Viterbi は、isdbt-dsp の全トレースバック Viterbi (浮動小数点) を基準にして、
//! 雑音を加えた符号語で誤り率が基準と同等になることを確かめる。
//! 逐次投入の復号器は、入力の区切り方によらず一括復号と同じビットを返すことを確かめる。
//! 録画 IQ を使う試験は tests/ofdm.rs と同じく環境変数 MSI2500_TEST_IQ を指定したときだけ実行する。

use isdbt_dsp::viterbi::{conv_encode, depuncture, puncture, Viterbi, PUNCTURE_2_3, PUNCTURE_3_4};
use msi2500_frontend::blocks::{BlockCounter, BlockState};
use msi2500_frontend::fastvit::{decode_parallel, quantize, Isa, StreamDecoder};
use msi2500_frontend::fecstream::LayerPipe;
use msi2500_frontend::ofdm::Ofdm;
use msi2500_frontend::raw384::decode_block;

/// 試験を再現可能にするための xorshift64 と Box-Muller による正規乱数。
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn unit(&mut self) -> f64 {
        ((self.next() >> 11) as f64 + 0.5) / (1u64 << 53) as f64
    }
    fn gauss(&mut self) -> f32 {
        let (u1, u2) = (self.unit(), self.unit());
        ((-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()) as f32
    }
}

/// 情報ビットを符号化・パンクチャし、BPSK (1→+1) に雑音を加えて母符号位置の軟値へ戻す。
fn noisy_codeword(rng: &mut Rng, n: usize, pattern: &[u8], sigma: f32) -> (Vec<u8>, Vec<f32>) {
    let info: Vec<u8> = (0..n).map(|_| (rng.next() & 1) as u8).collect();
    let (mother, _) = conv_encode(&info, 0);
    let tx = puncture(&mother, pattern);
    let rx: Vec<f32> = tx.iter().map(|&b| if b == 1 { 1.0 } else { -1.0 } + sigma * rng.gauss()).collect();
    (info, depuncture(&rx, pattern))
}

fn errors(a: &[u8], b: &[u8]) -> usize {
    a.iter().zip(b).filter(|(x, y)| x != y).count()
}

/// fecstream と同じ量子化 (軟値を ±16 に制限してから 12 倍)
fn quantize_like_pipe(soft: &[f32]) -> Vec<i8> {
    let clamped: Vec<f32> = soft.iter().map(|&x| x.clamp(-16.0, 16.0)).collect();
    quantize(&clamped, 12.0)
}

#[test]
fn noiseless_codewords_decode_exactly() {
    let mut rng = Rng(0x1234_5678_9abc_def1);
    for pattern in [&PUNCTURE_2_3[..], &PUNCTURE_3_4[..]] {
        // depuncture はパンクチャ周期に満たない末尾を捨てるため、2/3 と 3/4 の周期の公倍数の長さにする
        let (info, soft) = noisy_codeword(&mut rng, 60_000, pattern, 0.0);
        let q = quantize_like_pipe(&soft);
        for threads in [1, 3] {
            let bits = decode_parallel(&q, 8192, 192, threads, Isa::Base);
            assert_eq!(bits.len(), info.len());
            assert_eq!(errors(&bits, &info), 0, "threads={threads}");
        }
    }
}

#[test]
fn noisy_codewords_match_reference_error_rate() {
    let reference = Viterbi::new();
    let mut rng = Rng(0x0dd_ba11_cafe_f00d);
    // 復号前の誤りが数 % になる雑音で、基準と同程度まで訂正できることを確かめる
    for (pattern, sigma) in [(&PUNCTURE_2_3[..], 0.75f32), (&PUNCTURE_3_4[..], 0.62f32)] {
        let n = 200_000;
        let (info, soft) = noisy_codeword(&mut rng, n, pattern, sigma);
        let ref_bits = reference.decode(&soft);
        let fast = decode_parallel(&quantize_like_pipe(&soft), 8192, 192, 4, Isa::Base);
        let (e_ref, e_fast) = (errors(&ref_bits, &info), errors(&fast, &info));
        eprintln!("pattern={pattern:?} sigma={sigma}: reference errors={e_ref} block-parallel errors={e_fast} / {n}");
        assert!(e_ref > 0, "noise too weak to compare decoders");
        // 量子化とブロック境界による劣化は基準の誤り数の 1 割と 20 ビットまでとする
        assert!(e_fast <= e_ref + e_ref / 10 + 20, "reference={e_ref} block-parallel={e_fast}");
    }
}

#[test]
fn stream_decoder_matches_batch_for_any_chunking() {
    let mut rng = Rng(0x5eed_5eed_5eed_5eed);
    let (_, soft) = noisy_codeword(&mut rng, 300_000, &PUNCTURE_3_4, 0.6);
    let q = quantize_like_pipe(&soft);
    let batch = decode_parallel(&q, 8192, 192, 3, Isa::Base);
    for max_chunk in [2usize, 1000, 18_432, 1 << 20] {
        let mut sd = StreamDecoder::new(8192, 192, 3, Isa::Base);
        let mut out = Vec::new();
        let mut pos = 0;
        while pos < q.len() {
            // (r1, r2) の対を割らないように偶数長で区切る
            let len = ((rng.next() as usize % max_chunk) / 2 * 2 + 2).min(q.len() - pos);
            out.extend(sd.push(&q[pos..pos + len]));
            pos += len;
        }
        out.extend(sd.finish());
        assert_eq!(out, batch, "max_chunk={max_chunk}");
    }
}

#[test]
fn avx2_matches_base() {
    if !is_x86_feature_detected!("avx2") {
        eprintln!("AVX2 is not available; skipping");
        return;
    }
    let mut rng = Rng(0xa5a5_a5a5_0f0f_0f0f);
    let (_, soft) = noisy_codeword(&mut rng, 100_000, &PUNCTURE_3_4, 0.65);
    let q = quantize_like_pipe(&soft);
    assert_eq!(decode_parallel(&q, 8192, 192, 2, Isa::Avx2), decode_parallel(&q, 8192, 192, 2, Isa::Base));
}

type Pipes = Option<(LayerPipe, LayerPipe)>;

/// OFDM の出力を、TMCC が得られた時点で作った A・B 階層の誤り訂正へ流す。
fn feed(samples: &mut Vec<num_complex::Complex32>, ofdm: &mut Ofdm, pipes: &mut Pipes, ts_b: &mut Vec<u8>) {
    let mut ts_a = Vec::new();
    for sym in ofdm.push(samples) {
        if pipes.is_none() {
            if let Some(t) = sym.tmcc {
                let (na, nb) = (t.layers[0].nseg as usize, t.layers[1].nseg as usize);
                let sa: Vec<usize> = (0..na).collect();
                let sb: Vec<usize> = (na..na + nb).collect();
                *pipes = Some((LayerPipe::new(&t.layers[0], &sa, 1), LayerPipe::new(&t.layers[1], &sb, 2)));
            }
        }
        if let Some((a, b)) = pipes.as_mut() {
            a.push(&sym, &mut ts_a);
            b.push(&sym, ts_b);
        }
    }
    samples.clear();
}

#[test]
fn recorded_iq_decodes_without_rs_failures() {
    let Some(path) = std::env::var_os("MSI2500_TEST_IQ") else {
        eprintln!("MSI2500_TEST_IQ is not set; skipping");
        return;
    };
    let data = std::fs::read(path).unwrap();
    let mut counter = BlockCounter::with_startup_run(384, 1);
    let mut ofdm = Ofdm::new();
    let mut samples = Vec::new();
    let mut pipes: Pipes = None;
    let mut finished = Vec::new();
    let mut ts_b = Vec::new();
    for blk in data.chunks_exact(1024) {
        if let BlockState::Gap(_) | BlockState::Backward = counter.push(blk) {
            // 不連続の前後をつながず、復調と誤り訂正の状態をすべて作り直す
            ofdm = Ofdm::new();
            samples.clear();
            if let Some(p) = pipes.take() {
                finished.push(p);
            }
        }
        decode_block(blk, &mut samples);
        if samples.len() >= 256 * 384 {
            feed(&mut samples, &mut ofdm, &mut pipes, &mut ts_b);
        }
    }
    feed(&mut samples, &mut ofdm, &mut pipes, &mut ts_b);
    for (pa, pb) in &finished {
        assert_eq!(pa.stats.uncorrectable + pb.stats.uncorrectable, 0);
    }
    let (a, b) = pipes.expect("FEC started");
    assert!(b.stats.blocks > 10_000, "B blocks {}", b.stats.blocks);
    assert!(a.stats.blocks > 200, "A blocks {}", a.stats.blocks);
    assert_eq!((b.stats.uncorrectable, b.stats.tei, b.stats.cc_errors), (0, 0, 0));
    assert_eq!((a.stats.uncorrectable, a.stats.tei, a.stats.cc_errors), (0, 0, 0));
    assert_eq!(ts_b.len() % 188, 0);
    assert!(ts_b.chunks_exact(188).all(|p| p[0] == 0x47));
}
