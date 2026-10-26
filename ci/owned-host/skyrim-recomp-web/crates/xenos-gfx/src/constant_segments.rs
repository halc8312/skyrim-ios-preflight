//! GPU-independent, bounded patch segments. This owns stage carry and offset
//! generations; a backend may place sealed segments between ordered draws in
//! one command buffer. Resetting CPU records is NOT a guest fence or submit.
use crate::constants::{ConstantError, ConstantPlan};
use std::sync::atomic::{AtomicU64, Ordering};
const STAGES: usize = 5;
const INDICES: usize = 256;
const RECORD_WORDS: usize = 12;
const BIN_HEADER_WORDS: usize = 16;
pub const MAX_RECORDS: usize = 16384;
pub const MAX_VALUES: usize = 1 << 17;
static NEXT_SEGMENT: AtomicU64 = AtomicU64::new(1);
fn next_id() -> Result<u64, ConstantError> {
    NEXT_SEGMENT
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |x| x.checked_add(1))
        .map_err(|_| ConstantError("segment identity exhausted"))
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ConstantBlock {
    segment: u64,
    offset: u32,
    bytes: u32,
    window: u32,
}
impl ConstantBlock {
    pub fn offset(self) -> u32 {
        self.offset
    }
    pub fn bytes(self) -> u32 {
        self.bytes
    }
    pub fn window(self) -> u32 {
        self.window
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PushError {
    Invalid(&'static str),
    Full(CapacityFailure),
}
/// Exact bytes needed to snapshot the entire prospective dispatch. This does
/// not include the GPU output buffer: its capacity is checked separately.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SnapshotDemand {
    pub records: usize,
    pub values: usize,
}
impl SnapshotDemand {
    pub fn upload_bytes(self) -> u64 {
        if self.records == 0 { 0 } else {
            ((BIN_HEADER_WORDS + self.records * 13) * 4 + self.values * 16) as u64
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdmissionError<E> {
    Draw(PushError),
    Reservation(E),
}
impl<E> From<PushError> for AdmissionError<E> {
    fn from(value: PushError) -> Self { Self::Draw(value) }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CapacityFailure(u8);
impl CapacityFailure {
    /// Output capacity needs a rewind; parameter capacity can first seal and
    /// continue at the same offsets. Several exhausted budgets may be reported.
    pub fn rewind_required(self) -> bool {
        self.0 & 1 != 0
    }
    pub fn records_full(self) -> bool {
        self.0 & 2 != 0
    }
    pub fn values_full(self) -> bool {
        self.0 & 4 != 0
    }
}
/// Full CPU stage state is small (20KiB) and belongs to this segment. Each
/// lane's first emitted value is explicit in every dispatch, including a longer
/// block following a shorter one; no prior GPU register carry is assumed.
pub struct StageConstants<'a> {
    pub stage: usize,
    pub data: &'a [u8],
    pub window: u32,
}
#[derive(Clone, Copy)]
struct Planned {
    block: ConstantBlock,
    mask: [u32; 8],
    changed: usize,
    reused: bool,
    next: u64,
}
pub struct PatchSegment {
    id: u64,
    capacity: u64,
    alignment: u64,
    cursor: u64,
    sealed: bool,
    state: Box<[[[u8; 16]; INDICES]; STAGES]>,
    last: [Option<ConstantBlock>; STAGES],
    seen: [[u32; 8]; STAGES],
    dispatch: u32,
    records: Vec<u32>,
    values: Vec<u8>,
    binned: Vec<u32>,
}
pub struct SealedSegment<'a> {
    inner: &'a PatchSegment,
}
impl PatchSegment {
    pub fn new(capacity: u64, alignment: u64) -> Result<Self, ConstantError> {
        if capacity == 0
            || capacity > 16 << 20
            || !capacity.is_multiple_of(16)
            || alignment < 16
            || !alignment.is_power_of_two()
            || alignment > capacity
        {
            return Err(ConstantError("segment capacity/alignment"));
        }
        Ok(Self {
            id: next_id()?,
            capacity,
            alignment,
            cursor: 0,
            sealed: false,
            state: Box::new([[[0; 16]; INDICES]; STAGES]),
            last: [None; STAGES],
            seen: [[0; 8]; STAGES],
            dispatch: 0,
            records: vec![0],
            values: Vec::new(),
            binned: Vec::new(),
        })
    }
    pub fn capacity(&self) -> u64 {
        self.capacity
    }
    pub fn used_bytes(&self) -> u64 {
        self.cursor
    }
    pub fn record_count(&self) -> usize {
        self.records[0] as usize
    }
    pub fn changed_bytes(&self) -> usize {
        self.values.len()
    }
    /// Failure leaves all offsets, bytes, masks and carry unchanged. A Full
    /// result asks the host to seal and encode prior draws, then continue for
    /// parameter exhaustion or reset for output exhaustion. It NEVER submits
    /// or drops a guest synchronization boundary itself.
    fn validate_shape(stage: usize, data: &[u8], window: u32) -> Result<(), PushError> {
        if stage >= STAGES
            || data.len() > INDICES * 16
            || !data.len().is_multiple_of(16)
            || window == 0
            || (window as usize) < data.len()
            || !window.is_multiple_of(16)
            || window > 4096
        {
            return Err(PushError::Invalid("constant stage/data/window"));
        }
        Ok(())
    }
    fn plan(
        &self,
        stage: usize,
        data: &[u8],
        window: u32,
        cursor: u64,
        records: usize,
        values: usize,
    ) -> Result<Planned, PushError> {
        if self.sealed {
            return Err(PushError::Invalid("segment already sealed"));
        }
        Self::validate_shape(stage, data, window)?;
        let mut mask = [0u32; 8];
        let mut changed = 0usize;
        let mut equal = true;
        for (lane, bytes) in data.chunks_exact(16).enumerate() {
            let differs = self.state[stage][lane] != bytes;
            equal &= !differs;
            if self.seen[stage][lane / 32] & (1 << (lane % 32)) == 0 || differs {
                mask[lane / 32] |= 1 << (lane % 32);
                changed += 1;
            }
        }
        if let Some(previous) = self.last[stage] {
            if equal && previous.bytes == data.len() as u32 && previous.window == window {
                return Ok(Planned { block: previous, mask: [0; 8], changed: 0, reused: true, next: cursor });
            }
        }
        let offset = cursor.next_multiple_of(self.alignment);
        let failed = u8::from(offset + u64::from(window) > self.capacity)
            | (u8::from(records == MAX_RECORDS) << 1)
            | (u8::from(values + changed > MAX_VALUES) << 2);
        if failed & 1 != 0 && self.cursor == 0 {
            return Err(PushError::Invalid("draw exceeds empty segment capacity"));
        }
        if failed != 0 {
            return Err(PushError::Full(CapacityFailure(failed)));
        }
        Ok(Planned {
            block: ConstantBlock { segment: self.id, offset: offset as u32, bytes: data.len() as u32, window },
            mask,
            changed,
            reused: false,
            next: offset + data.len().max(16) as u64,
        })
    }
    fn apply(&mut self, stage: usize, data: &[u8], p: Planned) -> ConstantBlock {
        if p.reused {
            return p.block;
        }
        let first_value = self.values.len() / 16;
        for (lane, bytes) in data.chunks_exact(16).enumerate() {
            if p.mask[lane / 32] & (1 << (lane % 32)) != 0 {
                self.values.extend_from_slice(bytes);
                self.state[stage][lane].copy_from_slice(bytes);
            }
        }
        self.records.extend([stage as u32, p.block.offset / 16, (data.len() / 16) as u32, first_value as u32]);
        self.records.extend(p.mask);
        self.records[0] += 1;
        self.cursor = p.next;
        self.last[stage] = Some(p.block);
        for lane in 0..data.len() / 16 {
            self.seen[stage][lane / 32] |= 1 << (lane % 32);
        }
        p.block
    }
    pub fn push(&mut self, stage: usize, data: &[u8], window: u32) -> Result<ConstantBlock, PushError> {
        let p = self.plan(stage, data, window, self.cursor, self.record_count(), self.values.len() / 16)?;
        Ok(self.apply(stage, data, p))
    }
    /// All requested shader stages of one draw are admitted atomically, with no
    /// per-draw heap allocation. Duplicate stages are rejected before mutation.
    pub fn push_draw(&mut self, stages: &[StageConstants<'_>]) -> Result<[Option<ConstantBlock>; STAGES], PushError> {
        match self.push_draw_with_admission(stages, |_| Ok::<_, std::convert::Infallible>(())) {
            Ok(blocks) => Ok(blocks),
            Err(AdmissionError::Draw(error)) => Err(error),
            Err(AdmissionError::Reservation(never)) => match never {},
        }
    }
    /// Reserve backend snapshot capacity AFTER complete CPU validation, but
    /// BEFORE changing carry, offsets, records or values. Rejection is atomic;
    /// a recorder can still seal every previously admitted draw unchanged.
    pub fn push_draw_with_admission<E>(
        &mut self, stages: &[StageConstants<'_>],
        admit: impl FnOnce(SnapshotDemand) -> Result<(), E>,
    ) -> Result<[Option<ConstantBlock>; STAGES], AdmissionError<E>> {
        if self.sealed {
            return Err(PushError::Invalid("segment already sealed").into());
        }
        if stages.len() > STAGES {
            return Err(PushError::Invalid("draw stage count").into());
        }
        let mut requested = 0u32;
        for s in stages {
            Self::validate_shape(s.stage, s.data, s.window)?;
            if requested & (1 << s.stage) != 0 {
                return Err(PushError::Invalid("duplicate draw stage").into());
            }
            requested |= 1 << s.stage;
        }
        let mut plans = [None; STAGES];
        let (mut cursor, mut records, mut values) = (self.cursor, self.record_count(), self.values.len() / 16);
        for s in stages {
            if s.stage >= STAGES || plans[s.stage].is_some() {
                return Err(PushError::Invalid("duplicate/outside draw stage").into());
            }
            let p = self.plan(s.stage, s.data, s.window, cursor, records, values)?;
            if !p.reused {
                cursor = p.next;
                records += 1;
                values += p.changed;
            }
            plans[s.stage] = Some(p);
        }
        admit(SnapshotDemand { records, values }).map_err(AdmissionError::Reservation)?;
        let mut blocks = [None; STAGES];
        for s in stages {
            blocks[s.stage] = Some(self.apply(s.stage, s.data, plans[s.stage].unwrap()));
        }
        Ok(blocks)
    }
    /// Immutable borrow prevents changing the snapshots while a backend uses
    /// them. Additional pushes stay rejected even after this view is dropped.
    pub fn seal(&mut self) -> Result<SealedSegment<'_>, ConstantError> {
        if !self.sealed {
            self.binned = bin_records(&self.records)?;
            ConstantPlan::validate(&self.binned, &self.values, self.capacity as usize)?;
            self.sealed = true;
        }
        Ok(SealedSegment { inner: self })
    }
    /// After the backend encoded every consumer, keep append-only output windows
    /// and reuse tokens but begin a fresh dispatch. Each first-used lane is
    /// reseeded from CPU: the kernel does not carry registers between dispatches.
    pub fn continue_after_seal(&mut self) -> Result<(), ConstantError> {
        if !self.sealed {
            return Err(ConstantError("unsealed constants cannot continue"));
        }
        let dispatch = self.dispatch.checked_add(1).ok_or(ConstantError("constant dispatch sequence exhausted"))?;
        self.dispatch = dispatch;
        self.records.truncate(1);
        self.records[0] = 0;
        self.values.clear();
        self.binned.clear();
        self.seen = [[0; 8]; STAGES];
        self.sealed = false;
        Ok(())
    }
    /// Call only after previous consumers were encoded, or to explicitly discard
    /// an unsubmitted CPU segment. Old ConstantBlock tokens become invalid.
    pub fn reset(&mut self) -> Result<(), ConstantError> {
        let id = next_id()?;
        self.id = id;
        self.dispatch = 0;
        self.cursor = 0;
        self.sealed = false;
        self.last = [None; STAGES];
        self.seen = [[0; 8]; STAGES];
        // last=None prevents reuse and seen=0 explicitly seeds every first-used
        // lane, even when its bytes equal this retained CPU carry. Clearing the
        // entire20KiB state here is therefore redundant on every output rewind.
        self.records.truncate(1);
        self.records[0] = 0;
        self.values.clear();
        self.binned.clear();
        Ok(())
    }
}
impl SealedSegment<'_> {
    pub fn dispatch(&self) -> u32 {
        self.inner.dispatch
    }
    pub fn alignment(&self) -> u64 {
        self.inner.alignment
    }
    pub fn id(&self) -> u64 {
        self.inner.id
    }
    pub fn capacity(&self) -> u64 {
        self.inner.capacity
    }
    pub fn records(&self) -> &[u32] {
        &self.inner.binned
    }
    pub fn values(&self) -> &[u8] {
        &self.inner.values
    }
    pub fn block(&self, block: ConstantBlock) -> Result<(u32, u32), ConstantError> {
        if block.segment != self.inner.id {
            return Err(ConstantError("stale or foreign constant block"));
        }
        Ok((block.offset, block.window))
    }
    pub fn materialize(&self, destination: &mut [u8]) -> Result<(), ConstantError> {
        ConstantPlan::validate(self.records(), self.values(), self.capacity() as usize)?.execute(destination)?;
        Ok(())
    }
}

pub fn bin_records(records: &[u32]) -> Result<Vec<u32>, super::constants::ConstantError> {
    use super::constants::ConstantError;
    if records.is_empty() {
        return Err(ConstantError("missing patch count"));
    }
    let count = records[0] as usize;
    if count > MAX_RECORDS || records.len() != 1 + count * RECORD_WORDS {
        return Err(ConstantError("invalid constant records"));
    }
    let index_base = BIN_HEADER_WORDS + count * RECORD_WORDS;
    let mut result = vec![0u32; index_base + count];
    result[0] = count as u32;
    result[BIN_HEADER_WORDS..index_base].copy_from_slice(&records[1..]);
    for record in records[1..].chunks_exact(RECORD_WORDS) {
        let stage = record[0] as usize;
        if stage >= STAGES || record[2] as usize > INDICES {
            return Err(ConstantError("invalid constant stage or count"));
        }
        let header = 1 + stage * 3;
        result[header + 1] += 1;
        result[header + 2] = result[header + 2].max(record[2]);
    }
    let mut next = [0usize; STAGES];
    let mut start = index_base;
    for (stage, next) in next.iter_mut().enumerate() {
        let header = 1 + stage * 3;
        result[header] = start as u32;
        *next = start;
        start += result[header + 1] as usize;
    }
    for (index, record) in records[1..].chunks_exact(RECORD_WORDS).enumerate() {
        let next = &mut next[record[0] as usize];
        result[*next] = index as u32;
        *next += 1;
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn segment_failure_reuse_and_epoch_do_not_leak_state() {
        let mut b = PatchSegment::new(512, 256).unwrap();
        let full = [0x73; 256];
        let first = b.push(3, &full, 256).unwrap();
        assert_eq!(b.push(3, &full, 256), Ok(first));
        assert_eq!(b.record_count(), 1);
        let small = b.push(3, &[0x18; 16], 256).unwrap();
        let snapshot = (b.used_bytes(), b.record_count(), b.changed_bytes());
        assert_eq!(b.push(3, &full, 256), Err(PushError::Full(CapacityFailure(1))));
        assert_eq!(snapshot, (b.used_bytes(), b.record_count(), b.changed_bytes()));
        assert!(b.push(5, &full, 256).is_err());
        let mut out = vec![0xa5; 512];
        {
            let s = b.seal().unwrap();
            s.materialize(&mut out).unwrap();
            assert_eq!(s.block(first), Ok((0, 256)));
            assert_eq!(s.block(small), Ok((256, 256)));
        }
        assert_eq!(&out[..256], &full);
        assert_eq!(&out[256..272], &[0x18; 16]);
        assert!(out[272..].iter().all(|&n| n == 0xa5));
        assert!(b.push(0, &[0; 16], 16).is_err());
        b.reset().unwrap();
        let new = b.push(3, &full, 256).unwrap();
        assert_ne!(new, first);
        assert_eq!(b.changed_bytes(), 256, "same bytes still full after rollover");
        let s = b.seal().unwrap();
        assert!(s.block(first).is_err());
        assert!(s.block(new).is_ok());
        let mut other = PatchSegment::new(512, 256).unwrap();
        let foreign = other.push(3, &full, 256).unwrap();
        assert!(s.block(foreign).is_err());
    }
    #[test]
    fn generated_full_state_sequences_reconstruct_bits_across_short_blocks_and_rollovers() {
        let mut b = PatchSegment::new(16 << 10, 256).unwrap();
        let mut expected = Vec::new();
        let mut rng = 0x12345678u32;
        let mut rollovers = 0;
        fn verify(b: &mut PatchSegment, expected: &mut Vec<(ConstantBlock, Vec<u8>)>) {
            let s = b.seal().unwrap();
            let mut bytes = vec![0xa5; s.capacity() as usize];
            s.materialize(&mut bytes).unwrap();
            let mut written = vec![false; bytes.len()];
            for (block, data) in expected.iter() {
                let (offset, _) = s.block(*block).unwrap();
                let at = offset as usize;
                assert_eq!(&bytes[at..at + data.len()], data.as_slice());
                written[at..at + data.len()].fill(true);
            }
            for (i, &v) in bytes.iter().enumerate() {
                if !written[i] {
                    assert_eq!(v, 0xa5);
                }
            }
            expected.clear();
        }
        for n in 0..600 {
            rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
            let stage = (rng % 5) as usize;
            let lanes = [0, 1, 2, 13, 64, 256][(rng as usize >> 8) % 6];
            let data: Vec<u8> =
                (0..lanes * 4).flat_map(|lane| [rng, 0x7fc00001, 0x80000000, n][lane % 4].to_le_bytes()).collect();
            let window = (data.len() as u32).max(16);
            let block = match b.push(stage, &data, window) {
                Ok(v) => v,
                Err(PushError::Full(_)) => {
                    verify(&mut b, &mut expected);
                    b.reset().unwrap();
                    rollovers += 1;
                    b.push(stage, &data, window).unwrap()
                }
                e => panic!("{e:?}"),
            };
            expected.push((block, data));
        }
        verify(&mut b, &mut expected);
        assert!(rollovers > 10);
    }
}

#[cfg(test)]
mod seal_tests {
    use super::*;
    #[test]
    fn draw_admission_is_atomic_and_seal_keeps_offsets_while_reseeding_dispatch_carry() {
        let mut b = PatchSegment::new(1024, 256).unwrap();
        let a = [0x37; 256];
        let mut changed = a;
        changed[240..].fill(0x8a);
        let first = b
            .push_draw(&[
                StageConstants { stage: 0, data: &a, window: 256 },
                StageConstants { stage: 1, data: &a, window: 256 },
            ])
            .unwrap();
        let before = (b.records.clone(), b.values.clone(), b.cursor, b.last, b.state.clone());
        assert_eq!(
            b.push_draw(&[
                StageConstants { stage: 2, data: &a, window: 256 },
                StageConstants { stage: 3, data: &a, window: 256 },
                StageConstants { stage: 4, data: &a, window: 256 }
            ]),
            Err(PushError::Full(CapacityFailure(1)))
        );
        assert_eq!(before, (b.records.clone(), b.values.clone(), b.cursor, b.last, b.state.clone()));
        let mut out = vec![0xa5; 1024];
        {
            let s = b.seal().unwrap();
            s.materialize(&mut out).unwrap();
        }
        b.continue_after_seal().unwrap();
        assert_eq!(b.push(0, &a, 256).unwrap(), first[0].unwrap());
        assert_eq!(b.record_count(), 0);
        let next = b.push(1, &changed, 256).unwrap();
        assert_eq!(next.offset(), 512);
        assert_eq!(b.changed_bytes(), 256, "new dispatch must seed unchanged lanes too");
        {
            let s = b.seal().unwrap();
            assert!(s.block(first[0].unwrap()).is_ok());
            s.materialize(&mut out).unwrap();
        }
        assert_eq!(&out[..256], &a);
        assert_eq!(&out[256..512], &a);
        assert_eq!(&out[512..768], &changed);
        assert!(out[768..].iter().all(|&n| n == 0xa5));
    }
}

#[cfg(test)]
mod parameter_budget_tests {
    use super::*;
    #[test]
    fn records_and_values_can_seal_without_rewinding_live_windows() {
        for values in [false, true] {
            let mut b = PatchSegment::new(16 << 20, 16).unwrap();
            let bytes = if values { 4096 } else { 16 };
            let count = if values { MAX_VALUES / 256 } else { MAX_RECORDS };
            for n in 0..count {
                b.push(n % 5, &vec![(n / 5) as u8; bytes], bytes as u32).unwrap();
            }
            let result = b.push(0, &vec![0xf3; bytes], bytes as u32).unwrap_err();
            let PushError::Full(reason) = result else { panic!("{result:?}") };
            assert!(!reason.rewind_required());
            assert_eq!(reason.values_full(), values);
            assert_eq!(reason.records_full(), !values);
            let cursor = b.used_bytes();
            b.seal().unwrap();
            b.continue_after_seal().unwrap();
            let next = b.push(0, &vec![0xf3; bytes], bytes as u32).unwrap();
            assert_eq!(u64::from(next.offset()), cursor);
            assert_eq!(b.record_count(), 1);
        }
    }
}

#[cfg(test)]
mod continuation_lane_tests {
    use super::*;
    #[test]
    fn shortened_or_empty_first_block_reseeds_later_high_lanes_after_continuation() {
        for (rewind, same, short) in [false, true].into_iter().flat_map(|rewind| {
            [false, true].into_iter().flat_map(move |same| [0, 16].map(|short| (rewind, same, short)))
        }) {
            let mut p = PatchSegment::new(16384, 256).unwrap();
            let full = [0x37; 4096];
            p.push(0, &full, 4096).unwrap();
            let mut out = vec![0xa5; 16384];
            p.seal().unwrap().materialize(&mut out).unwrap();
            if rewind {
                p.reset().unwrap();
            } else {
                p.continue_after_seal().unwrap();
            }
            let value = if same { 0x37 } else { 0x18 };
            p.push(0, &vec![value; short], short.max(16) as u32).unwrap();
            let mut next = full;
            next[..short.max(16)].fill(value);
            let block = p.push(0, &next, 4096).unwrap();
            p.seal().unwrap().materialize(&mut out).unwrap();
            assert_eq!(
                &out[block.offset() as usize..block.offset() as usize + 4096],
                &next,
                "rewind={rewind} same={same} first length {short}"
            );
        }
    }
}

#[cfg(test)]
mod snapshot_admission_tests {
    use super::*;
    #[test]
    fn rejected_snapshot_leaves_all_cpu_state_and_callback_sees_exact_sealed_size() {
        let mut p = PatchSegment::new(16384, 256).unwrap();
        let first = [0x61; 64];
        p.push_draw(&[StageConstants { stage: 0, data: &first, window: 4096 }]).unwrap();
        let previous = (p.cursor, p.records.clone(), p.values.clone(), p.seen, p.last, p.state.clone());
        let data = [0x39; 32];
        let rejected = p.push_draw_with_admission(&[StageConstants { stage: 0, data: &data, window: 4096 }], |d| {
            assert_eq!(d.upload_bytes(), (16 + 2 * 13) * 4 + 96);
            Err("no backing")
        });
        assert!(matches!(rejected, Err(AdmissionError::Reservation("no backing"))));
        assert_eq!(previous, (p.cursor, p.records.clone(), p.values.clone(), p.seen, p.last, p.state.clone()));
        let mut demand = 0;
        p.push_draw_with_admission(&[StageConstants { stage: 0, data: &data, window: 4096 }], |d| { demand = d.upload_bytes(); Ok::<_, ()>(()) }).unwrap();
        let sealed = p.seal().unwrap();
        assert_eq!(demand as usize, sealed.records().len() * 4 + sealed.values().len());
        p.continue_after_seal().unwrap();
        p.push_draw_with_admission(&[StageConstants { stage: 0, data: &data, window: 4096 }], |d| { assert_eq!(d.upload_bytes(), 0); Ok::<_, ()>(()) }).unwrap();
    }
    #[test]
    fn invalid_later_stage_is_rejected_before_backend_reservation() {
        let mut p = PatchSegment::new(16384, 256).unwrap();
        let result = p.push_draw_with_admission(&[
            StageConstants { stage: 0, data: &[3; 16], window: 4096 },
            StageConstants { stage: 1, data: &[4; 17], window: 4096 },
        ], |_| -> Result<(), ()> { panic!("invalid draw must not reserve"); });
        assert!(matches!(result, Err(AdmissionError::Draw(PushError::Invalid(_)))));
        assert_eq!((p.used_bytes(), p.record_count(), p.changed_bytes()), (0, 0, 0));
    }
}
