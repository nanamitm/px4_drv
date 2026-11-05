//! RSP1 互換機 (Mirics MSi2500 + MSi001) による ISDB-T フルセグ受信の helper。
//! 設計は winusb/docs/msi2500-fullseg-design.md を参照。

pub mod blocks;
pub mod fastvit;
pub mod fecstream;
pub mod hostproto;
pub mod live;
pub mod ofdm;
pub mod raw384;
pub mod regs;
pub mod remux;
pub mod usb;
