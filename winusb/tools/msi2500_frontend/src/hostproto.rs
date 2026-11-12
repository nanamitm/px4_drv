//! DriverHost_PX4 との標準出力の形式。RTL-SDR の helper と同じ形式で、DriverHost 側は
//! winusb/src/DriverHost_PX4/rtlsdr/protocol.hpp の Frames が解釈する。
//!
//! 1 フレームは little endian の 24 バイトのヘッダー (magic、種別、payload 長、品質 0〜10000、RS 成功数、RS 失敗数) と
//! payload からなる。種別 1 は USB の初期化完了、2 は 188 バイト単位の TS、3 は状態、4 はエラー文字列。
//!
//! 状態の payload は空か、先頭 4 バイトに復調器の MER (dB×100、0〜10000 の符号なし整数) を置く。
//! 品質は RS の成功割合で C/N の表示に使えないため、MER を別に渡す。ヘッダーの形を変えると
//! RTL-SDR の helper と古い DriverHost の解釈が変わるため、payload へ追加した。MER を測れていない間
//! (判定帰還へ入る前など) は payload を空にする。

use std::io::Write;

pub const MAGIC: u32 = 0x3154_5352;
/// 1 フレームの TS の上限 (DriverHost の MaxTs と同じ)
pub const MAX_TS: usize = 188 * 2048;
/// エラー文字列の上限 (DriverHost の検査と同じ)
pub const MAX_ERROR: usize = 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Ready = 1,
    Ts = 2,
    Status = 3,
    Error = 4,
}

pub fn write_frame(out: &mut impl Write, kind: Kind, payload: &[u8], quality: u32, accepted: u64, rejected: u64) -> std::io::Result<()> {
    let payload = match kind {
        Kind::Error => &payload[..payload.len().min(MAX_ERROR)],
        _ => payload,
    };
    debug_assert!(payload.len() <= MAX_TS);
    let mut h = [0u8; 24];
    for (i, v) in [MAGIC, kind as u32, payload.len() as u32, quality.min(10_000), accepted.min(u32::MAX as u64) as u32, rejected.min(u32::MAX as u64) as u32]
        .into_iter()
        .enumerate()
    {
        h[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    out.write_all(&h)?;
    out.write_all(payload)
}

/// 状態フレームの payload (MER がなければ空)
pub fn status_payload(mer_db: Option<f32>) -> Vec<u8> {
    match mer_db {
        Some(m) if m.is_finite() => ((m * 100.0).round().clamp(0.0, 10_000.0) as u32).to_le_bytes().to_vec(),
        _ => Vec::new(),
    }
}

/// DriverHost は TEI の立ったパケット、adaptation_field_control が 0 のパケット、
/// adaptation field の長さが 183 を超えるパケットを含むフレームを形式不正として helper を止める。
/// RS の誤訂正でこうしたパケットが生じても受信を続けられるよう、送る前に取り除き、取り除いた数を返す。
pub fn retain_valid_packets(ts: &mut Vec<u8>) -> usize {
    let n = ts.len() / 188;
    let mut w = 0usize;
    for i in 0..n {
        let p = i * 188;
        let afc = (ts[p + 3] >> 4) & 3;
        let ok = ts[p] == 0x47 && ts[p + 1] & 0x80 == 0 && afc != 0 && !(afc & 2 != 0 && ts[p + 4] > 183);
        if ok {
            if w != p {
                ts.copy_within(p..p + 188, w);
            }
            w += 188;
        }
    }
    ts.truncate(w);
    n - w / 188
}

#[cfg(test)]
mod tests {
    use super::*;

    fn packet(b1: u8, b3: u8, b4: u8) -> [u8; 188] {
        let mut p = [0u8; 188];
        p[0] = 0x47;
        p[1] = b1;
        p[3] = b3;
        p[4] = b4;
        p
    }

    #[test]
    fn header_layout_matches_driverhost_parser() {
        let mut out = Vec::new();
        write_frame(&mut out, Kind::Ts, &[0x47; 188], 12_345, 7, 3).unwrap();
        let w = |i: usize| u32::from_le_bytes(out[i * 4..i * 4 + 4].try_into().unwrap());
        assert_eq!((w(0), w(1), w(2), w(3), w(4), w(5)), (MAGIC, 2, 188, 10_000, 7, 3));
        assert_eq!(out.len(), 24 + 188);
    }

    #[test]
    fn status_payload_carries_mer_x100() {
        assert_eq!(status_payload(None), Vec::<u8>::new());
        assert_eq!(status_payload(Some(f32::NAN)), Vec::<u8>::new());
        assert_eq!(status_payload(Some(31.234)), 3123u32.to_le_bytes().to_vec());
        assert_eq!(status_payload(Some(-3.0)), 0u32.to_le_bytes().to_vec());
        assert_eq!(status_payload(Some(200.0)), 10_000u32.to_le_bytes().to_vec());
    }

    #[test]
    fn invalid_packets_are_removed_in_order() {
        let good1 = packet(0x01, 0x10, 0); // payload のみ
        let tei = packet(0x81, 0x10, 0);
        let afc0 = packet(0x01, 0x00, 0);
        let long_af = packet(0x01, 0x30, 184);
        let good2 = packet(0x02, 0x30, 183); // adaptation field 183 は許容
        let mut ts: Vec<u8> = [good1, tei, afc0, long_af, good2].concat();
        assert_eq!(retain_valid_packets(&mut ts), 3);
        assert_eq!(ts, [good1, good2].concat());
    }
}
