//! RSP1 互換機 (Mirics MSi2500 + MSi001) による ISDB-T フルセグ受信の helper。
//! 設計は winusb/docs/msi2500-fullseg-design.md を参照。

pub mod blocks;
pub mod fastvit;
pub mod fecstream;
pub mod hostproto;
pub mod j83b;
pub mod j83b_tx;
pub mod live;
pub mod live_j83b;
pub mod ofdm;
pub mod qam;
pub mod raw384;
pub mod regs;
pub mod remux;
pub mod s1ud;
// WinUSB で MSi2500 を動かす。PX-S1UD の J.83B (serve-s1ud) は USB をホストが持つため使わない
#[cfg(windows)]
pub mod usb;
