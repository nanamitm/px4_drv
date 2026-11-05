//! RSP1 互換機 (Mirics MSi2500 + MSi001) による ISDB-T フルセグ受信の helper。
//! 設計は winusb/docs/msi2500-fullseg-design.md を参照。

pub mod blocks;
pub mod regs;
pub mod usb;
