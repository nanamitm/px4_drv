//! A・B・C 階層の TS を、多重フレーム上の TSP の位置に従って 1 本へ戻す。
//!
//! 送信側の再多重化装置は、ARIB STD-B31 のモデル受信機が再生する順序で TSP を並べる。
//! モデル受信機は 1 シンボルの復調データを FFT のサンプルクロックでセグメント番号の昇順、キャリヤーの昇順に
//! 読み出して階層へ振り分け、デパンクチャ後の符号化ビットが 1 TSP 分 (3,264 ビット、情報ビットで 204 バイト) に
//! 達した時点でその TSP を出力バッファへ移す。BTS の各 TSP の位置 (スロット) は、その時点で出力可能な TSP があれば
//! それを、なければヌル TSP を送る。BTS のクロックは FFT のサンプルクロックの 4 倍のため、1 スロットは
//! 204×8/4 = 408 サンプルクロックで、Mode 3・ガードインターバル 1/8 の 1 フレーム (204×9,216 クロック) は 4,608 スロット。
//!
//! この実装はモデルの処理遅延を各階層で等しいとみなし、データキャリヤーの読み出しをシンボルの先頭から始める。
//! 出力はヌル TSP を除いた階層の TSP だけのため、スロット位置の定数のずれは順序に影響しない。
//! 階層の TSP が同じ時刻に揃う場合の前後 (A を先に置く) と、遅延の定数は推定を含む。

use crate::fecstream::TspTag;
use crate::ofdm::Tmcc;
use std::collections::BTreeMap;

/// Mode 3・ガードインターバル 1/8 の 1 フレームのスロット数
pub const SLOTS_PER_FRAME: usize = 4608;
const SYMBOL_CLOCKS: u64 = 9216;
const SLOT_CLOCKS: u64 = 408;
const DATA_CARRIERS: usize = 384;
/// 符号化率 (TMCC の値 0..4 → 1/2, 2/3, 3/4, 5/6, 7/8)
const RATES: [(usize, usize); 5] = [(1, 2), (2, 3), (3, 4), (5, 6), (7, 8)];
const TSP_BITS: usize = 204 * 8;

/// モデル受信機で、各階層の k 番目の TSP が置かれるスロット番号。layers[l][k] = スロット。
/// 未使用の階層、または 1 フレームに TSP が収まらない設定の階層は空。
pub fn frame_slots(t: &Tmcc) -> [Vec<u16>; 3] {
    let mut per_carrier = [0usize; 3]; // 1 キャリヤーあたりの情報ビット (単位 1/den)
    let mut den = [1usize; 3];
    for l in 0..3 {
        let p = &t.layers[l];
        if p.nseg == 0 || p.nseg > 13 || p.mbits() == 0 || p.rate as usize >= RATES.len() {
            continue;
        }
        let (n, d) = RATES[p.rate as usize];
        per_carrier[l] = p.mbits() * n;
        den[l] = d;
    }
    let mut acc = [0usize; 3];
    let mut out: [Vec<u16>; 3] = Default::default();
    let mut next_free = 0u64;
    for sym in 0..204u64 {
        for seg in 0..13usize {
            let Some(l) = t.layer_of_segment(seg) else { continue };
            if per_carrier[l] == 0 {
                continue;
            }
            for i in 0..DATA_CARRIERS {
                acc[l] += per_carrier[l];
                if acc[l] >= TSP_BITS * den[l] {
                    acc[l] -= TSP_BITS * den[l];
                    // このキャリヤーを読み終えた時刻に TSP が揃い、その時刻以降の最初の空きスロットへ置く
                    let ready = sym * SYMBOL_CLOCKS + (seg * DATA_CARRIERS + i) as u64 + 1;
                    let slot = ready.div_ceil(SLOT_CLOCKS).max(next_free);
                    next_free = slot + 1;
                    out[l].push(slot.min(u16::MAX as u64) as u16);
                }
            }
        }
    }
    out
}

#[derive(Clone, Debug, Default)]
pub struct RemuxStats {
    pub frames: u64,
    pub packets: u64,
    /// 出力済みのフレームに属するため捨てたパケット
    pub late: u64,
    /// フレーム番号が未定のため捨てたパケット
    pub untagged: u64,
    /// 他の階層が 3 フレーム先へ進んだため、揃うのを待たずに出したフレーム
    pub forced: u64,
    /// ある階層のパケット数が TMCC から求めた 1 フレームの TSP 数と異なったフレーム。
    /// 区間の最初と最後のフレーム、訂正不能のパケット、フレーム番号の判定の誤りで増える
    pub partial_frames: u64,
}

/// 1 区間 (OFDM の同期を取り直すまで) の再多重化。
pub struct Remuxer {
    slots: [Vec<u16>; 3],
    frames: BTreeMap<i64, Vec<(u16, u8, [u8; 188])>>,
    /// 各階層が最後に出したパケットのフレーム番号 (None はまだ出力がない階層)
    latest: [Option<i64>; 3],
    emitted: Option<i64>,
    pub stats: RemuxStats,
}

/// 他の階層を待たずにフレームを出すまでの、先行する階層のフレーム数
const MAX_LEAD_FRAMES: i64 = 3;

impl Remuxer {
    pub fn new(t: &Tmcc) -> Self {
        Self { slots: frame_slots(t), frames: BTreeMap::new(), latest: [None; 3], emitted: None, stats: RemuxStats::default() }
    }

    /// 1 階層のパケット列 (188 バイト単位) とその位置を投入し、揃ったフレームを out へ書く。
    pub fn push(&mut self, layer: usize, tags: &[TspTag], data: &[u8], out: &mut Vec<u8>) {
        for (tag, pkt) in tags.iter().zip(data.chunks_exact(188)) {
            let Some(f) = tag.frame else {
                self.stats.untagged += 1;
                continue;
            };
            if self.emitted.is_some_and(|e| f <= e) {
                self.stats.late += 1;
                continue;
            }
            // 位置が表の外 (TMCC と実際の TSP 数が合わない) ならフレームの末尾へ置く
            let slot = self.slots[layer].get(tag.k as usize).copied().unwrap_or(u16::MAX);
            let mut p = [0u8; 188];
            p.copy_from_slice(pkt);
            self.frames.entry(f).or_default().push((slot, layer as u8, p));
            self.latest[layer] = Some(self.latest[layer].map_or(f, |x| x.max(f)));
        }
        self.emit_ready(out, false);
    }

    /// 区間の終わり。残りのフレームをすべて出す。
    pub fn finish(&mut self, out: &mut Vec<u8>) {
        self.emit_ready(out, true);
    }

    fn emit_ready(&mut self, out: &mut Vec<u8>, all: bool) {
        while let Some((&f, _)) = self.frames.first_key_value() {
            // TMCC で使われている階層がすべてこのフレームを過ぎたら揃ったとみなす。
            // まだ出力のない階層も待つ (起動直後は B 階層が A 階層より約 1 フレーム早く出力を始めるため、
            // 待たないと A 階層の最初のフレームが出力済みのフレームとして捨てられる)。
            // 誤り訂正が同期できない階層があっても、他の階層が 3 フレーム先へ進めば出す
            let expected = (0..3).filter(|&l| !self.slots[l].is_empty());
            let complete = expected.clone().all(|l| self.latest[l].is_some_and(|x| x > f));
            let forced = self.latest.iter().flatten().any(|&x| x >= f + MAX_LEAD_FRAMES);
            if !(all || complete || forced) {
                break;
            }
            if !all && !complete {
                self.stats.forced += 1;
            }
            let mut pk = self.frames.pop_first().unwrap().1;
            let mut count = [0usize; 3];
            for &(_, l, _) in &pk {
                count[l as usize] += 1;
            }
            if (0..3).any(|l| !self.slots[l].is_empty() && count[l] != self.slots[l].len()) {
                self.stats.partial_frames += 1;
            }
            // 同じスロット番号は表の外のパケットだけのため、その中では階層と到着の順を保つ
            pk.sort_by_key(|&(slot, layer, _)| (slot, layer));
            for (_, _, p) in &pk {
                out.extend_from_slice(p);
            }
            self.stats.frames += 1;
            self.stats.packets += pk.len() as u64;
            self.emitted = Some(f);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ofdm::LayerParam;

    fn tmcc_13_17ch() -> Tmcc {
        // 13〜17ch の録画で復号した TMCC: A QPSK 2/3 I=4 1 セグメント、B 64QAM 3/4 I=2 12 セグメント
        Tmcc {
            partial: true,
            layers: [
                LayerParam { modulation: 1, rate: 1, interleave: 3, nseg: 1 },
                LayerParam { modulation: 3, rate: 2, interleave: 2, nseg: 12 },
                LayerParam { modulation: 7, rate: 7, interleave: 7, nseg: 15 },
            ],
        }
    }

    #[test]
    fn pattern_has_expected_tsp_counts_and_fits_in_frame() {
        let s = frame_slots(&tmcc_13_17ch());
        assert_eq!(s[0].len(), 64);
        assert_eq!(s[1].len(), 2592);
        assert!(s[2].is_empty());
        let mut all: Vec<u16> = s[0].iter().chain(&s[1]).copied().collect();
        assert!(all.iter().all(|&x| (x as usize) < SLOTS_PER_FRAME));
        all.sort();
        all.dedup();
        assert_eq!(all.len(), 64 + 2592, "slots must be unique");
        // 各階層の中ではスロットが増加する
        for l in 0..2 {
            assert!(s[l].windows(2).all(|w| w[0] < w[1]));
        }
    }

    fn tag(frame: i64, k: u32) -> TspTag {
        TspTag { frame: Some(frame), k }
    }

    fn pkt(layer: u8, k: u32) -> [u8; 188] {
        let mut p = [0u8; 188];
        p[0] = 0x47;
        p[4] = layer;
        p[5..9].copy_from_slice(&k.to_le_bytes());
        p
    }

    fn ids(out: &[u8]) -> Vec<(u8, u32)> {
        out.chunks_exact(188).map(|p| (p[4], u32::from_le_bytes(p[5..9].try_into().unwrap()))).collect()
    }

    #[test]
    fn frames_are_merged_in_slot_order_after_all_layers_pass() {
        let t = tmcc_13_17ch();
        let s = frame_slots(&t);
        let mut r = Remuxer::new(&t);
        let mut out = Vec::new();
        // B のフレーム 0 が先に揃っても、A がフレーム 0 を終えるまでは出さない
        let push_frame = |r: &mut Remuxer, layer: usize, f: i64, n: u32, out: &mut Vec<u8>| {
            let tags: Vec<TspTag> = (0..n).map(|k| tag(f, k)).collect();
            let data: Vec<u8> = (0..n).flat_map(|k| pkt(layer as u8, k)).collect();
            r.push(layer, &tags, &data, out);
        };
        push_frame(&mut r, 1, 0, 2592, &mut out);
        push_frame(&mut r, 1, 1, 10, &mut out);
        assert!(out.is_empty());
        push_frame(&mut r, 0, 0, 64, &mut out);
        assert!(out.is_empty());
        push_frame(&mut r, 0, 1, 1, &mut out);
        let got = ids(&out);
        assert_eq!(got.len(), 64 + 2592);
        let mut expect: Vec<(u16, u8, u32)> = (0..64u32).map(|k| (s[0][k as usize], 0u8, k)).chain((0..2592u32).map(|k| (s[1][k as usize], 1u8, k))).collect();
        expect.sort();
        assert_eq!(got, expect.iter().map(|&(_, l, k)| (l, k)).collect::<Vec<_>>());
        assert_eq!(r.stats.frames, 1);
        assert_eq!(r.stats.forced, 0);
    }

    #[test]
    fn stalled_layer_does_not_hold_output_forever() {
        let t = tmcc_13_17ch();
        let mut r = Remuxer::new(&t);
        let mut out = Vec::new();
        r.push(0, &[tag(0, 0)], &pkt(0, 0), &mut out);
        for f in 0..4 {
            r.push(1, &[tag(f, 0)], &pkt(1, 0), &mut out);
        }
        // B が 3 フレーム先へ進んだため、A を待たずにフレーム 0 を出す
        // (B の最初の TSP はシンボル 0 で揃い、A の最初の TSP は約 3 シンボル後に揃うため B が先)
        assert_eq!(ids(&out), vec![(1, 0), (0, 0)]);
        assert_eq!(r.stats.forced, 1);
        // 出力済みのフレームへ遅れて届いたパケットは捨てる
        r.push(0, &[tag(0, 1)], &pkt(0, 1), &mut out);
        assert_eq!(r.stats.late, 1);
        r.finish(&mut out);
        assert_eq!(ids(&out).len(), 5);
    }
}
