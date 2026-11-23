//! K=7 (G1=0o171, G2=0o133) レート 1/2 のビタビ。整数メトリックとブロック並列。
//!
//! 状態遷移は isdbt-dsp の viterbi.rs と同じ: reg=(u<<6)|s, 次状態 ns=reg>>1。
//! したがって次状態 ns の前状態は s=((ns<<1)&0x3f)|b (b=0,1)、入力ビットは u=ns>>5。
//! 出力は「前後 overlap ステップの助走を付けたブロック」を独立に復号して得る。
//! 助走の長さが十分なら、通しの復号と同じビット列になる (検証は呼び出し側の試験で行う)。

const G1: u16 = 0o171;
const G2: u16 = 0o133;

fn parity(x: u16) -> i16 {
    (x.count_ones() & 1) as i16
}

/// 前状態 2j+b から入力 u で遷移するときの出力 (X, Y) を ±1 で表した表。[u][b][j]
pub struct Tables {
    sx: [[[i16; 32]; 2]; 2],
    sy: [[[i16; 32]; 2]; 2],
}

impl Tables {
    pub fn new() -> Self {
        let mut sx = [[[0i16; 32]; 2]; 2];
        let mut sy = [[[0i16; 32]; 2]; 2];
        for u in 0..2usize {
            for b in 0..2usize {
                for j in 0..32usize {
                    let s = (2 * j + b) as u16;
                    let reg = ((u as u16) << 6) | s;
                    sx[u][b][j] = if parity(reg & G1) == 1 { 1 } else { -1 };
                    sy[u][b][j] = if parity(reg & G2) == 1 { 1 } else { -1 };
                }
            }
        }
        Self { sx, sy }
    }
}

/// 軟判定値 (正で bit=1) を i8 へ量子化する。scale は 1.0 の軟値に対応する整数値。
pub fn quantize(soft: &[f32], scale: f32) -> Vec<i8> {
    soft.iter().map(|&x| (x * scale).round().clamp(-127.0, 127.0) as i8).collect()
}

/// 前進計算の本体。steps 個の (r1, r2) から各ステップの判定を dec に書き、最終メトリックを返す。
/// 判定は 1 ステップを u64 にまとめ、ビット ns が次状態 ns の判定 (0/1) を表す。状態ごとに 1 バイトで持つと
/// 1 ブロック (8,576 ステップ) で約 550 KB になり、4 コアで L2 2 MB を共有する Intel N150 では
/// 作業スレッドどうしと前段・OFDM がキャッシュを取り合って遅くなった
#[inline(always)]
fn forward_body(t: &Tables, q: &[i8], dec: &mut [u64]) -> [i16; 64] {
    let steps = q.len() / 2;
    let mut m = [0i16; 64];
    for st in 0..steps {
        let r1 = q[2 * st] as i16;
        let r2 = q[2 * st + 1] as i16;
        let mut me = [0i16; 32];
        let mut mo = [0i16; 32];
        for j in 0..32 {
            me[j] = m[2 * j];
            mo[j] = m[2 * j + 1];
        }
        let mut d = [0u8; 64];
        let mut nm = [0i16; 64];
        for u in 0..2 {
            let sx0 = &t.sx[u][0];
            let sy0 = &t.sy[u][0];
            let sx1 = &t.sx[u][1];
            let sy1 = &t.sy[u][1];
            for j in 0..32 {
                let c0 = me[j].wrapping_add(sx0[j] * r1).wrapping_add(sy0[j] * r2);
                let c1 = mo[j].wrapping_add(sx1[j] * r1).wrapping_add(sy1[j] * r2);
                let pick = c1 > c0;
                nm[u * 32 + j] = if pick { c1 } else { c0 };
                d[u * 32 + j] = pick as u8;
            }
        }
        // 1ステップの増分は最大 254。16 ステップごとに状態 0 のメトリックを引いて i16 の範囲に保つ
        if st % 16 == 15 {
            let base = nm[0];
            for v in nm.iter_mut() {
                *v = v.wrapping_sub(base);
            }
        }
        let mut bits = 0u64;
        for (k, &b) in d.iter().enumerate() {
            bits |= (b as u64) << k;
        }
        dec[st] = bits;
        m = nm;
    }
    m
}

/// forward_body と同じ計算 (i16 の折り返し加算、c1 > c0 で c1 を選ぶ、16 ステップごとの正規化) を AVX2 で行う。
/// 自動ベクトル化に任せた forward_body は Intel N150 で 1 ステップ約 130 サイクルかかり、B 階層のビタビだけで
/// 1 コアの約 0.75 倍を使って、OFDM と前段と合わせると実時間に間に合わなかった
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[target_feature(enable = "avx2")]
unsafe fn forward_avx2(t: &Tables, q: &[i8], dec: &mut [u64]) -> [i16; 64] {
    #[cfg(target_arch = "x86")]
    use std::arch::x86::*;
    #[cfg(target_arch = "x86_64")]
    use std::arch::x86_64::*;
    let steps = q.len() / 2;
    // G1 と G2 はどちらも最下位 (前状態の b) と最上位 (入力 u) のタップを持つため、b か u を反転すると出力の符号が
    // 反転する: sx[u][b][j] = (-1)^(u+b) sx[0][0][j] (sy も同じ)。i16 の折り返し演算では -(x + y) = (-x) + (-y) なので、
    // 枝メトリック bm = sx[0][0]·r1 + sy[0][0]·r2 の符号を変えるだけで forward_body と同じ値になる
    let mut sx = [_mm256_setzero_si256(); 2];
    let mut sy = [_mm256_setzero_si256(); 2];
    for h in 0..2 {
        sx[h] = _mm256_loadu_si256(t.sx[0][0].as_ptr().add(16 * h) as *const __m256i);
        sy[h] = _mm256_loadu_si256(t.sy[0][0].as_ptr().add(16 * h) as *const __m256i);
    }
    // 128 ビットの各半分の中で、i16 の偶数番目を前に、奇数番目を後ろに集める
    let split = _mm256_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15, 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15);
    // m[16k..16k+16] を mv[k] に持つ
    let mut mv = [_mm256_setzero_si256(); 4];
    for st in 0..steps {
        let r1 = _mm256_set1_epi16(q[2 * st] as i16);
        let r2 = _mm256_set1_epi16(q[2 * st + 1] as i16);
        // me[j] = m[2j]、mo[j] = m[2j+1] を、j の前半 (h=0) と後半 (h=1) のベクトルにする
        let mut me = [_mm256_setzero_si256(); 2];
        let mut mo = [_mm256_setzero_si256(); 2];
        for h in 0..2 {
            let a = _mm256_permute4x64_epi64::<0b11_01_10_00>(_mm256_shuffle_epi8(mv[2 * h], split));
            let b = _mm256_permute4x64_epi64::<0b11_01_10_00>(_mm256_shuffle_epi8(mv[2 * h + 1], split));
            me[h] = _mm256_permute2x128_si256::<0x20>(a, b);
            mo[h] = _mm256_permute2x128_si256::<0x31>(a, b);
        }
        let mut nm = [_mm256_setzero_si256(); 4];
        let mut pick = [_mm256_setzero_si256(); 4];
        for h in 0..2 {
            // 表の値は ±1 で 0 を含まないため、sign は掛け算と同じ値を返す
            let bm = _mm256_add_epi16(_mm256_sign_epi16(r1, sx[h]), _mm256_sign_epi16(r2, sy[h]));
            for u in 0..2 {
                let (c0, c1) = if u == 0 {
                    (_mm256_add_epi16(me[h], bm), _mm256_sub_epi16(mo[h], bm))
                } else {
                    (_mm256_sub_epi16(me[h], bm), _mm256_add_epi16(mo[h], bm))
                };
                pick[2 * u + h] = _mm256_cmpgt_epi16(c1, c0);
                nm[2 * u + h] = _mm256_max_epi16(c0, c1);
            }
        }
        // 判定のマスクを i8 に詰めて、ビット u*32+j が次状態 u*32+j の判定になるよう並べ替える
        let lo = _mm256_permute4x64_epi64::<0b11_01_10_00>(_mm256_packs_epi16(pick[0], pick[1]));
        let hi = _mm256_permute4x64_epi64::<0b11_01_10_00>(_mm256_packs_epi16(pick[2], pick[3]));
        dec[st] = (_mm256_movemask_epi8(lo) as u32 as u64) | ((_mm256_movemask_epi8(hi) as u32 as u64) << 32);
        if st % 16 == 15 {
            let base = _mm256_broadcastw_epi16(_mm256_castsi256_si128(nm[0]));
            for v in nm.iter_mut() {
                *v = _mm256_sub_epi16(*v, base);
            }
        }
        mv = nm;
    }
    let mut m = [0i16; 64];
    for k in 0..4 {
        _mm256_storeu_si256(m.as_mut_ptr().add(16 * k) as *mut __m256i, mv[k]);
    }
    m
}

#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
unsafe fn forward_avx2(t: &Tables, q: &[i8], dec: &mut [u64]) -> [i16; 64] {
    forward_body(t, q, dec)
}

fn forward_base(t: &Tables, q: &[i8], dec: &mut [u64]) -> [i16; 64] {
    forward_body(t, q, dec)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Isa {
    Base,
    Avx2,
}

pub fn best_isa() -> Isa {
    if is_x86_feature_detected!("avx2") { Isa::Avx2 } else { Isa::Base }
}

/// 1区間を前進計算し、末尾で最良の状態から逆追跡して入力ビットを返す。
fn decode_span(t: &Tables, q: &[i8], isa: Isa, dec: &mut Vec<u64>) -> Vec<u8> {
    let steps = q.len() / 2;
    dec.resize(steps, 0);
    let m = match isa {
        Isa::Avx2 => unsafe { forward_avx2(t, q, dec) },
        Isa::Base => forward_base(t, q, dec),
    };
    // メトリックの比較は wrapping 演算の差で行う (範囲は正規化で保たれている)
    let mut ns = 0usize;
    for s in 1..64 {
        if m[s].wrapping_sub(m[ns]) > 0 {
            ns = s;
        }
    }
    let mut bits = vec![0u8; steps];
    for st in (0..steps).rev() {
        bits[st] = (ns >> 5) as u8;
        let b = ((dec[st] >> ns) & 1) as usize;
        ns = ((ns << 1) & 0x3f) | b;
    }
    bits
}

/// ブロック並列で復号する。q は (r1, r2) の並び。block は出力ステップ数、overlap は前後の助走。
pub fn decode_parallel(q: &[i8], block: usize, overlap: usize, threads: usize, isa: Isa) -> Vec<u8> {
    let t = Tables::new();
    decode_region(&t, q, 0, q.len() / 2, block, overlap, threads, isa)
}

/// q の中の出力区間 [out_start, out_end) を、out_start から block ごとに区切って復号する。
/// 各ブロックの助走は q の範囲 [0, q.len()/2) で切り詰める。
fn decode_region(t: &Tables, q: &[i8], out_start: usize, out_end: usize, block: usize, overlap: usize,
                 threads: usize, isa: Isa) -> Vec<u8> {
    let steps = q.len() / 2;
    let n = out_end - out_start;
    let mut out = vec![0u8; n];
    let nblk = n.div_ceil(block);
    // ブロックは空いたスレッドが順に取る。最初に均等に割り振ると、同時に動く OFDM や前段に CPU を取られた
    // スレッドを全員が待ち、コア数に余裕のない Intel N150 では受信が実時間に追いつかなかった
    let it = std::sync::Mutex::new(out.chunks_mut(block).enumerate());
    std::thread::scope(|sc| {
        for _ in 0..threads.max(1).min(nblk) {
            let it = &it;
            sc.spawn(move || {
                let mut dec = Vec::new();
                loop {
                    let Some((bi, dst)) = it.lock().unwrap().next() else { break };
                    let s0 = out_start + bi * block;
                    let a = s0.saturating_sub(overlap);
                    let e = (s0 + dst.len() + overlap).min(steps);
                    let bits = decode_span(t, &q[2 * a..2 * e], isa, &mut dec);
                    dst.copy_from_slice(&bits[s0 - a..s0 - a + dst.len()]);
                }
            });
        }
    });
    out
}

/// 逐次投入用の復号器。後ろの助走まで揃ったブロックを、スレッド数×4 個ずつまとめて並列に復号する。
/// ブロック境界と助走の切り詰めは decode_parallel と同じため、同じ入力なら出力も一致する。
pub struct StreamDecoder {
    t: Tables,
    block: usize,
    overlap: usize,
    threads: usize,
    isa: Isa,
    batch: usize,
    /// buf[0] に当たる全体のステップ番号
    base: usize,
    /// 次に出力するステップ番号 (常に block の倍数)
    next_out: usize,
    buf: Vec<i8>,
    max_buf: usize,
}

impl StreamDecoder {
    pub fn new(block: usize, overlap: usize, threads: usize, isa: Isa) -> Self {
        let threads = threads.max(1);
        Self { t: Tables::new(), block, overlap, threads, isa, batch: threads * 4, base: 0, next_out: 0, buf: Vec::new(), max_buf: 0 }
    }
    pub fn batch_blocks(&self) -> usize {
        self.batch
    }
    pub fn max_buffered(&self) -> usize {
        self.max_buf
    }
    fn have(&self) -> usize {
        self.base + self.buf.len() / 2
    }
    fn run(&mut self, out_end: usize) -> Vec<u8> {
        let lo = self.next_out - self.base;
        let hi = out_end - self.base;
        let bits = decode_region(&self.t, &self.buf, lo, hi, self.block, self.overlap, self.threads, self.isa);
        self.next_out = out_end;
        // 次のブロックの前の助走だけ残して捨てる
        let keep_from = self.next_out.saturating_sub(self.overlap).max(self.base);
        let drop_steps = keep_from - self.base;
        self.buf.drain(0..2 * drop_steps);
        self.base = keep_from;
        bits
    }
    /// (r1, r2) の並びを追加し、復号できた分のビットを返す。
    pub fn push(&mut self, q: &[i8]) -> Vec<u8> {
        self.buf.extend_from_slice(q);
        self.max_buf = self.max_buf.max(self.buf.len() / 2);
        let mut out = Vec::new();
        loop {
            let span = self.batch * self.block;
            if self.have() < self.next_out + span + self.overlap {
                break;
            }
            let end = self.next_out + span;
            out.extend(self.run(end));
        }
        out
    }
    /// 入力の終わり。残りを後ろの助走なしで復号する。
    pub fn finish(&mut self) -> Vec<u8> {
        let end = self.have();
        if end <= self.next_out {
            return Vec::new();
        }
        self.run(end)
    }
}

struct PoolJob {
    id: u64,
    /// 助走を含むブロックの入力
    q: Vec<i8>,
    /// 出力の先頭 (前の助走の長さ) と出力の長さ (ステップ数)
    skip: usize,
    len: usize,
}

/// 常駐の作業スレッドでブロックを復号する逐次投入用の復号器。ブロックの入力 (後ろの助走まで) が揃うたびに
/// 作業スレッドへ渡し、復号の終わったブロックを順に返す。ブロック境界と助走は StreamDecoder と同じため、
/// 同じ入力なら同じビット列を返す (返す時点だけが異なる)。
/// StreamDecoder は スレッド数×4 個のブロックごとに作業スレッドを起動して全員の終了を待つため、同時に動く
/// OFDM や前段に CPU を取られたスレッドを全員が待ち、コア数に余裕のない Intel N150 では受信が実時間に追いつかなかった
pub struct PoolDecoder {
    block: usize,
    overlap: usize,
    /// 同時に復号するブロックの上限 (出力の遅れの上限にもなる)
    max_inflight: u64,
    /// buf[0] に当たる全体のステップ番号
    base: usize,
    /// 次に作業スレッドへ渡すブロックの先頭のステップ番号
    next_job: usize,
    buf: Vec<i8>,
    jobs: Option<std::sync::mpsc::SyncSender<PoolJob>>,
    results: std::sync::mpsc::Receiver<(u64, Vec<u8>)>,
    workers: Vec<std::thread::JoinHandle<()>>,
    pending: std::collections::BTreeMap<u64, Vec<u8>>,
    next_id: u64,
    next_emit: u64,
}

impl PoolDecoder {
    /// name は作業スレッドの名前の先頭
    pub fn new(block: usize, overlap: usize, threads: usize, isa: Isa, name: &str) -> Self {
        let threads = threads.max(1);
        let max_inflight = threads as u64 * 4;
        let (job_tx, job_rx) = std::sync::mpsc::sync_channel::<PoolJob>(max_inflight as usize);
        let (res_tx, res_rx) = std::sync::mpsc::channel();
        let job_rx = std::sync::Arc::new(std::sync::Mutex::new(job_rx));
        let workers = (0..threads)
            .map(|i| {
                let (job_rx, res_tx) = (job_rx.clone(), res_tx.clone());
                std::thread::Builder::new()
                    .name(format!("{name}-{i}"))
                    .spawn(move || {
                        let t = Tables::new();
                        let mut dec = Vec::new();
                        loop {
                            let job = match job_rx.lock().unwrap().recv() {
                                Ok(j) => j,
                                Err(_) => break,
                            };
                            let bits = decode_span(&t, &job.q, isa, &mut dec);
                            if res_tx.send((job.id, bits[job.skip..job.skip + job.len].to_vec())).is_err() {
                                break;
                            }
                        }
                    })
                    .expect("spawn viterbi worker")
            })
            .collect();
        Self {
            block,
            overlap,
            max_inflight,
            base: 0,
            next_job: 0,
            buf: Vec::new(),
            jobs: Some(job_tx),
            results: res_rx,
            workers,
            pending: std::collections::BTreeMap::new(),
            next_id: 0,
            next_emit: 0,
        }
    }

    fn take_ready(&mut self, out: &mut Vec<u8>) {
        while let Some(bits) = self.pending.remove(&self.next_emit) {
            out.extend_from_slice(&bits);
            self.next_emit += 1;
        }
    }

    /// 作業スレッドへ渡したブロックの復号がすべて終わるまで待ち、まだ返していない分のビットを返す。
    /// 返す時点がスレッドの進み具合で変わっても、入力の終わりや状態の作り直しで出力が変わらないよう、
    /// 呼び出し側はその前にこれを呼ぶ (後ろの助走が揃わず渡していないブロックは含まない)
    pub fn drain(&mut self) -> Vec<u8> {
        let mut out = Vec::new();
        while self.next_emit < self.next_id {
            while !self.pending.contains_key(&self.next_emit) {
                let (id, bits) = self.results.recv().expect("viterbi worker stopped");
                self.pending.insert(id, bits);
            }
            self.take_ready(&mut out);
        }
        out
    }

    /// (r1, r2) の並びを追加し、復号の終わった分のビットを返す。
    pub fn push(&mut self, q: &[i8]) -> Vec<u8> {
        self.buf.extend_from_slice(q);
        let mut out = Vec::new();
        while self.base + self.buf.len() / 2 >= self.next_job + self.block + self.overlap {
            // 同時に復号するブロックが上限に達したら、最も古いブロックが終わるまで待つ
            while self.next_id - self.next_emit >= self.max_inflight {
                while !self.pending.contains_key(&self.next_emit) {
                    let (id, bits) = self.results.recv().expect("viterbi worker stopped");
                    self.pending.insert(id, bits);
                }
                self.take_ready(&mut out);
            }
            let s0 = self.next_job;
            let a = s0.saturating_sub(self.overlap);
            let e = s0 + self.block + self.overlap;
            let job = PoolJob { id: self.next_id, q: self.buf[2 * (a - self.base)..2 * (e - self.base)].to_vec(), skip: s0 - a, len: self.block };
            self.jobs.as_ref().expect("pool open").send(job).expect("viterbi worker stopped");
            self.next_id += 1;
            self.next_job += self.block;
            // 次のブロックの前の助走だけ残して捨てる
            let keep_from = self.next_job.saturating_sub(self.overlap).max(self.base);
            self.buf.drain(0..2 * (keep_from - self.base));
            self.base = keep_from;
        }
        while let Ok((id, bits)) = self.results.try_recv() {
            self.pending.insert(id, bits);
        }
        self.take_ready(&mut out);
        out
    }
}

impl Drop for PoolDecoder {
    fn drop(&mut self) {
        // 投入口を閉じ、作業スレッドが残りの仕事を終えて抜けるのを待つ
        self.jobs = None;
        while self.results.recv().is_ok() {}
        for h in self.workers.drain(..) {
            let _ = h.join();
        }
    }
}

#[cfg(all(test, any(target_arch = "x86", target_arch = "x86_64")))]
mod tests {
    use super::*;

    /// forward_avx2 が前提にする、表の符号の対称性
    #[test]
    fn tables_are_sign_symmetric() {
        let t = Tables::new();
        for u in 0..2 {
            for b in 0..2 {
                let s = if (u + b) % 2 == 0 { 1 } else { -1 };
                for j in 0..32 {
                    assert_eq!(t.sx[u][b][j], s * t.sx[0][0][j]);
                    assert_eq!(t.sy[u][b][j], s * t.sy[0][0][j]);
                }
            }
        }
    }

    /// AVX2 版の前進計算が、各ステップの判定と最終メトリックまで基本版と一致すること
    #[test]
    fn forward_avx2_matches_base_decisions() {
        if !is_x86_feature_detected!("avx2") {
            return;
        }
        let t = Tables::new();
        let mut seed = 0x1234_5678_9abc_def0u64;
        let mut next = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            (seed >> 33) as u32
        };
        // 乱数、飽和に近い値、0 の並び (同点の判定) を混ぜる
        let mut q: Vec<i8> = (0..2 * 20_000).map(|_| ((next() % 255) as i32 - 127) as i8).collect();
        for (i, v) in q.iter_mut().enumerate() {
            match (i / 512) % 4 {
                1 => *v = if next() & 1 == 0 { 127 } else { -127 },
                2 => *v = 0,
                _ => {}
            }
        }
        let steps = q.len() / 2;
        let (mut d0, mut d1) = (vec![0u64; steps], vec![0u64; steps]);
        let m0 = forward_base(&t, &q, &mut d0);
        let m1 = unsafe { forward_avx2(&t, &q, &mut d1) };
        assert_eq!(m0, m1);
        assert_eq!(d0, d1);
    }
}
