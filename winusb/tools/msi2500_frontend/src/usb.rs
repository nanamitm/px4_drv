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
/// 同時に投入する転送数。検証環境で 600 秒の取り込みに欠落がなかった値
const TRANSFERS: usize = 32;
pub const BLOCK: usize = 1024;

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
        let path = device_path(guid, index)?;
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
    let mut submitted = 0usize;
    for k in 0..TRANSFERS {
        if let Err(e) = submit(k, k > 0, &mut desc[k], &mut ov[k]) {
            result = Err(UsbError::Win32("WinUsb_ReadIsochPipeAsap", e));
            break;
        }
        submitted += 1;
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
        stats.transfers += 1;
        if !ok {
            stats.transfer_errors += 1;
            consecutive_errors += 1;
            if consecutive_errors > limits.max_consecutive_errors {
                result = Err(UsbError::TransferErrors(consecutive_errors));
                break;
            }
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
                let base = k * xlen + d.Offset as usize;
                let mut o = 0usize;
                while o + BLOCK <= d.Length as usize {
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
        // 失敗した転送の後は連続性が切れるため、新しいストリームとして投入し直す
        if let Err(e) = submit(k, ok, &mut desc[k], &mut ov[k]) {
            if let Err(e2) = submit(k, false, &mut desc[k], &mut ov[k]) {
                let _ = e;
                result = Err(UsbError::DeviceGone(e2));
                break;
            }
        }
        k = (k + 1) % TRANSFERS;
    }

    // 後始末: Linux の msi2500 ドライバーと同じく、先に転送を止めてから停止要求を送り、
    // USB インターフェースと ADC を休止させる (adc_stop)。転送が流れている最中に停止要求を送ると、
    // 長めの取り込みの後で、次のストリーミング開始でデータが届かない状態に入ることがあった
    unsafe { WinUsb_AbortPipe(dev.wh, pi.PipeId) };
    let mut pending = 0usize;
    for o in ov.iter().take(submitted) {
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
