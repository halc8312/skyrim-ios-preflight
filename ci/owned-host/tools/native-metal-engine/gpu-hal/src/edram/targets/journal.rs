//! Exact GPU write journal. This is evidence of written bytes, not permission
//! to overwrite real guest memory: the host must still enforce its fence and
//! publication contract. No untouched CPU bytes are returned for writeback.
use super::*;
use std::time::{Duration, Instant};

#[derive(Debug, PartialEq, Eq)]
pub struct GuestByteWrite {
    pub address: u32,
    pub bytes: Vec<u8>,
}
pub struct GuestWriteJournal {
    pub(super) owner: Rc<Owner<Platform>>,
    pub(super) submission: Rc<Cell<resolve::Submission>>,
    pub(super) readback: Buffer<Platform>,
    pub(super) range: Range<u32>,
    pub(super) aligned_start: u32,
    pub(super) snapshot_bytes: u64,
    pub(super) mask_bytes: u64,
}
impl GuestWriteJournal {
    pub fn extent(&self) -> Range<u32> {
        self.range.clone()
    }
    pub fn serial(&self) -> Option<Serial> {
        match self.submission.get() {
            resolve::Submission::Submitted(s) => Some(s),
            _ => None,
        }
    }
    /// Returns immutable, ordered contiguous runs after this exact store's
    /// verified success. Later stores cannot change the journal. Does not apply
    /// writes, advance guest fences, or release mirror ownership by itself.
    pub fn read(&self, gpu: &mut Gpu<Platform>, timeout: Duration) -> Result<Vec<GuestByteWrite>> {
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("guest journal device"));
        }
        let serial = self
            .serial()
            .ok_or(Error::Rejected("guest journal frame unsubmitted"))?;
        let start = Instant::now();
        gpu.wait(serial, timeout)?;
        let remaining = timeout
            .checked_sub(start.elapsed())
            .ok_or(Error::Timeout(serial))?;
        let data = gpu.read_buffer(
            &self.readback,
            0..self.snapshot_bytes + self.mask_bytes,
            remaining,
        )?;
        self.owner.healthy()?;
        decode(
            &data,
            self.range.clone(),
            self.aligned_start,
            self.snapshot_bytes as usize,
        )
    }
}
fn decode(
    data: &[u8],
    range: Range<u32>,
    aligned_start: u32,
    snapshot_bytes: usize,
) -> Result<Vec<GuestByteWrite>> {
    let count = (range.end - range.start) as usize;
    let masks = &data[snapshot_bytes..];
    if (count..masks.len() * 8).any(|bit| masks[bit / 8] & (1 << (bit % 8)) != 0) {
        return Err(Error::Rejected("guest journal mask outside extent"));
    }
    let mut runs: Vec<GuestByteWrite> = Vec::new();
    for (index, word) in masks.chunks_exact(4).enumerate() {
        let mut bits = u32::from_le_bytes(word.try_into().unwrap());
        while bits != 0 {
            let first = bits.trailing_zeros();
            let length = (bits >> first).trailing_ones();
            let begin = index * 32 + first as usize;
            let address = range.start + begin as u32;
            let offset = (range.start - aligned_start) as usize + begin;
            let payload = &data[offset..offset + length as usize];
            if let Some(last) = runs
                .last_mut()
                .filter(|r| r.address + r.bytes.len() as u32 == address)
            {
                last.bytes.extend_from_slice(payload);
            } else {
                runs.push(GuestByteWrite {
                    address,
                    bytes: payload.to_vec(),
                });
            }
            let end = first + length;
            bits = if end == 32 { 0 } else { bits & (!0u32 << end) };
        }
    }
    Ok(runs)
}
