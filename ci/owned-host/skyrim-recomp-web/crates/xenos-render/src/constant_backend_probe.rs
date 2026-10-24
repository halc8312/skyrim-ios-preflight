//! Authored, offscreen constant-replay comparison. No guest input or renderer attachment.
//! Both GPU backends receive identical binned records, integer values and output extent.
//! Pipeline creation and warm-up are outside samples. Readback is explicit and timed separately.
//! These host elapsed timings do not measure game FPS or GPU execution time.
use super::*;
use std::result::Result;
use std::time::Instant;

#[derive(Clone, Debug)]
pub struct Batch {
    pub words: Vec<u32>,
    pub record_bytes: Vec<u8>,
    pub values: Vec<u8>,
    pub expected: Vec<u8>,
    pub records: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Times {
    pub upload_ns: u64,
    pub encode_ns: u64,
    pub finish_ns: u64,
    pub submit_ns: u64,
    pub completion_ns: u64,
    pub readback_ns: u64,
    pub total_ns: u64,
}
const _: () = assert!(std::mem::size_of::<Times>() == 56);

pub trait Backend {
    /// Create/reuse resources outside samples; must finish before returning.
    fn prepare(&mut self, batch: &Batch) -> Result<(), String>;
    /// Reset output to 0xa5 AND poison readback to 0x5a before EVERY run, outside timing.
    fn reset(&mut self, batch: &Batch) -> Result<(), String>;
    /// Must complete and return every byte, including untouched alignment gaps.
    fn run(&mut self, batch: &Batch) -> Result<(Vec<u8>, Times), String>;
}

pub fn limits() -> [u64; 7] { [STAGES as u64, INDICES as u64, RECORD_WORDS as u64,
    BIN_HEADER_WORDS as u64, MAX_RECORDS as u64, MAX_VALUES as u64, BLOCK_BUFFER_BYTES] }

pub fn validate(batch: &Batch) -> Result<(), String> {
    let r = &batch.words;
    if r.len() < BIN_HEADER_WORDS || r.len() > BIN_HEADER_WORDS + MAX_RECORDS * (RECORD_WORDS + 1)
        || batch.expected.is_empty() || batch.expected.len() > BLOCK_BUFFER_BYTES as usize
        || batch.expected.len() % 16 != 0 || batch.values.len() > MAX_VALUES * 16 || batch.values.len() % 16 != 0
        || batch.record_bytes.len() != r.len() * 4
        || !r.iter().flat_map(|n| n.to_le_bytes()).eq(batch.record_bytes.iter().copied()) { return Err("fixture extent".into()); }
    let count = r[0] as usize;
    if count > MAX_RECORDS || count != batch.records { return Err("record count".into()); }
    let base = BIN_HEADER_WORDS + count * RECORD_WORDS;
    if r.len() != base + count { return Err("record count".into()); }
    let mut source = 0; let mut spans = Vec::new();
    for index in 0..count {
        let record = &r[BIN_HEADER_WORDS + index * RECORD_WORDS..][..RECORD_WORDS];
        let (offset, len) = (record[1] as u64 * 16, record[2] as u64 * 16);
        if record[0] >= STAGES || record[2] > INDICES as u32 || record[3] as usize != source
            || offset + len > batch.expected.len() as u64 { return Err("record range".into()); }
        if len != 0 { spans.push((offset, offset + len)); }
        for (w, &mask) in record[4..].iter().enumerate() {
            let bits = (record[2] as usize).saturating_sub(w * 32).min(32);
            let valid = if bits == 32 { u32::MAX } else { (1u32 << bits) - 1 };
            if mask & !valid != 0 { return Err("mask outside block".into()); }
            source += mask.count_ones() as usize;
        }
    }
    if source * 16 != batch.values.len() { return Err("value extent".into()); }
    spans.sort_unstable();
    if spans.windows(2).any(|w| w[0].1 > w[1].0) { return Err("overlapping fixture writes".into()); }
    let mut next = base; let mut seen = vec![false; count];
    for stage in 0..STAGES as usize {
        let h = &r[1 + stage * 3..][..3]; let n = h[1] as usize;
        if h[0] as usize != next || n > count || next + n > r.len() { return Err("bin extent".into()); }
        let mut maximum = 0; let mut previous = None;
        for &index in &r[next..next+n] {
            let index = index as usize;
            if index >= count || seen[index] || previous.is_some_and(|p| index <= p)
                || r[BIN_HEADER_WORDS + index * RECORD_WORDS] != stage as u32 { return Err("bin index".into()); }
            seen[index] = true; previous = Some(index);
            maximum = maximum.max(r[BIN_HEADER_WORDS + index * RECORD_WORDS + 2]);
        }
        if maximum != h[2] { return Err("bin maximum".into()); } next += n;
    }
    if next != r.len() || !seen.iter().all(|n| *n) { return Err("bin coverage".into()); }
    Ok(())
}

fn ns(at: Instant) -> u64 { at.elapsed().as_nanos().try_into().unwrap_or(u64::MAX) }

/// Independent record-major scalar oracle, not the GPU's stage/lane traversal.
fn expected(records: &[u32], values: &[u8], size: usize) -> Vec<u8> {
    let mut out = vec![0xa5; size];
    let mut state = [[[0u8; 16]; INDICES]; STAGES as usize];
    for r in records[1..].chunks_exact(RECORD_WORDS) {
        let stage = &mut state[r[0] as usize];
        let mut source = r[3] as usize * 16;
        for (index, current) in stage.iter_mut().enumerate() {
            if r[4 + index / 32] & (1 << (index % 32)) != 0 {
                current.copy_from_slice(&values[source..source + 16]); source += 16;
            }
        }
        for (index, current) in stage.iter().take(r[2] as usize).enumerate() {
            let offset = r[1] as usize * 16 + index * 16;
            out[offset..offset + 16].copy_from_slice(current);
        }
    }
    out
}

/// Numeric batch sizes observed locally; all content is authored, never copied from a game.
/// Interleaved stages, shrinking/growing blocks, no-change records, high mask bits and NaN bits.
pub fn batches() -> Vec<Batch> {
    [6usize, 27, 37, 68, 136, 1024].into_iter().map(|count| {
        let mut records = vec![count as u32];
        let mut values = Vec::new();
        let mut cursor = 0usize;
        for index in 0..count {
            let stage = index % STAGES as usize;
            let len = [256usize, 31, 33, 3, 128, 0, 256][index % 7];
            let offset = cursor.next_multiple_of(256);
            let mut record = [0u32; RECORD_WORDS];
            record[..4].copy_from_slice(&[stage as u32, (offset / 16) as u32, len as u32, (values.len() / 16) as u32]);
            for lane in 0..len {
                if index < STAGES as usize || index % 9 != 0 && (lane + index) % 19 == 0 {
                    record[4 + lane / 32] |= 1 << (lane % 32);
                    for word in [0x7fc0_0001u32 ^ index as u32, lane as u32, 0x8000_0000, u32::MAX] {
                        values.extend_from_slice(&word.to_le_bytes());
                    }
                }
            }
            records.extend(record);
            cursor = offset + (len * 16).max(16);
        }
        let output = expected(&records, &values, cursor.next_multiple_of(256) + 64);
        assert!(output.len() <= BLOCK_BUFFER_BYTES as usize);
        let words = bin_records(&records);
        let record_bytes = words.iter().flat_map(|n| n.to_le_bytes()).collect();
        Batch { words, record_bytes, values, expected: output, records: count }
    }).collect()
}

/// Reuses the production shader/pipeline/layout. The consumer is an ordered GPU copy.
pub struct WebGpuBackend { device: Device, patcher: ConstantPatcher, readback: Buffer,
    initial: Option<Buffer>, poison: Option<Buffer> }
impl WebGpuBackend {
    pub fn new(device: Device) -> Self {
        let readback = device.create_buffer(&BufferDescriptor { label:"constant backend readback", size:BLOCK_BUFFER_BYTES,
            usage:BufferUsage::CopyDst | BufferUsage::MapRead, mapped_at_creation:false });
        Self { patcher:ConstantPatcher::new(&device, 256), device, readback, initial:None, poison:None }
    }
}
impl Backend for WebGpuBackend {
    fn prepare(&mut self, batch: &Batch) -> Result<(), String> {
        validate(batch)?;
        // Output does not have COPY_DST in production. Initialize the sentinel using
        // an ordinary upload buffer and GPU copy, without changing production usages.
        let initial = self.device.create_buffer(&BufferDescriptor { label:"constant probe initial", size:batch.expected.len() as u64,
            usage:BufferUsage::CopySrc | BufferUsage::CopyDst, mapped_at_creation:false });
        self.device.queue().write_buffer(&initial, 0, &vec![0xa5; batch.expected.len()]);
        let poison = self.device.create_buffer(&BufferDescriptor { label:"constant probe poison", size:batch.expected.len() as u64,
            usage:BufferUsage::CopySrc | BufferUsage::CopyDst, mapped_at_creation:false });
        self.device.queue().write_buffer(&poison, 0, &vec![0x5a; batch.expected.len()]);
        // A separate probe-only destination permits a full gap check. Bind the same
        // production shader to it, keeping production buffers/flags unchanged.
        let buffer = self.device.create_buffer(&BufferDescriptor { label:"constant probe output", size:BLOCK_BUFFER_BYTES,
            usage:BufferUsage::Storage | BufferUsage::Uniform | BufferUsage::CopySrc | BufferUsage::CopyDst,
            mapped_at_creation:false });
        let entries = [BindGroupEntry { binding:0, resource:BindingResource::Buffer { buffer:&self.patcher.records_buffer, offset:0, size:None } },
            BindGroupEntry { binding:1, resource:BindingResource::Buffer { buffer:&self.patcher.values_buffer, offset:0, size:None } },
            BindGroupEntry { binding:2, resource:BindingResource::Buffer { buffer:&buffer, offset:0, size:None } }];
        self.patcher.group = self.device.create_bind_group("constant probe output", &self.patcher.pipeline.get_bind_group_layout(0), &entries);
        self.patcher.buffer = buffer;
        self.initial = Some(initial); self.poison = Some(poison);
        self.reset(batch)
    }
    fn reset(&mut self, batch: &Batch) -> Result<(), String> {
        let mut encoder = self.device.create_command_encoder("constant probe reset");
        encoder.copy_buffer_to_buffer(self.initial.as_ref().ok_or("prepare required")?, 0, &self.patcher.buffer, 0, batch.expected.len() as u64);
        encoder.copy_buffer_to_buffer(self.poison.as_ref().ok_or("prepare required")?, 0, &self.readback, 0, batch.expected.len() as u64);
        // Queue writes from prepare are ordered before this same-queue submission.
        self.device.queue().submit([encoder.finish("constant probe reset")]);
        self.device.block_on(self.device.queue().on_submitted_work_done()).map_err(|e| format!("reset {e:?}"))?;
        if !self.device.take_errors().is_empty() { return Err("GPU setup/reset error".into()); }
        Ok(())
    }
    fn run(&mut self, batch: &Batch) -> Result<(Vec<u8>, Times), String> {
        self.device.push_error_scope(ErrorFilter::Validation);
        let result = self.run_inner(batch);
        let scope = self.device.block_on(self.device.pop_error_scope()).map_err(|e| format!("scope {e:?}"))?;
        if scope.is_some() || !self.device.take_errors().is_empty() { return Err("GPU execution error".into()); }
        result
    }
}
impl WebGpuBackend {
    fn run_inner(&mut self, batch: &Batch) -> Result<(Vec<u8>, Times), String> {
        let mut out = vec![0u8; batch.expected.len()];
        let started = Instant::now(); let mut time = Times::default();
        self.device.queue().write_buffer(&self.patcher.records_buffer, 0, &batch.record_bytes);
        if !batch.values.is_empty() { self.device.queue().write_buffer(&self.patcher.values_buffer, 0, &batch.values); }
        time.upload_ns = ns(started);
        let at = Instant::now();
        let mut encoder = self.device.create_command_encoder("constant backend compare");
        { let mut pass = encoder.begin_compute_pass("constant replay");
            pass.set_pipeline(&self.patcher.pipeline); pass.set_bind_group(0, &self.patcher.group, &[]);
            pass.dispatch_workgroups(STAGES, 1, 1); pass.end(); }
        encoder.copy_buffer_to_buffer(&self.patcher.buffer, 0, &self.readback, 0, batch.expected.len() as u64);
        time.encode_ns = ns(at);
        let at = Instant::now(); let commands = encoder.finish("constant backend compare"); time.finish_ns = ns(at);
        let at = Instant::now(); self.device.queue().submit([commands]); time.submit_ns = ns(at);
        let at = Instant::now();
        if let Err(e) = self.device.block_on(self.readback.map_async(MapMode::Read, 0, Some(batch.expected.len() as u64))) {
            self.readback.unmap(); return Err(format!("readback {e:?}"));
        }
        time.completion_ns = ns(at);
        let at = Instant::now();
        let result = self.readback.read_mapped(0, &mut out); self.readback.unmap();
        result.map_err(|e| format!("mapped {e:?}"))?; time.readback_ns = ns(at); time.total_ns = ns(started);
        Ok((out, time))
    }
}

/// Two warm-ups per backend, then three ABBA/BAAB pairs. All samples are retained.
/// There is deliberately no winner/auto-selection: hardware and workload decide.
pub struct Sample { pub records: usize, pub backend:u32, pub bytes:usize, pub ordinal:u32, pub start_ns:u64, pub times:Times }
pub struct Report { pub cases:usize, pub samples:Vec<Sample> }
pub fn compare(a: &mut dyn Backend, b: &mut dyn Backend) -> Result<Report, String> {
    let mut samples = Vec::new();
    let batches = batches(); let origin = Instant::now();
    for batch in &batches {
        validate(batch)?;
        a.prepare(&batch)?; b.prepare(&batch)?;
        for _ in 0..2 { a.reset(batch)?; if a.run(&batch)?.0 != batch.expected { return Err("warm-up output mismatch A".into()); } }
        for _ in 0..2 { b.reset(batch)?; if b.run(&batch)?.0 != batch.expected { return Err("warm-up output mismatch B".into()); } }
        for (ordinal, id) in [0u32,1,1,0,1,0,0,1,0,1,1,0].into_iter().enumerate() {
            if id == 0 { a.reset(batch)?; } else { b.reset(batch)?; }
            let start_ns = ns(origin);
            let (out, time) = if id == 0 { a.run(&batch)? } else { b.run(&batch)? };
            if out != batch.expected { return Err(format!("constant output mismatch backend {id}")); }
            samples.push(Sample { records:batch.records, backend:id, bytes:batch.expected.len(), ordinal:ordinal as u32, start_ns, times:time });
        }
    }
    Ok(Report { cases:batches.len(), samples })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[derive(Default)]
    struct OracleBackend { bytes:Vec<u8>, calls:usize, resets:usize, skip:bool, corrupt_gap:bool }
    impl Backend for OracleBackend {
        fn prepare(&mut self, batch:&Batch) -> Result<(), String> { self.bytes = vec![0x5a; batch.expected.len()]; Ok(()) }
        fn reset(&mut self, _: &Batch) -> Result<(), String> { self.bytes.fill(0x5a); self.resets += 1; Ok(()) }
        fn run(&mut self, batch:&Batch) -> Result<(Vec<u8>, Times), String> {
            self.calls += 1;
            if !self.skip || self.calls == 1 { self.bytes.copy_from_slice(&batch.expected); }
            if self.corrupt_gap { *self.bytes.last_mut().unwrap() ^= 1; }
            Ok((self.bytes.clone(), Times::default()))
        }
    }
    #[test]
    fn every_warmup_and_sample_is_reset_and_stale_or_gap_outputs_are_detected() {
        let (mut a, mut b) = (OracleBackend::default(), OracleBackend::default());
        let report = compare(&mut a, &mut b).unwrap();
        assert_eq!((report.cases, report.samples.len(), a.calls, b.calls, a.resets, b.resets), (6,72,48,48,48,48));
        assert!(compare(&mut OracleBackend { skip:true, ..Default::default() }, &mut OracleBackend::default()).is_err());
        assert!(compare(&mut OracleBackend::default(), &mut OracleBackend { corrupt_gap:true, ..Default::default() }).is_err());
    }
    #[test]
    fn malformed_fixture_cannot_reach_either_gpu_with_an_ambiguous_layout() {
        let original = batches().remove(0); validate(&original).unwrap();
        let mut bad = original.clone(); bad.words[17 + RECORD_WORDS] = 0;
        bad.record_bytes = bad.words.iter().flat_map(|n| n.to_le_bytes()).collect();
        assert_eq!(validate(&bad).unwrap_err(), "overlapping fixture writes");
        let mut bad = original.clone(); bad.record_bytes[0] ^= 1;
        assert_eq!(validate(&bad).unwrap_err(), "fixture extent");
        let mut bad = original.clone(); bad.words[3] += 1;
        bad.record_bytes = bad.words.iter().flat_map(|n| n.to_le_bytes()).collect();
        assert_eq!(validate(&bad).unwrap_err(), "bin maximum");
        let mut bad = original; bad.words[0] = u32::MAX;
        bad.record_bytes = bad.words.iter().flat_map(|n| n.to_le_bytes()).collect();
        assert_eq!(validate(&bad).unwrap_err(), "record count");
    }
    #[test]
    fn authored_batches_fit_and_production_replay_matches_scalar_oracle() {
        let instance = Instance::new().unwrap();
        let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
        let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default())).unwrap();
        eprintln!("authored constant replay adapter: {:?}", adapter.info());
        let mut backend = WebGpuBackend::new(device);
        for batch in batches() {
            backend.prepare(&batch).unwrap();
            for _ in 0..2 {
                backend.reset(&batch).unwrap();
                let (actual, times) = backend.run(&batch).unwrap(); assert_eq!(actual, batch.expected);
                assert!(times.total_ns >= times.upload_ns);
            }
        }
    }
}
