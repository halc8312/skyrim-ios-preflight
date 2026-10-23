//! Authored identity/read interleavings using the real mirror and GPU readback.
use super::*;
use gpu_webgpu::{DeviceDescriptor, Instance, MapMode, RequestAdapterOptions};
use std::sync::Mutex;

const BYTES: usize = PAGE as usize * 2;

fn pattern(seed: u8) -> Vec<u8> {
    (0..BYTES).map(|i| seed.wrapping_add((i as u8).wrapping_mul(17))).collect()
}

struct State {
    bytes: Vec<u8>,
    generation: u64,
    identification_calls: u32,
    interleave: bool,
    staging_reads: u32,
}

/// Each individual call is synchronized. The mutation takes place between
/// identify and staging, then restores the original bytes after the staging
/// copy. The versioned control records both changes and never reuses a version.
struct Memory {
    versioned: bool,
    state: Mutex<State>,
}

impl Memory {
    fn new(versioned: bool, interleave: bool) -> Self {
        Self {
            versioned,
            state: Mutex::new(State {
                bytes: pattern(0x19),
                generation: 10,
                identification_calls: 0,
                interleave,
                staging_reads: 0,
            }),
        }
    }

    fn identified(state: &mut State) {
        state.identification_calls += 1;
        if state.interleave && state.identification_calls == 2 {
            state.bytes = pattern(0x73);
            state.generation += 1;
        }
    }

    fn replace(&self, seed: u8) {
        let mut state = self.state.lock().unwrap();
        state.bytes = pattern(seed);
        state.generation += 1;
    }
}

impl PhysicalMemory for Memory {
    fn read(&self, addr: u32, out: &mut [u8]) {
        let mut state = self.state.lock().unwrap();
        out.copy_from_slice(&state.bytes[addr as usize..addr as usize + out.len()]);
        if out.len() == BYTES {
            state.staging_reads += 1;
            if state.interleave {
                state.bytes = pattern(0x19);
                state.generation += 1;
                state.interleave = false;
            }
        } else if !self.versioned {
            assert_eq!(out.len(), PAGE as usize);
            Self::identified(&mut state);
        }
    }

    fn write(&self, _: u32, _: &[u8]) {
        panic!("fixture writes are explicit and synchronized")
    }

    fn version(&self, _: u32, len: u32) -> Option<u64> {
        if !self.versioned {
            return None;
        }
        assert_eq!(len, PAGE);
        let mut state = self.state.lock().unwrap();
        let generation = state.generation;
        Self::identified(&mut state);
        Some(generation)
    }
}

fn device() -> Device {
    let instance = Instance::new().expect("instance");
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).expect("adapter");
    instance.block_on(adapter.request_device(&DeviceDescriptor::default())).expect("device")
}

fn readback(device: &Device, shared: &SharedMemory) -> Vec<u8> {
    let out = device.create_buffer(&BufferDescriptor {
        label: "authored mirror readback",
        size: BYTES as u64,
        usage: BufferUsage::CopyDst | BufferUsage::MapRead,
        mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder("authored mirror readback");
    assert!(shared.gather(&mut encoder, 0, BYTES as u32, &out, 0));
    device.queue().submit([encoder.finish("authored mirror readback")]);
    device.block_on(out.map_async(MapMode::Read, 0, None)).expect("map");
    let mut bytes = vec![0; BYTES];
    out.read_mapped(0, &mut bytes).expect("readback");
    out.unmap();
    assert!(device.take_errors().is_empty());
    bytes
}

fn expect_bytes(actual: &[u8], expected: &[u8]) {
    assert_eq!(actual.len(), expected.len());
    assert_eq!(
        actual.iter().zip(expected).position(|(a, b)| a != b),
        None,
        "actual GPU mirror differs from the independently supplied byte oracle"
    );
}

fn restore_after_staging(versioned: bool) {
    let device = device();
    let memory = Memory::new(versioned, true);
    let mut shared = SharedMemory::new(&device);
    shared.sync(&device, &memory, 0, BYTES as u32).unwrap();
    assert_eq!(memory.state.lock().unwrap().staging_reads, 1);
    expect_bytes(&memory.state.lock().unwrap().bytes, &pattern(0x19));
    expect_bytes(&readback(&device, &shared), &pattern(0x73));
    shared.end_batch();
    shared.sync(&device, &memory, 0, BYTES as u32).unwrap();
    expect_bytes(&readback(&device, &shared), &pattern(0x19));
    assert_eq!(shared.uploaded, BYTES as u64 * 2);
    assert_eq!(memory.state.lock().unwrap().staging_reads, 2);
    shared.end_batch();
    shared.sync(&device, &memory, 0, BYTES as u32).unwrap();
    assert_eq!(shared.uploaded, BYTES as u64 * 2, "unchanged pages remain cached");
}

#[test]
fn hash_identity_matches_the_uploaded_snapshot_after_content_aba() {
    restore_after_staging(false);
}

#[test]
fn write_versions_detect_content_aba_without_reusing_a_generation() {
    restore_after_staging(true);
}

#[test]
fn conflict_preflight_and_gpu_written_pages_preserve_the_previous_batch() {
    let device = device();
    for versioned in [false, true] {
        let memory = Memory::new(versioned, false);
        let mut shared = SharedMemory::new(&device);
        // Only page 1 is already used by this batch. A later range checks the
        // changed page 0 first, then must reject page 1 before any data upload.
        shared.sync(&device, &memory, PAGE, PAGE).unwrap();
        let (chunk, offset) = shared.contiguous(0, BYTES as u32).unwrap();
        let gpu = pattern(0xC1);
        device.queue().write_buffer(&shared.chunks[chunk], offset, &gpu);
        shared.mark_gpu_written(PAGE, PAGE);
        assert!(shared.gpu_covers(&memory, &[(PAGE, PAGE)]));
        shared.sync(&device, &memory, PAGE, PAGE).unwrap();
        expect_bytes(&readback(&device, &shared), &gpu);
        memory.replace(0x38);
        let uploaded = shared.uploaded;
        assert_eq!(shared.sync(&device, &memory, 0, BYTES as u32), Err(Conflict));
        assert_eq!(shared.uploaded, uploaded);
        expect_bytes(&readback(&device, &shared), &gpu);
        shared.end_batch();
        shared.sync(&device, &memory, 0, BYTES as u32).unwrap();
        expect_bytes(&readback(&device, &shared), &pattern(0x38));
        assert!(!shared.gpu_covers(&memory, &[(PAGE, PAGE)]));
    }
}
