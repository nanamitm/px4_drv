// SPDX-License-Identifier: GPL-3.0-or-later
mod pat;
use isdbt_dsp::StreamingDecoder;
use std::ffi::{c_char, c_void, CStr};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    mpsc, Arc,
};
use std::time::{Duration, Instant};

const MAGIC: u32 = 0x3154_5352;
const MAX_TS: usize = 188 * 2048;
enum Input {
    Ready,
    Iq(Vec<u8>),
    Error(String),
}
#[derive(Default)]
struct Options {
    serial: String,
    frequency: u32,
    gain: i32,
    replay: Option<PathBuf>,
}
fn options() -> Result<Options, String> {
    let mut out = Options {
        gain: 297,
        ..Options::default()
    };
    let mut args = std::env::args_os().skip(1);
    while let Some(key) = args.next() {
        let value = args.next().ok_or("Missing argument value")?;
        match key.to_str() {
            Some("--serial") => out.serial = value.into_string().map_err(|_| "Invalid serial")?,
            Some("--frequency") => {
                out.frequency = value
                    .to_str()
                    .ok_or("Invalid frequency")?
                    .parse()
                    .map_err(|_| "Invalid frequency")?
            }
            Some("--gain") => {
                out.gain = value
                    .to_str()
                    .ok_or("Invalid gain")?
                    .parse()
                    .map_err(|_| "Invalid gain")?
            }
            Some("--replay") => out.replay = Some(value.into()),
            _ => return Err("Unknown argument".into()),
        }
    }
    if out.replay.is_none()
        && (out.serial.is_empty() || !(473_142_000..=707_143_000).contains(&out.frequency))
    {
        return Err("Specify --serial and a UHF channel centre with --frequency".into());
    }
    if !(-600..=600).contains(&out.gain) {
        return Err("Gain must be in tenths of a dB, -600..600".into());
    }
    Ok(out)
}

#[link(name = "kernel32")]
extern "system" {
    fn LoadLibraryExW(name: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
}
type Dev = *mut c_void;
type Count = unsafe extern "C" fn() -> u32;
type Strings = unsafe extern "C" fn(u32, *mut c_char, *mut c_char, *mut c_char) -> i32;
type Open = unsafe extern "C" fn(*mut Dev, u32) -> i32;
type Close = unsafe extern "C" fn(Dev) -> i32;
type DeviceStrings = unsafe extern "C" fn(Dev, *mut c_char, *mut c_char, *mut c_char) -> i32;
type SetU32 = unsafe extern "C" fn(Dev, u32) -> i32;
type SetInt = unsafe extern "C" fn(Dev, i32) -> i32;
type GetInt = unsafe extern "C" fn(Dev) -> i32;
type Gains = unsafe extern "C" fn(Dev, *mut i32) -> i32;
type Cancel = unsafe extern "C" fn(Dev) -> i32;
type Callback = unsafe extern "C" fn(*mut u8, u32, *mut c_void);
type ReadAsync = unsafe extern "C" fn(Dev, Callback, *mut c_void, u32, u32) -> i32;
struct CaptureContext<'a> {
    tx: &'a mpsc::SyncSender<Input>,
    stop: &'a AtomicBool,
    dev: Dev,
    cancel: Cancel,
    error: Option<String>,
}
unsafe extern "C" fn on_iq(data: *mut u8, size: u32, context: *mut c_void) {
    let ctx = &mut *context.cast::<CaptureContext<'_>>();
    if ctx.stop.load(Ordering::Relaxed) {
        (ctx.cancel)(ctx.dev);
        return;
    }
    if size == 0 {
        return;
    }
    if data.is_null() || size > 262_144 || size % 2 != 0 {
        ctx.error = Some("Invalid asynchronous USB I/Q length".into());
        (ctx.cancel)(ctx.dev);
        return;
    }
    let bytes = std::slice::from_raw_parts(data, size as usize).to_vec();
    if let Err(error) = ctx.tx.try_send(Input::Iq(bytes)) {
        ctx.error = Some(format!("I/Q queue full or decoder stopped: {error}"));
        (ctx.cancel)(ctx.dev);
    }
}
struct Library {
    module: *mut c_void,
    dev: Dev,
    close: Option<Close>,
}
impl Drop for Library {
    fn drop(&mut self) {
        unsafe {
            if !self.dev.is_null() {
                if let Some(close) = self.close {
                    close(self.dev);
                }
            }
            FreeLibrary(self.module);
        }
    }
}
impl Library {
    fn new() -> Result<Self, String> {
        use std::os::windows::ffi::OsStrExt;
        let path = std::env::current_exe()
            .map_err(|e| e.to_string())?
            .with_file_name("rtlsdr.dll");
        let name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let module = unsafe { LoadLibraryExW(name.as_ptr(), std::ptr::null_mut(), 0x100 | 0x800) };
        if module.is_null() {
            return Err(format!(
                "Cannot load {}: {}",
                path.display(),
                std::io::Error::last_os_error()
            ));
        }
        Ok(Self {
            module,
            dev: std::ptr::null_mut(),
            close: None,
        })
    }
    fn symbol(&self, name: &[u8]) -> Result<*mut c_void, String> {
        let ptr = unsafe { GetProcAddress(self.module, name.as_ptr()) };
        if ptr.is_null() {
            Err(format!(
                "Missing librtlsdr export: {}",
                String::from_utf8_lossy(name)
            ))
        } else {
            Ok(ptr)
        }
    }
}
macro_rules! symbol {
    ($api:expr,$name:literal,$ty:ty) => {
        unsafe {
            std::mem::transmute::<*mut c_void, $ty>($api.symbol(concat!($name, "\0").as_bytes())?)
        }
    };
}
fn checked(value: i32, what: &str) -> Result<(), String> {
    if value == 0 {
        Ok(())
    } else {
        Err(format!("{what}: librtlsdr error {value}"))
    }
}
fn usb_string(value: &[c_char; 256]) -> String {
    let data: Vec<u8> = value.iter().map(|&v| v as u8).collect();
    CStr::from_bytes_until_nul(&data)
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default()
}
fn capture(opts: Options, tx: &mpsc::SyncSender<Input>, stop: &AtomicBool) -> Result<(), String> {
    let mut api = Library::new()?;
    let count = symbol!(api, "rtlsdr_get_device_count", Count);
    let strings = symbol!(api, "rtlsdr_get_device_usb_strings", Strings);
    let open = symbol!(api, "rtlsdr_open", Open);
    api.close = Some(symbol!(api, "rtlsdr_close", Close));
    let device_strings = symbol!(api, "rtlsdr_get_usb_strings", DeviceStrings);
    let frequency = symbol!(api, "rtlsdr_set_center_freq", SetU32);
    let rate = symbol!(api, "rtlsdr_set_sample_rate", SetU32);
    let mode = symbol!(api, "rtlsdr_set_tuner_gain_mode", SetInt);
    let gain = symbol!(api, "rtlsdr_set_tuner_gain", SetInt);
    let gains = symbol!(api, "rtlsdr_get_tuner_gains", Gains);
    let reset = symbol!(api, "rtlsdr_reset_buffer", GetInt);
    let read = symbol!(api, "rtlsdr_read_async", ReadAsync);
    let cancel = symbol!(api, "rtlsdr_cancel_async", Cancel);
    let mut selected = None;
    for index in 0..unsafe { count() } {
        let mut s = [0; 256];
        if unsafe {
            strings(
                index,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                s.as_mut_ptr(),
            )
        } == 0
            && usb_string(&s) == opts.serial
        {
            if selected.replace(index).is_some() {
                return Err("Duplicate USB serial; disconnect the other RTL-SDR".into());
            }
        }
    }
    let index = selected.ok_or("USB serial not found, unavailable, or in use")?;
    checked(unsafe { open(&mut api.dev, index) }, "Open")?;
    if api.dev.is_null() {
        return Err("Open returned no device".into());
    }
    let mut s = [0; 256];
    checked(
        unsafe {
            device_strings(
                api.dev,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                s.as_mut_ptr(),
            )
        },
        "Read identity",
    )?;
    if usb_string(&s) != opts.serial {
        return Err("USB identity changed while opening".into());
    }
    // レート設定による再選局が 0 Hz を使わないよう、UHF 周波数を先に確定する。
    checked(unsafe { frequency(api.dev, opts.frequency) }, "Tune")?;
    checked(unsafe { rate(api.dev, 1_015_873) }, "Sample rate")?;
    let n = unsafe { gains(api.dev, std::ptr::null_mut()) };
    if !(1..=256).contains(&n) {
        return Err("Invalid tuner gain list".into());
    }
    let mut values = vec![0; n as usize];
    if unsafe { gains(api.dev, values.as_mut_ptr()) } != n {
        return Err("Tuner gain list changed".into());
    }
    let selected_gain = *values
        .iter()
        .min_by_key(|&&v| (v - opts.gain).abs())
        .unwrap();
    checked(unsafe { mode(api.dev, 1) }, "Manual gain")?;
    checked(unsafe { gain(api.dev, selected_gain) }, "Tuner gain")?;
    checked(unsafe { reset(api.dev) }, "Reset buffer")?;
    eprintln!(
        "RTL-SDR serial={} frequency={} rate=1015873 gain={selected_gain}",
        opts.serial, opts.frequency
    );
    tx.send(Input::Ready).map_err(|_| "Output closed")?;
    // 連続入力は librtlsdr 既定の複数 URB で受ける。同期要求の再発行間隔による
    // USB FIFO の空白を避け、取消も callback と同じ USB スレッドで実行する。
    let mut context = CaptureContext {
        tx,
        stop,
        dev: api.dev,
        cancel,
        error: None,
    };
    let result = unsafe {
        read(
            api.dev,
            on_iq,
            (&mut context as *mut CaptureContext<'_>).cast(),
            0,
            262_144,
        )
    };
    if let Some(error) = context.error {
        return Err(error);
    }
    if stop.load(Ordering::Relaxed) {
        return Ok(());
    }
    checked(result, "USB asynchronous I/Q read")?;
    Err("USB stream stopped unexpectedly".into())
}
fn replay(path: &Path, tx: &mpsc::SyncSender<Input>, stop: &AtomicBool) -> Result<(), String> {
    let mut file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    tx.send(Input::Ready).map_err(|_| "Output closed")?;
    while !stop.load(Ordering::Relaxed) {
        let mut data = vec![0; 262_144];
        let n = file.read(&mut data).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        data.truncate(n);
        tx.send(Input::Iq(data)).map_err(|_| "Output closed")?;
    }
    Ok(())
}
fn frame(
    out: &mut impl Write,
    kind: u32,
    ts: &[u8],
    quality: u32,
    accepted: usize,
    total: usize,
) -> Result<(), String> {
    if ts.len() > MAX_TS {
        return Err("Frame too large".into());
    }
    for value in [
        MAGIC,
        kind,
        ts.len() as u32,
        quality,
        accepted.min(u32::MAX as usize) as u32,
        total.saturating_sub(accepted).min(u32::MAX as usize) as u32,
    ] {
        out.write_all(&value.to_le_bytes())
            .map_err(|e| e.to_string())?;
    }
    out.write_all(ts)
        .and_then(|_| out.flush())
        .map_err(|e| e.to_string())
}
fn run() -> Result<(), String> {
    let opts = options()?;
    let is_replay = opts.replay.is_some();
    let stop = Arc::new(AtomicBool::new(false));
    let input_stop = stop.clone();
    std::thread::spawn(move || {
        let mut byte = [0];
        let _ = std::io::stdin().read(&mut byte);
        eprintln!("Frontend stop requested via stdin");
        input_stop.store(true, Ordering::Relaxed);
    });
    // x86 の初期復号で約8.5秒を測定。約16.5秒分を上限とし、その間も USB を継続する。
    // 満杯時に I/Q を捨てるフォールバックは使わない。
    let (tx, rx) = mpsc::sync_channel(128);
    let read_stop = stop.clone();
    let reader = std::thread::spawn(move || {
        let result = if let Some(ref path) = opts.replay {
            replay(path, &tx, &read_stop)
        } else {
            capture(opts, &tx, &read_stop)
        };
        if let Err(error) = result {
            let _ = tx.send(Input::Error(error));
        }
    });
    let result = (|| {
        let mut decoder = StreamingDecoder::new();
        let mut pat = pat::Pat::default();
        let mut out = std::io::stdout().lock();
        let mut last_ts = Instant::now();
        let mut last_iq = Instant::now();
        let mut last_status = Instant::now();
        let mut had_ts = false;
        let started = Instant::now();
        let mut samples = 0u64;
        loop {
            if stop.load(Ordering::Relaxed) {
                break;
            }
            match rx.recv_timeout(Duration::from_millis(250)) {
                Ok(Input::Ready) => frame(&mut out, 1, &[], 0, 0, 0)?,
                Ok(Input::Error(error)) => return Err(error),
                Ok(Input::Iq(bytes)) => {
                    last_iq = Instant::now();
                    samples += bytes.len() as u64 / 2;
                    let ts = decoder.feed(&bytes);
                    let (accepted, total) = decoder.stats();
                    if !ts.is_empty() {
                        if !had_ts {
                            eprintln!(
                                "First TS after {:.3}s, input_samples={samples}",
                                started.elapsed().as_secs_f64()
                            );
                        }
                        let ts = pat.append(&ts)?;
                        let quality = (accepted as u64 * 10_000 / total.max(1) as u64) as u32;
                        for chunk in ts.chunks(MAX_TS) {
                            frame(&mut out, 2, chunk, quality, accepted, total)?;
                        }
                        had_ts = true;
                        last_ts = Instant::now();
                    } else if had_ts && last_ts.elapsed() > Duration::from_secs(5) {
                        eprintln!("TS lost; resetting decoder and PAT state");
                        decoder = StreamingDecoder::new();
                        pat = pat::Pat::default();
                        had_ts = false;
                    }
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    if !is_replay && last_iq.elapsed() > Duration::from_secs(5) {
                        return Err("USB I/Q stalled".into());
                    }
                }
            }
            if last_status.elapsed() >= Duration::from_millis(250) {
                let (accepted, total) = decoder.stats();
                let quality = if had_ts && last_ts.elapsed() < Duration::from_secs(3) {
                    (accepted as u64 * 10_000 / total.max(1) as u64) as u32
                } else {
                    0
                };
                frame(&mut out, 3, &[], quality, accepted, total)?;
                last_status = Instant::now();
            }
        }
        let (accepted, total) = decoder.stats();
        eprintln!("RS accepted={accepted} total={total}");
        Ok(())
    })();
    stop.store(true, Ordering::Relaxed);
    eprintln!("Frontend decoder stopped");
    drop(rx);
    // stdin/USB が停止しない異常時は親の Job Object が期限付きで回収する。
    let deadline = Instant::now() + Duration::from_millis(500);
    while !reader.is_finished() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(10));
    }
    if reader.is_finished() && reader.join().is_err() {
        return Err("USB worker panicked".into());
    }
    result
}
fn main() {
    if let Err(error) = run() {
        eprintln!("RTL-SDR frontend: {error}");
        let _ = frame(
            &mut std::io::stdout().lock(),
            4,
            error.as_bytes().get(..1024).unwrap_or(error.as_bytes()),
            0,
            0,
            0,
        );
        std::process::exit(1);
    }
}
