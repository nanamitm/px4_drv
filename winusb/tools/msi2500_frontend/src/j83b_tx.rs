//! J.83 Annex B (256QAM) の送信側の符号化。受信側 (j83b.rs) の試験に使う。
//! MPEG のチェックサム (B.4) → RS(128,122) (B.5.1) → インターリーブ (B.5.2) → ランダム化 (B.5.4)
//! → 同期トレーラー (B.5.3) → 差動プリコーダーと BCC、配置 (B.5.5)。

use crate::j83b::{
    checksum, interleave_params, pn_frame, rs_encode, BccEncoder, FRAME_MPEG_BYTES, FRAME_SYMBOLS, GROUPS_PER_FRAME, MAP,
    RS_BLOCKS_PER_FRAME, RS_K, RS_N, SYNC_GROUPS, SYNC_WORD,
};
use num_complex::Complex32;
use std::collections::VecDeque;

/// 図 B.15 の群内の並び (B なら true、番号)。受信側と同じ表を独立に持つ
const ORDER: [(bool, u8); 38] = [
    (false, 0), (true, 0), (false, 1), (false, 2), (false, 3), (true, 1), (true, 2), (true, 3),
    (false, 4), (true, 4), (false, 5), (false, 6), (false, 7), (true, 5), (true, 6), (true, 7),
    (false, 8), (true, 8), (false, 9), (false, 10), (false, 11), (true, 9), (true, 10), (true, 11),
    (false, 12), (true, 12), (false, 13), (false, 14), (false, 15), (true, 13), (true, 14), (true, 15),
    (false, 16), (false, 17), (false, 18), (true, 16), (true, 17), (true, 18),
];

pub struct J83bTx {
    ctrl: u8,
    i: usize,
    j: usize,
    fifos: Vec<VecDeque<u8>>,
    k: usize,
    /// MPEG のバイト列 (チェックサムで同期バイトを置き換えた後)
    mpeg: Vec<u8>,
    /// 直前のパケットの 187 バイト (最初は 0)
    prev: Vec<u8>,
    enc_x: BccEncoder,
    enc_y: BccEncoder,
    xp: u8,
    yp: u8,
    /// C7..C0 からレベルの番号 (I, Q) への逆写像
    inv: [(u8, u8); 256],
    /// 次のフレームの同期トレーラーだけに入れる制御語 (受信側の誤りへの耐性の試験用。インターリーブは変えない)
    ctrl_override: Option<u8>,
}

impl J83bTx {
    pub fn new(ctrl: u8) -> Self {
        let (i, j) = interleave_params(ctrl).expect("valid interleaver control word");
        let fifos = (0..i).map(|b| VecDeque::from(vec![0u8; b * j])).collect();
        let mut inv = [(0u8, 0u8); 256];
        for (ii, row) in MAP.iter().enumerate() {
            for (qi, &c) in row.iter().enumerate() {
                inv[c as usize] = (ii as u8, qi as u8);
            }
        }
        Self { ctrl, i, j, fifos, k: 0, mpeg: Vec::new(), prev: vec![0u8; 187], enc_x: BccEncoder::default(), enc_y: BccEncoder::default(), xp: 0, yp: 0, inv, ctrl_override: None }
    }

    /// 次に組み上がるフレームの同期トレーラーの制御語だけを ctrl にする
    pub fn corrupt_next_control_word(&mut self, ctrl: u8) {
        self.ctrl_override = Some(ctrl & 15);
    }

    /// TS パケット (188 バイト単位、先頭 0x47) を入れ、組み上がったフレームのシンボル (レベル ±1..±15) を out へ追加する
    pub fn push_packets(&mut self, ts: &[u8], out: &mut Vec<Complex32>) {
        let cs = checksum();
        for p in ts.chunks_exact(188) {
            // 同期バイトの位置に、直前のパケットの 187 バイトのチェックサムを置く
            self.mpeg.push(cs.encode(&self.prev));
            self.mpeg.extend_from_slice(&p[1..]);
            self.prev.copy_from_slice(&p[1..]);
        }
        while self.mpeg.len() >= FRAME_MPEG_BYTES {
            let bytes: Vec<u8> = self.mpeg.drain(..FRAME_MPEG_BYTES).collect();
            self.frame(&bytes, out);
        }
    }

    fn frame(&mut self, bytes: &[u8], out: &mut Vec<Complex32>) {
        // MPEG のビット列を 7 ビットの RS シンボルにして RS 符号化する
        let bits: Vec<u8> = bytes.iter().flat_map(|&b| (0..8).rev().map(move |k| (b >> k) & 1)).collect();
        let msyms: Vec<u8> = bits.chunks_exact(7).map(|c| c.iter().fold(0u8, |a, &b| (a << 1) | b)).collect();
        assert_eq!(msyms.len(), RS_BLOCKS_PER_FRAME * RS_K);
        let mut syms = Vec::with_capacity(FRAME_SYMBOLS);
        for m in msyms.chunks_exact(RS_K) {
            syms.extend_from_slice(&rs_encode(m));
        }
        assert_eq!(syms.len(), RS_BLOCKS_PER_FRAME * RS_N);
        // インターリーブ (枝 b の遅延 b × J 回の訪問) とランダム化
        let pn = pn_frame();
        let mut fbits = Vec::with_capacity(FRAME_SYMBOLS * 7);
        for (n, &s) in syms.iter().enumerate() {
            let b = self.k % self.i;
            self.k += 1;
            let f = &mut self.fifos[b];
            f.push_back(s);
            let v = f.pop_front().unwrap() ^ pn[n];
            for k in (0..7).rev() {
                fbits.push((v >> k) & 1);
            }
        }
        let _ = self.j;
        // 同期トレーラー: 32 ビットの同期語、4 ビットの制御語、4 ビットの 0
        let mut sync = Vec::with_capacity(40);
        for k in (0..32).rev() {
            sync.push(((SYNC_WORD >> k) & 1) as u8);
        }
        let ctrl = self.ctrl_override.take().unwrap_or(self.ctrl);
        for k in (0..4).rev() {
            sync.push((ctrl >> k) & 1);
        }
        sync.extend_from_slice(&[0, 0, 0, 0]);
        let mut fi = 0usize;
        let mut si = 0usize;
        for g in 0..GROUPS_PER_FRAME {
            let sync_group = g >= GROUPS_PER_FRAME - SYNC_GROUPS;
            let mut a = [0u8; 19];
            let mut b = [0u8; 19];
            for &(is_b, n) in ORDER.iter() {
                let bit = if sync_group && n % 4 == 0 && n <= 12 {
                    si += 1;
                    sync[si - 1]
                } else {
                    fi += 1;
                    fbits[fi - 1]
                };
                if is_b {
                    b[n as usize] = bit;
                } else {
                    a[n as usize] = bit;
                }
            }
            self.group(&a, &b, out);
        }
        assert_eq!(fi, fbits.len());
        assert_eq!(si, 40);
    }

    fn group(&mut self, a: &[u8; 19], b: &[u8; 19], out: &mut Vec<Complex32>) {
        // 差動プリコーダー (図 B.16): X = W + X' + Z (X' + Y')、Y = Z + W + Y' + Z (X' + Y')
        let mut x = [0u8; 4];
        let mut y = [0u8; 4];
        for k in 0..4 {
            let (w, z) = (a[4 * k], b[4 * k]);
            let s = z & (self.xp ^ self.yp);
            x[k] = w ^ self.xp ^ s;
            y[k] = z ^ w ^ self.yp ^ s;
            self.xp = x[k];
            self.yp = y[k];
        }
        let u = self.enc_x.group(x);
        let v = self.enc_y.group(y);
        let base = [1usize, 5, 9, 13, 16];
        for t in 0..5 {
            let bb = base[t];
            let c = (a[bb + 2] << 7) | (a[bb + 1] << 6) | (a[bb] << 5) | (u[t] << 4) | (b[bb + 2] << 3) | (b[bb + 1] << 2) | (b[bb] << 1) | v[t];
            let (ii, qi) = self.inv[c as usize];
            out.push(Complex32::new(-15.0 + 2.0 * ii as f32, -15.0 + 2.0 * qi as f32));
        }
    }
}
