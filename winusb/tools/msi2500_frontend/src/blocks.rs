//! MSi2500 の 1024 バイトブロックのヘッダー (先頭 4 バイト、little endian) にあるサンプル番号で連続性を確かめる。
//! 番号はブロックごとに形式のサンプル数 (384 形式なら 384) ずつ増える。
//! ストリーミング開始直後は、形式切り替え前のブロックと、開始から約 0.06 秒後 (検証環境で約 1,200 ブロック目) の
//! 1 回の欠落がある。増分が STARTUP_RUN ブロック続くまでを開始区間とし、その間の不連続は開始区間をやり直す。
//! 開始区間のブロックは捨てる。

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlockState {
    /// 連続性が確立する前のブロック (捨てる)
    Startup,
    Ok,
    /// 受信中の欠落 (失ったサンプル数)
    Gap(u64),
    /// 番号の巻き戻り (選局し直しなど)
    Backward,
}

/// 開始区間を終えるのに必要な、連続して正しい増分のブロック数 (384 形式で約 0.19 秒)
pub const STARTUP_RUN: u32 = 4096;

#[derive(Clone, Debug, Default)]
pub struct BlockCounter {
    spb: u32,
    startup_run: u32,
    run: u32,
    expect: Option<u32>,
    started: bool,
    pub startup_blocks: u64,
    pub blocks: u64,
    pub gaps: u64,
    pub lost_samples: u64,
    pub backward: u64,
}

impl BlockCounter {
    pub fn new(samples_per_block: u32) -> Self {
        Self::with_startup_run(samples_per_block, STARTUP_RUN)
    }

    pub fn with_startup_run(samples_per_block: u32, startup_run: u32) -> Self {
        Self { spb: samples_per_block, startup_run, ..Default::default() }
    }

    /// 選局し直しなどで新しいストリームとして数え直す
    pub fn restart(&mut self) {
        self.started = false;
        self.run = 0;
        self.expect = None;
    }

    pub fn push(&mut self, blk: &[u8]) -> BlockState {
        let addr = u32::from_le_bytes([blk[0], blk[1], blk[2], blk[3]]);
        let prev = self.expect.replace(addr.wrapping_add(self.spb));
        if !self.started {
            self.run = if prev == Some(addr) { self.run + 1 } else { 0 };
            self.startup_blocks += 1;
            if self.run >= self.startup_run {
                self.started = true;
            }
            return BlockState::Startup;
        }
        self.blocks += 1;
        let e = prev.unwrap();
        if addr == e {
            return BlockState::Ok;
        }
        let d = addr.wrapping_sub(e) as i32;
        if d > 0 {
            self.gaps += 1;
            self.lost_samples += d as u64;
            BlockState::Gap(d as u64)
        } else {
            self.backward += 1;
            BlockState::Backward
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn blk(addr: u32) -> [u8; 1024] {
        let mut b = [0u8; 1024];
        b[..4].copy_from_slice(&addr.to_le_bytes());
        b
    }

    #[test]
    fn startup_blocks_are_separated_from_gaps() {
        let mut c = BlockCounter::with_startup_run(384, 3);
        // 形式切り替え前の 252 形式のブロック、少し続いた後の開始直後の欠落、その後の連続
        assert_eq!(c.push(&blk(0xfe)), BlockState::Startup);
        assert_eq!(c.push(&blk(0x1fa)), BlockState::Startup);
        assert_eq!(c.push(&blk(0x1fa + 384)), BlockState::Startup);
        assert_eq!(c.push(&blk(40000)), BlockState::Startup); // 開始区間内の欠落は数え直し
        assert_eq!(c.push(&blk(40384)), BlockState::Startup);
        assert_eq!(c.push(&blk(40768)), BlockState::Startup);
        assert_eq!(c.push(&blk(41152)), BlockState::Startup); // 3 回続いて開始区間を終える
        assert_eq!(c.push(&blk(41536)), BlockState::Ok);
        assert_eq!(c.gaps, 0);
        assert_eq!(c.lost_samples, 0);
        assert_eq!(c.startup_blocks, 7);
    }

    #[test]
    fn gap_and_backward_are_reported() {
        let mut c = BlockCounter::with_startup_run(384, 1);
        c.push(&blk(0));
        c.push(&blk(384));
        assert_eq!(c.push(&blk(384 * 5)), BlockState::Gap(384 * 3));
        assert_eq!(c.lost_samples, 384 * 3);
        assert_eq!(c.push(&blk(100)), BlockState::Backward);
        assert_eq!(c.push(&blk(484)), BlockState::Ok);
    }

    #[test]
    fn counter_wraps_around_u32() {
        let mut c = BlockCounter::with_startup_run(384, 1);
        c.push(&blk(u32::MAX - 383 - 384));
        c.push(&blk(u32::MAX - 383));
        assert_eq!(c.push(&blk(0)), BlockState::Ok);
    }
}
