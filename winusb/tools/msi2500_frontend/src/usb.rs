//! WinUSB による MSi2500 の制御と等時転送の取り込み。
//!
//! - 制御転送は bmRequestType 0x40 (vendor、宛先=デバイス) で送る。libmirisdr の 0x42 (宛先=エンドポイント) は、
//!   WinUSB が wIndex の下位バイトを実在のエンドポイントとして検査するため、レジスター値を詰めた書き込みの多くが拒否される。
//! - 取り込みは alternate setting 1 (エンドポイント 0x81、1 マイクロフレームあたり 3×1024 バイト) の等時転送。
//! - USB 抜去後は完了しない転送があるため、終了処理の各待機には期限を設ける。
//! - データの届かない時間と転送の失敗の連続に上限を設け、超えたら失敗として返す。

use crate::regs::{write_setup, Op};
use std::time::{Duration, Instant};
use windows_sys::core::GUID;
use windows_sys::Win32::Devices::DeviceAndDriverInstallation::{
    SetupDiDestroyDeviceInfoList, SetupDiEnumDeviceInterfaces, SetupDiGetClassDevsW,
    SetupDiGetDeviceInterfaceDetailW, DIGCF_DEVICEINTERFACE, DIGCF_PRESENT, SP_DEVICE_INTERFACE_DATA,
    SP_DEVICE_INTERFACE_DETAIL_DATA_W,
};
use windows_sys::Win32::Devices::Usb::{
    WinUsb_AbortPipe, WinUsb_ControlTransfer, WinUsb_Free, WinUsb_GetOverlappedResult, WinUsb_Initialize,
    WinUsb_QueryPipeEx, WinUsb_ReadIsochPipeAsap, WinUsb_RegisterIsochBuffer, WinUsb_SetCurrentAlternateSetting,
    WinUsb_UnregisterIsochBuffer, USBD_ISO_PACKET_DESCRIPTOR, WINUSB_PIPE_INFORMATION_EX, WINUSB_SETUP_PACKET,
};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_IO_PENDING, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE,
    WAIT_OBJECT_0,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OVERLAPPED, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
};
use windows_sys::Win32::System::Threading::{CreateEventW, ResetEvent, WaitForSingleObject};
use windows_sys::Win32::System::IO::OVERLAPPED;

/// SDRplay の WinUSB INF が登録するデバイスインターフェース GUID
pub const SDRPLAY_INTERFACE_GUID: GUID = GUID {
    data1: 0x2D93E8F2,
    data2: 0xBC67,
    data3: 0x4F7D,
    data4: [0x88, 0x91, 0x57, 0x48, 0x30, 0xAA, 0x63, 0x7D],
};

const REQ_WRITE_REG: u8 = 0x41;
const REQ_START: u8 = 0x43;
const REQ_STOP: u8 = 0x45;
/// 1 転送あたりのパケット数 (High-Speed は 8 の倍数が必要)。64 パケット = 8 ms
const PACKETS: usize = 64;
/// 同時に投入する転送数 (1 転送 8 ms)。32 (256 ms 分) では、OFDM 復調と誤り訂正を同時に動かした 600 秒の受信で
/// 投入が間に合わない転送 (USBD_STATUS_ISO_NOT_ACCESSED_LATE) が数回起きたため、先行分を 512 ms に増やした
const TRANSFERS: usize = 64;
pub const BLOCK: usize = 1024;
/// 投入前に転送のバッファーへ書く印。長さが返ってもこの値が残るブロックはドライバーが書かなかったものとして捨てる
const UNWRITTEN: u8 = 0xEE;

#[derive(Debug)]
pub enum UsbError {
    NotFound,
    Win32(&'static str, u32),
    /// 転送の再投入に失敗した (USB 抜去など)
    DeviceGone(u32),
    /// データが届かない時間が上限を超えた
    Stalled(Duration),
    /// 転送の失敗が上限を超えて続いた
    TransferErrors(u32),
}

impl std::fmt::Display for UsbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            UsbError::NotFound => write!(f, "device not found"),
            UsbError::Win32(what, e) => write!(f, "{what} failed (win32 error {e})"),
            UsbError::DeviceGone(e) => write!(f, "transfer resubmit failed (win32 error {e}); device removed?"),
            UsbError::Stalled(d) => write!(f, "no data for {:.1}s", d.as_secs_f64()),
            UsbError::TransferErrors(n) => write!(f, "{n} consecutive transfer errors"),
        }
    }
}

impl std::error::Error for UsbError {}

fn last_error() -> u32 {
    unsafe { GetLastError() }
}

/// WinUSB で開いた MSi2500。
pub struct Device {
    file: HANDLE,
    wh: *mut core::ffi::c_void,
    pub path: String,
}

// ハンドルは1つのスレッドからだけ使うが、取り込みスレッドへ移せるようにする
unsafe impl Send for Device {}

impl Device {
    /// インターフェース GUID の index 番目のデバイスを開く。
    pub fn open(guid: &GUID, index: u32) -> Result<Self, UsbError> {
        Self::open_path(device_path(guid, index)?)
    }

    /// デバイスインターフェースのパスで開く。DriverHost が列挙した個体をそのまま指定するために使う
    /// (この機器はシリアル番号を返さない個体があり、番号での指定は抜き差しで対象が変わる)。
    pub fn open_path(path: String) -> Result<Self, UsbError> {
        let wide: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
        let file = unsafe {
            CreateFileW(
                wide.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                std::ptr::null(),
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                std::ptr::null_mut(),
            )
        };
        if file == INVALID_HANDLE_VALUE {
            return Err(UsbError::Win32("CreateFileW", last_error()));
        }
        let mut wh = std::ptr::null_mut();
        if unsafe { WinUsb_Initialize(file, &mut wh) } == 0 {
            let e = last_error();
            unsafe { CloseHandle(file) };
            return Err(UsbError::Win32("WinUsb_Initialize", e));
        }
        Ok(Self { file, wh, path })
    }

    fn vendor_out(&self, req: u8, value: u16, index: u16) -> Result<(), UsbError> {
        let sp = WINUSB_SETUP_PACKET { RequestType: 0x40, Request: req, Value: value, Index: index, Length: 0 };
        let mut n = 0u32;
        if unsafe { WinUsb_ControlTransfer(self.wh, sp, std::ptr::null_mut(), 0, &mut n, std::ptr::null()) } == 0 {
            return Err(UsbError::Win32("WinUsb_ControlTransfer", last_error()));
        }
        Ok(())
    }

    /// レジスター書き込みと開始・停止の列を送る。
    pub fn apply(&self, ops: &[Op]) -> Result<(), UsbError> {
        for op in ops {
            match *op {
                Op::Write(r, v) => {
                    let (value, index) = write_setup(r, v);
                    self.vendor_out(REQ_WRITE_REG, value, index)?;
                }
                Op::Stop => self.vendor_out(REQ_STOP, 0, 0)?,
                Op::Start => self.vendor_out(REQ_START, 0, 0)?,
            }
        }
        Ok(())
    }

    /// 停止要求。終了処理と失敗時の後始末で使うため、結果は返さない。
    pub fn stop_streaming(&self) {
        let _ = self.vendor_out(REQ_STOP, 0, 0);
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        unsafe {
            WinUsb_Free(self.wh);
            CloseHandle(self.file);
        }
    }
}

fn device_path(guid: &GUID, index: u32) -> Result<String, UsbError> {
    unsafe {
        let di = SetupDiGetClassDevsW(guid, std::ptr::null(), std::ptr::null_mut(), DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if di == INVALID_HANDLE_VALUE as _ {
            return Err(UsbError::Win32("SetupDiGetClassDevsW", last_error()));
        }
        let mut ifd: SP_DEVICE_INTERFACE_DATA = std::mem::zeroed();
        ifd.cbSize = std::mem::size_of::<SP_DEVICE_INTERFACE_DATA>() as u32;
        if SetupDiEnumDeviceInterfaces(di, std::ptr::null(), guid, index, &mut ifd) == 0 {
            SetupDiDestroyDeviceInfoList(di);
            return Err(UsbError::NotFound);
        }
        let mut need = 0u32;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, std::ptr::null_mut(), 0, &mut need, std::ptr::null_mut());
        // 構造体の先頭 (cbSize) は 4 バイト境界に置く
        let mut buf = vec![0u32; (need as usize + 3) / 4 + 1];
        let det = buf.as_mut_ptr() as *mut SP_DEVICE_INTERFACE_DETAIL_DATA_W;
        (*det).cbSize = std::mem::size_of::<SP_DEVICE_INTERFACE_DETAIL_DATA_W>() as u32;
        if SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, std::ptr::null_mut(), std::ptr::null_mut()) == 0 {
            let e = last_error();
            SetupDiDestroyDeviceInfoList(di);
            return Err(UsbError::Win32("SetupDiGetDeviceInterfaceDetailW", e));
        }
        // x86 ではこの構造体が 1 バイト境界に詰められるため、フィールドへの参照を作らずにアドレスだけを取る
        let p = std::ptr::addr_of!((*det).DevicePath) as *const u16;
        let mut len = 0usize;
        while *p.add(len) != 0 {
            len += 1;
        }
        let path = String::from_utf16_lossy(std::slice::from_raw_parts(p, len));
        SetupDiDestroyDeviceInfoList(di);
        Ok(path)
    }
}

/// 取り込みの統計
#[derive(Clone, Debug, Default)]
pub struct CaptureStats {
    pub transfers: u64,
    pub packets: u64,
    pub empty_packets: u64,
    pub bad_packets: u64,
    pub transfer_errors: u64,
    pub blocks: u64,
    /// 長さが BLOCK の倍数でないパケット (端数のバイトはブロックとして扱えない)
    pub odd_packets: u64,
    /// 長さが返ったのに、投入前に書いた印が残っていたブロック (ドライバーがデータを書かなかった)
    pub unwritten_blocks: u64,
    /// 最後に失敗した転送の Win32 エラー (完了待ちの期限切れは 0x1_0000_0102 = WAIT_TIMEOUT を上位に印を付けて記録)
    pub last_error: u64,
}

pub struct CaptureLimits {
    /// データが届かない時間の上限
    pub stall: Duration,
    /// 転送の失敗が続く回数の上限 (32 転送 = 約 0.26 秒)
    pub max_consecutive_errors: u32,
}

impl Default for CaptureLimits {
    fn default() -> Self {
        Self { stall: Duration::from_secs(3), max_consecutive_errors: 64 }
    }
}

/// 設定列を送り、等時転送で取り込む。on_block が false を返すと正常に終了する。
/// 終了時は停止要求、パイプの中止、期限付きの完了待ち、バッファーの登録解除を行う。
pub fn capture(
    dev: &Device,
    setup: &[Op],
    limits: &CaptureLimits,
    stats: &mut CaptureStats,
    mut on_block: impl FnMut(&[u8], &CaptureStats) -> bool,
) -> Result<(), UsbError> {
    dev.apply(setup)?;
    let alt = 1u8;
    if unsafe { WinUsb_SetCurrentAlternateSetting(dev.wh, alt) } == 0 {
        return Err(UsbError::Win32("WinUsb_SetCurrentAlternateSetting", last_error()));
    }
    let mut pi: WINUSB_PIPE_INFORMATION_EX = unsafe { std::mem::zeroed() };
    if unsafe { WinUsb_QueryPipeEx(dev.wh, alt, 0, &mut pi) } == 0 {
        return Err(UsbError::Win32("WinUsb_QueryPipeEx", last_error()));
    }
    let per = pi.MaximumBytesPerInterval as usize;
    let xlen = PACKETS * per;
    let mut buf = vec![0u8; xlen * TRANSFERS].into_boxed_slice();
    let mut bh: *mut core::ffi::c_void = std::ptr::null_mut();
    if unsafe { WinUsb_RegisterIsochBuffer(dev.wh, pi.PipeId, buf.as_mut_ptr(), (xlen * TRANSFERS) as u32, &mut bh) } == 0 {
        return Err(UsbError::Win32("WinUsb_RegisterIsochBuffer", last_error()));
    }
    let mut desc: Vec<[USBD_ISO_PACKET_DESCRIPTOR; PACKETS]> = vec![unsafe { std::mem::zeroed() }; TRANSFERS];
    let mut ov: Vec<OVERLAPPED> = vec![unsafe { std::mem::zeroed() }; TRANSFERS];
    for o in ov.iter_mut() {
        o.hEvent = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
    }
    let submit = |k: usize, cont: bool, desc: &mut [USBD_ISO_PACKET_DESCRIPTOR; PACKETS], o: &mut OVERLAPPED| -> Result<(), u32> {
        unsafe { ResetEvent(o.hEvent) };
        // 遅れた転送を新しいストリームとして投入し直した後、ハードウェアが触れないパケットの記述子が
        // 前回の完了時の長さと状態のまま残り、数秒前のデータを新しいデータとして読む状態が約 1 秒続いた。
        // 投入のたびに長さと状態を消し、触れられなかったパケットを空として扱う
        for d in desc.iter_mut() {
            d.Length = 0;
            d.Status = 0;
        }
        let ok = unsafe {
            WinUsb_ReadIsochPipeAsap(bh, (k * xlen) as u32, xlen as u32, cont as i32, PACKETS as u32, desc.as_mut_ptr(), o)
        };
        if ok == 0 {
            let e = last_error();
            if e != ERROR_IO_PENDING {
                return Err(e);
            }
        }
        Ok(())
    };

    let mut result: Result<(), UsbError> = Ok(());
    // ドライバーへ投入し、まだ完了を受け取っていない転送
    let mut inflight = [false; TRANSFERS];
    buf.fill(UNWRITTEN);
    for k in 0..TRANSFERS {
        if let Err(e) = submit(k, k > 0, &mut desc[k], &mut ov[k]) {
            result = Err(UsbError::Win32("WinUsb_ReadIsochPipeAsap", e));
            break;
        }
        inflight[k] = true;
    }
    if result.is_ok() {
        result = dev.apply(&[Op::Start]);
    }
    let mut last_data = Instant::now();
    let mut consecutive_errors = 0u32;
    let mut k = 0usize;
    'run: while result.is_ok() {
        // 完了待ちにも期限を設け、データ途絶を検出できるようにする
        let w = unsafe { WaitForSingleObject(ov[k].hEvent, 1000) };
        let mut n = 0u32;
        let ok = w == WAIT_OBJECT_0 && unsafe { WinUsb_GetOverlappedResult(dev.wh, &ov[k], &mut n, 0) } != 0;
        if w == WAIT_OBJECT_0 {
            inflight[k] = false;
        }
        stats.transfers += 1;
        if !ok {
            stats.last_error = if w == WAIT_OBJECT_0 { last_error() as u64 } else { 0x1_0000_0000 | w as u64 };
            // 連続する失敗の最初の数回だけ記録する (投入し直した後も失敗するかを見分ける)
            if consecutive_errors < 3 {
                let bad = desc[k].iter().filter(|d| d.Status != 0).count();
                eprintln!("[usb] transfer {k} failed: error=0x{:x} bad_packets_in_transfer={bad} first_status=0x{:x}",
                    stats.last_error, desc[k].iter().map(|d| d.Status).find(|&s| s != 0).unwrap_or(0));
            }
            stats.transfer_errors += 1;
            consecutive_errors += 1;
            if consecutive_errors > limits.max_consecutive_errors {
                result = Err(UsbError::TransferErrors(consecutive_errors));
                break;
            }
            if last_data.elapsed() > limits.stall {
                result = Err(UsbError::Stalled(last_data.elapsed()));
                break;
            }
            if w != WAIT_OBJECT_0 {
                // 完了していない転送のバッファーはドライバーが使っているため、投入し直さずに完了を待ち続ける
                continue 'run;
            }
            // 完了した転送の失敗 (検証環境では USBD_STATUS_ISO_NOT_ACCESSED_LATE、投入が間に合わなかった) の後は
            // 連続性が切れるため、この転送だけを新しいストリームとして投入し直す。
            // パイプを止めて全転送を投入し直す方法は、以後の転送がすべて ERROR_BUSY で失敗し続けたため採用しなかった
            buf[k * xlen..(k + 1) * xlen].fill(UNWRITTEN);
            match submit(k, false, &mut desc[k], &mut ov[k]) {
                Ok(()) => inflight[k] = true,
                Err(e) => {
                    result = Err(UsbError::DeviceGone(e));
                    break 'run;
                }
            }
            k = (k + 1) % TRANSFERS;
            continue 'run;
        } else {
            consecutive_errors = 0;
            for d in desc[k].iter() {
                stats.packets += 1;
                if d.Status != 0 {
                    stats.bad_packets += 1;
                    continue;
                }
                if d.Length == 0 {
                    stats.empty_packets += 1;
                    continue;
                }
                if d.Length as usize % BLOCK != 0 {
                    stats.odd_packets += 1;
                    if stats.odd_packets <= 20 {
                        eprintln!("[usb] packet length {} is not a multiple of {BLOCK} (transfer {}, packet {})",
                            d.Length, stats.transfers, desc[k].iter().position(|x| std::ptr::eq(x, d)).unwrap_or(0));
                    }
                }
                let base = k * xlen + d.Offset as usize;
                let mut o = 0usize;
                while o + BLOCK <= d.Length as usize {
                    let blk = &buf[base + o..base + o + BLOCK];
                    if blk[4..8] == [UNWRITTEN; 4] {
                        // ヘッダーの 5〜8 バイト目は実データでは 0x26, x, 0, 0 で、印の値にはならない
                        stats.unwritten_blocks += 1;
                        o += BLOCK;
                        continue;
                    }
                    stats.blocks += 1;
                    last_data = Instant::now();
                    if !on_block(&buf[base + o..base + o + BLOCK], stats) {
                        break 'run;
                    }
                    o += BLOCK;
                }
            }
        }
        if last_data.elapsed() > limits.stall {
            result = Err(UsbError::Stalled(last_data.elapsed()));
            break;
        }
        buf[k * xlen..(k + 1) * xlen].fill(UNWRITTEN);
        if submit(k, true, &mut desc[k], &mut ov[k]).is_err() {
            // 続きとして投入できなければ新しいストリームとして投入する。抜去などでそれも失敗すると DeviceGone で終わる
            if let Err(e) = submit(k, false, &mut desc[k], &mut ov[k]) {
                result = Err(UsbError::DeviceGone(e));
                break;
            }
        }
        inflight[k] = true;
        k = (k + 1) % TRANSFERS;
    }

    // 後始末: Linux の msi2500 ドライバーと同じく、先に転送を止めてから停止要求を送り、
    // USB インターフェースと ADC を休止させる (adc_stop)。転送が流れている最中に停止要求を送ると、
    // 長めの取り込みの後で、次のストリーミング開始でデータが届かない状態に入ることがあった
    unsafe { WinUsb_AbortPipe(dev.wh, pi.PipeId) };
    let mut pending = 0usize;
    for (o, _) in ov.iter().zip(inflight.iter()).filter(|(_, &f)| f) {
        if unsafe { WaitForSingleObject(o.hEvent, 1000) } == WAIT_OBJECT_0 {
            let mut n = 0u32;
            unsafe { WinUsb_GetOverlappedResult(dev.wh, o, &mut n, 0) };
        } else {
            pending += 1;
        }
    }
    // 転送の停止から停止要求まで、Linux ドライバーと同じく 20 ms あける
    std::thread::sleep(Duration::from_millis(20));
    dev.stop_streaming();
    let _ = dev.apply(&crate::regs::adc_stop());
    unsafe { WinUsb_UnregisterIsochBuffer(bh) };
    if pending > 0 {
        // 完了しない転送が後からバッファーとイベントへ書き込む可能性があるため、解放せずに残す
        eprintln!("[usb] {pending} transfers did not complete after abort; leaking their buffers");
        std::mem::forget(buf);
        std::mem::forget(desc);
        std::mem::forget(ov);
    } else {
        for o in ov.iter() {
            unsafe { CloseHandle(o.hEvent) };
        }
    }
    result
}
