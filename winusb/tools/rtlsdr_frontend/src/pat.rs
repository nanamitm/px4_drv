// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

pub fn crc(data: &[u8]) -> u32 {
    let mut value = 0xffff_ffffu32;
    for &byte in data {
        value ^= (byte as u32) << 24;
        for _ in 0..8 {
            value = (value << 1)
                ^ if value & 0x8000_0000 != 0 {
                    0x04c1_1db7
                } else {
                    0
                };
        }
    }
    value
}

#[derive(Default)]
struct Section {
    data: Vec<u8>,
    cc: Option<u8>,
}
impl Section {
    fn bytes(&mut self, bytes: &[u8], result: &mut Vec<Vec<u8>>, allow_new: bool) {
        let mut pos = 0;
        while pos < bytes.len() {
            if self.data.is_empty() && (!allow_new || bytes[pos] == 0xff) {
                break;
            }
            let target = if self.data.len() < 3 {
                3
            } else {
                3 + (((self.data[1] & 15) as usize) << 8 | self.data[2] as usize)
            };
            if !(3..=1024).contains(&target) {
                self.data.clear();
                break;
            }
            let take = (target - self.data.len()).min(bytes.len() - pos);
            self.data.extend_from_slice(&bytes[pos..pos + take]);
            pos += take;
            if self.data.len() >= 3 {
                let size = 3 + (((self.data[1] & 15) as usize) << 8 | self.data[2] as usize);
                if size > 1024 || size < 8 {
                    self.data.clear();
                    break;
                }
                if self.data.len() == size {
                    if crc(&self.data) == 0 {
                        result.push(std::mem::take(&mut self.data));
                    } else {
                        self.data.clear();
                    }
                    if !allow_new {
                        break;
                    }
                }
            }
        }
    }
    fn feed(&mut self, p: &[u8]) -> Vec<Vec<u8>> {
        let mut result = Vec::new();
        let afc = (p[3] >> 4) & 3;
        if p[1] & 0x80 != 0 || p[3] & 0xc0 != 0 || afc == 0 {
            self.data.clear();
            self.cc = None;
            return result;
        }
        let mut pos = 4;
        if afc & 2 != 0 {
            let size = p[4] as usize;
            if size > 183 {
                self.data.clear();
                self.cc = None;
                return result;
            }
            if size > 0 && p[5] & 0x80 != 0 {
                self.data.clear();
                self.cc = None;
            }
            pos = 5 + size;
        }
        if afc & 1 == 0 || pos >= 188 {
            return result;
        }
        let cc = p[3] & 15;
        if let Some(last) = self.cc {
            if cc == last {
                return result;
            }
            if cc != (last + 1) & 15 {
                self.data.clear();
            }
        }
        self.cc = Some(cc);
        if p[1] & 0x40 != 0 {
            let pointer = p[pos] as usize;
            pos += 1;
            if pointer > 188 - pos {
                self.data.clear();
                return result;
            }
            if !self.data.is_empty() {
                self.bytes(&p[pos..pos + pointer], &mut result, false);
            }
            self.data.clear();
            self.bytes(&p[pos + pointer..], &mut result, true);
        } else if !self.data.is_empty() {
            self.bytes(&p[pos..], &mut result, true);
        }
        result
    }
}

#[derive(Default)]
pub struct Pat {
    sections: BTreeMap<u16, Section>,
    programs: BTreeMap<u16, u16>,
    tsid: Option<u16>,
    broadcast: bool,
    cc: u8,
    version: u8,
    since: usize,
}
impl Pat {
    pub fn append(&mut self, input: &[u8]) -> Result<Vec<u8>, String> {
        if input.len() % 188 != 0 {
            return Err("Decoder returned unaligned TS".into());
        }
        let mut out = Vec::with_capacity(input.len() + 188 * (input.len() / 188 / 64 + 1));
        for p in input.chunks_exact(188) {
            if p[0] != 0x47 || p[1] & 0x80 != 0 {
                return Err("Decoder returned invalid TS".into());
            }
            let pid = (((p[1] & 31) as u16) << 8) | p[2] as u16;
            if pid == 0 || pid == 0x11 || (0x1fc8..=0x1fcf).contains(&pid) {
                for section in self.sections.entry(pid).or_default().feed(p) {
                    if section[5] & 1 == 0 {
                        continue;
                    }
                    if pid == 0 && section[0] == 0 {
                        self.broadcast = true;
                    }
                    if pid == 0x11 && section[0] == 0x42 && section.len() >= 15 {
                        let tsid = u16::from_be_bytes([section[3], section[4]]);
                        if self.tsid != Some(tsid) {
                            self.tsid = Some(tsid);
                            self.version = (self.version + 1) & 31;
                            self.since = 64;
                        }
                    }
                    if (0x1fc8..=0x1fcf).contains(&pid) && section[0] == 2 && section.len() >= 16 {
                        let program = u16::from_be_bytes([section[3], section[4]]);
                        if program != 0 && self.programs.insert(pid, program) != Some(program) {
                            self.version = (self.version + 1) & 31;
                            self.since = 64;
                        }
                    }
                }
            }
            if !self.broadcast
                && self.tsid.is_some()
                && !self.programs.is_empty()
                && self.since >= 64
            {
                out.extend_from_slice(&self.packet());
                self.since = 0;
            }
            out.extend_from_slice(p);
            self.since = self.since.saturating_add(1);
        }
        Ok(out)
    }
    fn packet(&mut self) -> [u8; 188] {
        let size = 9 + 4 * self.programs.len();
        let mut section = vec![0, 0xb0 | ((size >> 8) as u8), size as u8];
        section.extend_from_slice(&self.tsid.unwrap().to_be_bytes());
        section.extend_from_slice(&[0xc1 | (self.version << 1), 0, 0]);
        for (&pid, &program) in &self.programs {
            section.extend_from_slice(&program.to_be_bytes());
            section.extend_from_slice(&(0xe000 | pid).to_be_bytes());
        }
        section.extend_from_slice(&crc(&section).to_be_bytes());
        let mut p = [0xff; 188];
        p[..5].copy_from_slice(&[0x47, 0x40, 0, 0x10 | self.cc, 0]);
        p[5..5 + section.len()].copy_from_slice(&section);
        self.cc = (self.cc + 1) & 15;
        p
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn section(pid: u16, table: u8, id: u16) -> [u8; 188] {
        let mut s = vec![
            table,
            0xb0,
            13,
            (id >> 8) as u8,
            id as u8,
            0xc1,
            0,
            0,
            0,
            0,
            0,
            0,
        ];
        s.extend_from_slice(&crc(&s).to_be_bytes());
        let mut p = [0xff; 188];
        p[..5].copy_from_slice(&[0x47, 0x40 | (pid >> 8) as u8, pid as u8, 0x10, 0]);
        p[5..21].copy_from_slice(&s);
        p
    }
    #[test]
    fn creates_pat_with_crc_and_program() {
        let mut pat = Pat::default();
        assert_eq!(pat.append(&section(0x1fc8, 2, 123)).unwrap().len(), 188);
        let out = pat.append(&section(0x11, 0x42, 456)).unwrap();
        assert_eq!(out.len(), 376);
        assert_eq!(&out[..5], &[0x47, 0x40, 0, 0x10, 0]);
        assert_eq!(crc(&out[5..21]), 0);
        assert_eq!(&out[8..10], &456u16.to_be_bytes());
        assert_eq!(&out[13..17], &[0, 123, 0xff, 0xc8]);
    }
    #[test]
    fn rejects_corrupt_crc_and_pointer() {
        let mut pat = Pat::default();
        let mut p = section(0x1fc8, 2, 123);
        p[12] ^= 1;
        pat.append(&p).unwrap();
        assert!(pat.programs.is_empty());
        p = section(0x11, 0x42, 456);
        p[4] = 255;
        pat.append(&p).unwrap();
        assert_eq!(pat.tsid, None);
    }
    #[test]
    fn retains_broadcast_pat() {
        let mut pat = Pat::default();
        pat.append(&section(0, 0, 456)).unwrap();
        pat.append(&section(0x1fc8, 2, 123)).unwrap();
        assert_eq!(pat.append(&section(0x11, 0x42, 456)).unwrap().len(), 188);
    }
    #[test]
    fn assembles_split_sections_and_resets_on_gap() {
        let full = section(0x1fc8, 2, 123);
        let mut first = [0xff; 188];
        first[..7].copy_from_slice(&[0x47, 0x5f, 0xc8, 0x30, 175, 0, 0]);
        first[180] = 0;
        first[181..].copy_from_slice(&full[5..12]);
        let mut second = [0xff; 188];
        second[..4].copy_from_slice(&[0x47, 0x1f, 0xc8, 0x11]);
        second[4..13].copy_from_slice(&full[12..21]);
        let mut pat = Pat::default();
        pat.append(&first).unwrap();
        pat.append(&second).unwrap();
        assert_eq!(pat.programs.get(&0x1fc8), Some(&123));
        let mut pat = Pat::default();
        pat.append(&first).unwrap();
        second[3] = 0x12;
        pat.append(&second).unwrap();
        assert!(pat.programs.is_empty());
    }
}
