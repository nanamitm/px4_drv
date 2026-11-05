//! MSi2500 (USB ブリッジ・ADC) と MSi001 (チューナー) のレジスター設定。
//!
//! libmirisdr-5 (ericek111/libmirisdr-5 コミット 6392af06、GPL-2.0-or-later) の
//! hard.c / soft.c / gain.c / adc.c の計算を移植した。書き込みの順序と整数演算の折り返しは元の実装と同じにしてあり、
//! tests/regs_golden.rs で元の実装が実機へ送った書き込み列と一致することを確認する。
//!
//! レジスター 0x09 への書き込みは MSi001 への SPI 転送で、値の下位 4 ビットが MSi001 のレジスター番号を表す。

/// デバイスへ送る操作。`Write(reg, value)` は vendor 要求 0x41、Stop は 0x45、Start は 0x43。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    Write(u8, u32),
    Stop,
    Start,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    S252,
    S336,
    S384,
    S504,
}

impl Format {
    /// 1 ブロック (1024 バイト) あたりのサンプル数
    pub fn samples_per_block(self) -> u32 {
        match self {
            Format::S252 => 252,
            Format::S336 => 336,
            Format::S384 => 384,
            Format::S504 => 504,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bandwidth {
    K200,
    K300,
    K600,
    K1536,
    M5,
    M6,
    M7,
    M8,
    Max,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IfMode {
    Zero,
    K450,
    K1620,
    K2048,
}

/// RF 入力経路を切り替える GPIO (レジスター 0x08) の表。
/// USB ID が同じ純正機と互換機を区別できないため、設定で選ぶ。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrontEnd {
    /// libmirisdr の既定の表。検証した互換機はこちらで受信できた
    Default,
    /// libmirisdr の SDRplay 用の表
    SdrPlay,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Band {
    Am1,
    Am2,
    Vhf,
    B3,
    B45,
    BL,
}

const MODE_AM: i32 = 0x01;
const MODE_VHF: i32 = 0x02;
const MODE_B3: i32 = 0x04;
const MODE_B45: i32 = 0x08;
const MODE_BL: i32 = 0x10;

struct PlanRow {
    low_cut_mhz: u64,
    mode: i32,
    upconvert: u32,
    am_port: u32,
    lo_div: u64,
    band_select: u32,
}

const fn row(low_cut_mhz: u64, mode: i32, upconvert: u32, am_port: u32, lo_div: u64, band_select: u32) -> PlanRow {
    PlanRow { low_cut_mhz, mode, upconvert, am_port, lo_div, band_select }
}

// libmirisdr soft.c の hw_switch_freq_plan_default / hw_switch_freq_plan_sdrplay と同じ値。
// mode 6 の行は元の表のとおりで、帯域の判定 (band) はどの名前にも一致せず直前の値が残る。
const PLAN_DEFAULT: [PlanRow; 10] = [
    row(0, MODE_AM, 1, 1, 16, 0xf780),
    row(12, MODE_AM, 1, 1, 16, 0xff80),
    row(30, MODE_AM, 1, 1, 16, 0xf280),
    row(50, MODE_VHF, 0, 0, 32, 0xf380),
    row(108, MODE_B3, 0, 0, 16, 0xfa80),
    row(250, MODE_B3, 0, 0, 16, 0xf680),
    row(259, 6, 0, 0, 8, 0xf680),
    row(330, MODE_B45, 0, 0, 4, 0xf380),
    row(960, MODE_BL, 0, 0, 2, 0xfa80),
    row(2400, -1, 0, 0, 0, 0x0000),
];

const PLAN_SDRPLAY: [PlanRow; 10] = [
    row(0, MODE_AM, 1, 1, 16, 0xf580),
    row(12, MODE_AM, 1, 1, 16, 0xf580),
    row(30, MODE_AM, 1, 1, 16, 0xf580),
    row(50, MODE_VHF, 0, 0, 32, 0xf180),
    row(112, MODE_B3, 0, 0, 16, 0xf580),
    row(250, MODE_B3, 0, 0, 16, 0xf480),
    row(261, 6, 0, 0, 8, 0xf480),
    row(404, MODE_B45, 0, 0, 4, 0xf580),
    row(1000, MODE_BL, 0, 0, 2, 0xf580),
    row(2400, -1, 0, 0, 0, 0x0000),
];

pub const SAMPLE_RATE_MIN: u32 = 1_300_000;
pub const SAMPLE_RATE_MAX: u32 = 15_000_000;

/// チューナーと ADC の設定状態。libmirisdr の mirisdr_dev のうちレジスター計算に使う部分。
#[derive(Clone, Debug)]
pub struct Tuner {
    pub freq: u32,
    pub rate: u32,
    pub format_auto: bool,
    pub format: Format,
    pub bandwidth: Bandwidth,
    pub if_mode: IfMode,
    pub front_end: FrontEnd,
    pub bias: bool,
    gain: i32,
    gr_lna: u32,
    gr_mixbuffer: u32,
    gr_mixer: u32,
    gr_baseband: u32,
    band: Band,
    reg8: u32,
    dc_mode: u32,
    dc_speedup: bool,
    dc_track: u32,
    dc_period: u32,
}

impl Default for Tuner {
    fn default() -> Self {
        Self::new()
    }
}

impl Tuner {
    /// libmirisdr の mirisdr_open() が設定する初期値
    pub fn new() -> Self {
        Self {
            freq: 90_000_000,
            rate: 2_000_000,
            format_auto: true,
            format: Format::S252,
            bandwidth: Bandwidth::M8,
            if_mode: IfMode::Zero,
            front_end: FrontEnd::Default,
            bias: false,
            gain: 43,
            gr_lna: 0,
            gr_mixbuffer: 0,
            gr_mixer: 0,
            gr_baseband: 43,
            band: Band::Vhf,
            reg8: 0,
            dc_mode: 2, // MIRISDR_DC_PERIODIC2
            dc_speedup: false,
            dc_track: 0x1f,
            dc_period: 0x800,
        }
    }

    pub fn band(&self) -> Band {
        self.band
    }

    /// mirisdr_open() の後半: ストリーミング停止、ADC 停止、ADC 初期化、hard / soft / gain
    pub fn open_sequence(&mut self) -> Vec<Op> {
        let mut ops = vec![Op::Stop];
        ops.extend(adc_stop());
        ops.extend(adc_init());
        ops.extend(self.hard());
        ops.extend(self.soft());
        ops.extend(self.gain_regs());
        ops
    }

    /// mirisdr_set_hard(): サンプル形式と標本化レート
    pub fn hard(&mut self) -> Vec<Op> {
        let mut ops = Vec::new();
        self.rate = self.rate.clamp(SAMPLE_RATE_MIN, SAMPLE_RATE_MAX);
        if self.format_auto {
            self.format = if self.rate <= 6_048_000 {
                Format::S252
            } else if self.rate <= 8_064_000 {
                Format::S336
            } else if self.rate <= 9_216_000 {
                Format::S384
            } else {
                Format::S504
            };
        }
        let fmt_reg = match self.format {
            Format::S252 => 0x000094,
            Format::S336 => 0x000085,
            Format::S384 => 0x0000a5,
            Format::S504 => 0x000c94,
        };
        ops.push(Op::Write(0x07, fmt_reg));
        // 分周比: VCO が 202 MHz 以上になる最小の偶数 i (4..14)。見つからなければ i=16、VCO は i=14 の値
        let mut i: u64 = 4;
        let mut vco: u64 = 0;
        while i < 16 {
            vco = self.rate as u64 * i * 12;
            if vco >= 202_000_000 {
                break;
            }
            i += 2;
        }
        let n = vco / 48_000_000;
        let fract = 0x200000u64 * (vco % 48_000_000) / 48_000_000;
        let agc: u64 = match self.format {
            Format::S252 => 0x01,
            Format::S336 => 0x05,
            Format::S384 => 0x09,
            Format::S504 => 0x0d,
        };
        let mut reg3: u64 = 3;
        reg3 |= (0x07 & (i / 2 - 1)) << 2;
        reg3 |= (0x01 & (fract >> 20)) << 7;
        reg3 |= (0x0f & n) << 8;
        reg3 |= (0x0f & agc) << 12;
        reg3 |= 1 << 16;
        let reg4 = 0xfffff & fract;
        ops.push(Op::Write(0x04, reg4 as u32));
        ops.push(Op::Write(0x03, reg3 as u32));
        ops
    }

    /// mirisdr_set_soft(): 周波数、帯域切り替え、IF モード、帯域幅
    pub fn soft(&mut self) -> Vec<Op> {
        let plan: &[PlanRow; 10] = match self.front_end {
            FrontEnd::Default => &PLAN_DEFAULT,
            FrontEnd::SdrPlay => &PLAN_SDRPLAY,
        };
        let f = self.freq as u64;
        let mut i = 0usize;
        while f >= 1_000_000 * plan[i].low_cut_mhz {
            if plan[i].mode < 0 {
                break;
            }
            i += 1;
        }
        let sp = &plan[i - 1];
        let mut reg0: u64 = 0;
        let mut offset: u64 = 0;
        let lo_div: u64;
        if sp.mode == MODE_AM {
            reg0 |= (MODE_AM as u64) << 4;
            reg0 |= (sp.upconvert as u64) << 9;
            reg0 |= (sp.am_port as u64) << 11;
            if sp.upconvert != 0 {
                offset += 120_000_000;
            }
            lo_div = 16;
            self.band = if sp.am_port == 0 { Band::Am1 } else { Band::Am2 };
        } else {
            reg0 |= (sp.mode as u64) << 4;
            lo_div = sp.lo_div;
            match sp.mode {
                MODE_VHF => self.band = Band::Vhf,
                MODE_B3 => self.band = Band::B3,
                MODE_B45 => self.band = Band::B45,
                MODE_BL => self.band = Band::BL,
                _ => {}
            }
        }
        reg0 |= 1 << 10; // RF シンセサイザーは常に有効
        reg0 |= match self.if_mode {
            IfMode::Zero => 3,
            IfMode::K450 => 2,
            IfMode::K1620 => 1,
            IfMode::K2048 => 0,
        } << 12;
        let mut regd: u64 = 0x0d;
        reg0 |= match self.bandwidth {
            Bandwidth::K200 => 0,
            Bandwidth::K300 => 1,
            Bandwidth::K600 => 2,
            Bandwidth::K1536 => 3,
            Bandwidth::M5 => 4,
            Bandwidth::M6 => 5,
            Bandwidth::M7 => 6,
            Bandwidth::M8 => 7,
            Bandwidth::Max => {
                regd |= 1 << 4;
                7
            }
        } << 14;
        reg0 |= 0x02 << 17; // 水晶 24 MHz (変更は非対応)
        // 低消費電力モード (20..23 ビット) は通常動作 = 0

        let fvco = (f + offset) * lo_div;
        let n = fvco / 96_000_000;
        let mut thresh = 96_000_000 / lo_div;
        let mut frac = (fvco % 96_000_000) / lo_div;
        // thresh と frac を最大公約数で約分する
        let (mut a, mut b) = (thresh, frac);
        while a != 0 {
            let c = a;
            a = b % a;
            b = c;
        }
        thresh /= b;
        frac /= b;
        // レジスターの範囲 (12 ビット) まで分解能を落とす
        let a = (thresh + 4094) / 4095;
        thresh = (thresh + (a / 2)) / a;
        frac = (frac + (a / 2)) / a;
        let rfvco = 96_000_000u64.wrapping_mul(n * thresh * 4096 + frac * 4096) / (thresh * 4096 * lo_div);
        if f + offset < rfvco {
            frac = frac.wrapping_sub(1);
        }
        let rfvco = 96_000_000u64.wrapping_mul((n * thresh * 4096).wrapping_add(frac.wrapping_mul(4096)))
            / (thresh * 4096 * lo_div);
        // 元の実装と同じく符号なしで計算する (負になる場合は折り返し、下位 12 ビットだけを使う)
        let afc = (f + offset)
            .wrapping_sub(rfvco)
            .wrapping_mul(thresh)
            .wrapping_mul(4096)
            .wrapping_mul(lo_div)
            / 96_000_000;

        let reg3: u64 = 3 | ((afc & 4095) << 4);
        let reg5: u64 = 5 | ((0xfff & thresh) << 4) | (0x28 << 16);
        let reg2: u64 = 2 | ((0xfff & frac) << 4) | ((0x3f & n) << 16);

        self.reg8 = sp.band_select;
        vec![
            Op::Write(0x08, self.reg8_value()),
            Op::Write(0x09, 0x0e),
            Op::Write(0x09, reg3 as u32),
            Op::Write(0x09, reg0 as u32),
            Op::Write(0x09, reg5 as u32),
            Op::Write(0x09, reg2 as u32),
            Op::Write(0x09, regd as u32),
        ]
    }

    fn reg8_value(&self) -> u32 {
        // GPIO3 はバイアス T (アンテナ給電)
        self.reg8 | if self.bias { 1 << (3 + 8) } else { 0 }
    }

    /// mirisdr_set_gain(): 受信ゲインと DC オフセット補正
    pub fn gain_regs(&self) -> Vec<Op> {
        let mut reg1: u32 = 1;
        reg1 |= self.gr_baseband << 4;
        reg1 |= match self.band {
            Band::Am1 => (self.gr_mixbuffer & 0x03) << 10,
            Band::Am2 => (if self.gr_mixbuffer == 0 { 0 } else { 0x03 }) << 10,
            _ => 0,
        };
        reg1 |= self.gr_mixer << 12;
        if !matches!(self.band, Band::Am1 | Band::Am2) {
            reg1 |= self.gr_lna << 13;
        }
        reg1 |= (self.dc_mode & 0x7) << 14;
        reg1 |= (self.dc_speedup as u32) << 17;
        let reg6: u32 = 6 | ((self.dc_track & 0x3f) << 4) | ((self.dc_period & 0xfff) << 10);
        vec![Op::Write(0x09, reg1), Op::Write(0x09, reg6)]
    }

    /// mirisdr_set_tuner_gain(): 0..102 dB を LNA・ミキサー・ベースバンドの減衰へ割り振る
    pub fn set_tuner_gain(&mut self, gain: i32) -> Vec<Op> {
        let g = gain.clamp(0, 102);
        self.gain = g;
        if g >= 43 {
            self.gr_lna = 0;
            self.gr_mixbuffer = 0;
            self.gr_mixer = 0;
            self.gr_baseband = (59 - (g - 43)) as u32;
        } else if g >= 19 {
            self.gr_lna = 1;
            self.gr_mixbuffer = 3;
            self.gr_mixer = 0;
            self.gr_baseband = (59 - (g - 19)) as u32;
        } else {
            self.gr_lna = 1;
            self.gr_mixbuffer = 3;
            self.gr_mixer = 1;
            self.gr_baseband = (59 - g) as u32;
        }
        self.gain_regs()
    }

    pub fn tuner_gain(&self) -> i32 {
        self.gain
    }

    /// mirisdr_set_sample_format(): None は自動選択
    pub fn set_sample_format(&mut self, f: Option<Format>) -> Vec<Op> {
        match f {
            None => self.format_auto = true,
            Some(f) => {
                self.format_auto = false;
                self.format = f;
            }
        }
        self.hard()
    }

    pub fn set_sample_rate(&mut self, rate: u32) -> Vec<Op> {
        self.rate = rate;
        self.hard()
    }

    /// mirisdr_set_center_freq(): soft の後にゲインを書き直す
    pub fn set_center_freq(&mut self, freq: u32) -> Vec<Op> {
        self.freq = freq;
        let mut ops = self.soft();
        ops.extend(self.gain_regs());
        ops
    }

    pub fn set_if_mode(&mut self, m: IfMode) -> Vec<Op> {
        self.if_mode = m;
        let mut ops = self.soft();
        ops.extend(self.gain_regs());
        ops
    }

    /// mirisdr_set_bandwidth(): 指定値以上で最も狭い帯域幅を選ぶ。狭帯域では IF モードも変える
    pub fn set_bandwidth(&mut self, bw: u32) -> Vec<Op> {
        let mut b = Bandwidth::Max;
        if bw <= 8_000_000 {
            b = Bandwidth::M8;
        }
        if bw <= 7_000_000 {
            b = Bandwidth::M7;
        }
        if bw <= 6_000_000 {
            b = Bandwidth::M6;
        }
        if bw <= 5_000_000 {
            b = Bandwidth::M5;
        }
        if bw <= 1_536_000 {
            b = Bandwidth::K1536;
            if self.rate >= 5_000_000 {
                self.if_mode = IfMode::K1620;
            }
        }
        if bw <= 600_000 {
            b = Bandwidth::K600;
            self.if_mode = IfMode::K450;
        }
        if bw <= 300_000 {
            b = Bandwidth::K300;
            self.if_mode = IfMode::K450;
        }
        if bw <= 200_000 {
            b = Bandwidth::K200;
            self.if_mode = IfMode::K450;
        }
        self.bandwidth = b;
        let mut ops = self.soft();
        ops.extend(self.gain_regs());
        ops
    }

    /// 製品で使う設定列: 現在の状態から、停止・ADC 初期化・hard・soft・gain を1回ずつ送る。
    /// libmirisdr の個々の set_* で重なる書き込みを省き、最終的なレジスター値は同じになる。
    pub fn configure_sequence(&mut self) -> Vec<Op> {
        self.open_sequence()
    }
}

/// mirisdr_adc_stop(): USB インターフェースと ADC を休止させる
pub fn adc_stop() -> Vec<Op> {
    vec![Op::Write(0x03, 0x010000)]
}

/// mirisdr_adc_init(): Linux の msi2500 ドライバーと同じ固定の初期化
pub fn adc_init() -> Vec<Op> {
    vec![
        Op::Write(0x08, 0x006080),
        Op::Write(0x05, 0x00000c),
        Op::Write(0x00, 0x000200),
        Op::Write(0x02, 0x004801),
        Op::Write(0x08, 0x00f380),
    ]
}

/// レジスター書き込みを vendor 要求の wValue / wIndex に変換する (libmirisdr reg.c と同じ詰め方)。
/// WinUSB は宛先がエンドポイントの要求で wIndex を検査するため、bmRequestType は宛先=デバイス (0x40) で送る。
pub fn write_setup(reg: u8, val: u32) -> (u16, u16) {
    let value = (((val & 0xff) << 8) | reg as u32) as u16;
    let index = ((val >> 8) & 0xffff) as u16;
    (value, index)
}
