//! MSi2500 の 384 形式 (10 ビット + 2 ビットのシフト指定) の 1024 バイトブロックを IQ に復号する。
//! 配置は libmirisdr convert/384_s16.c と同じ: 16 バイトのヘッダーの後に 6 グループ、
//! 各グループは 10 バイト×16 (8 値ずつ) と 4 バイトのシフト指定。値は I/Q の順で、最後の 24 バイトは使わない。

use num_complex::Complex32;

pub const SAMPLES_PER_BLOCK: usize = 384;

/// 1 ブロックを復号して out に 384 サンプル (±1 に正規化) を追加する。
pub fn decode_block(blk: &[u8], out: &mut Vec<Complex32>) {
    let mut vals = [0i16; 768];
    let mut r = 0usize;
    for j in 0..6 {
        let g = 16 + j * 164;
        let sb = &blk[g + 160..g + 164];
        let shift = u32::from_le_bytes([sb[0], sb[1], sb[2], sb[3]]);
        for k in 0..16 {
            let s = &blk[g + k * 10..g + k * 10 + 10];
            let b = |i: usize| s[i] as u16;
            let v: [u16; 8] = [
                (b(0) << 6) | ((b(1) & 0x03) << 14),
                ((b(1) & 0xfc) << 4) | ((b(2) & 0x0f) << 12),
                ((b(2) & 0xf0) << 2) | ((b(3) & 0x3f) << 10),
                (b(3) & 0xc0) | (b(4) << 8),
                (b(5) << 6) | ((b(6) & 0x03) << 14),
                ((b(6) & 0xfc) << 4) | ((b(7) & 0x0f) << 12),
                ((b(7) & 0xf0) << 2) | ((b(8) & 0x3f) << 10),
                (b(8) & 0xc0) | (b(9) << 8),
            ];
            // シフト指定 0 は 2 ビット、1 は 1 ビット右へずらす (符号は保つ)。2 と 3 はそのまま
            let sh = match (shift >> (2 * k)) & 3 {
                0 => 2,
                1 => 1,
                _ => 0,
            };
            for x in v {
                vals[r] = (x as i16) >> sh;
                r += 1;
            }
        }
    }
    for i in 0..SAMPLES_PER_BLOCK {
        out.push(Complex32::new(vals[2 * i] as f32 / 32768.0, vals[2 * i + 1] as f32 / 32768.0));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 10 ビット値 8 個を 10 バイトへ詰める (decode_block の逆)
    fn pack(v: [i16; 8]) -> [u8; 10] {
        let u: Vec<u16> = v.iter().map(|&x| (x as u16) & 0x3ff).collect();
        [
            u[0] as u8,
            ((u[0] >> 8) | (u[1] << 2)) as u8,
            ((u[1] >> 6) | (u[2] << 4)) as u8,
            ((u[2] >> 4) | (u[3] << 6)) as u8,
            (u[3] >> 2) as u8,
            u[4] as u8,
            ((u[4] >> 8) | (u[5] << 2)) as u8,
            ((u[5] >> 6) | (u[6] << 4)) as u8,
            ((u[6] >> 4) | (u[7] << 6)) as u8,
            (u[7] >> 2) as u8,
        ]
    }

    #[test]
    fn decodes_10bit_values_with_shift_and_sign() {
        let mut blk = [0u8; 1024];
        let vals: [i16; 8] = [1, -1, 511, -512, 100, -100, 0, 255];
        // グループ 0 の先頭 2 組: シフト指定 3 (そのまま) と 0 (2 ビット右)
        blk[16..26].copy_from_slice(&pack(vals));
        blk[26..36].copy_from_slice(&pack(vals));
        blk[16 + 160..16 + 164].copy_from_slice(&(0b00_11u32).to_le_bytes());
        let mut out = Vec::new();
        decode_block(&blk, &mut out);
        assert_eq!(out.len(), 384);
        // シフト 3: 10 ビット値は上位へ詰められ、×64 になる
        for (i, &v) in vals.iter().enumerate() {
            let c = out[i / 2];
            let got = if i % 2 == 0 { c.re } else { c.im };
            assert_eq!(got, (v as f32 * 64.0) / 32768.0, "shift3 index {i}");
        }
        // シフト 0: さらに 2 ビット右 (×16)
        for (i, &v) in vals.iter().enumerate() {
            let c = out[4 + i / 2];
            let got = if i % 2 == 0 { c.re } else { c.im };
            assert_eq!(got, (v as f32 * 16.0) / 32768.0, "shift0 index {i}");
        }
    }
}
