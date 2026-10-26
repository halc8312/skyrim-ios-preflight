use skyrim_native_gpu::compute::StorageBinding;
use skyrim_native_gpu::{Config, Error, Gpu, backend::Platform, types::BufferUses as U};
use std::{
    sync::{
        Mutex, Once,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};

const DEADLINE: Duration = Duration::from_secs(5);
static TEST_LOCK: Mutex<()> = Mutex::new(());
static ERRORS: AtomicUsize = AtomicUsize::new(0);
static SYNC_HAZARDS: AtomicUsize = AtomicUsize::new(0);
struct Logger;
impl log::Log for Logger {
    fn enabled(&self, _: &log::Metadata<'_>) -> bool {
        true
    }
    fn log(&self, record: &log::Record<'_>) {
        if record.level() == log::Level::Error {
            ERRORS.fetch_add(1, Ordering::SeqCst);
        }
        if record
            .args()
            .to_string()
            .contains("SYNC-HAZARD-WRITE-AFTER-WRITE")
        {
            SYNC_HAZARDS.fetch_add(1, Ordering::SeqCst);
        }
        if record.level() <= log::Level::Info {
            eprintln!("{} {}", record.level(), record.args());
        }
    }
    fn flush(&self) {}
}
fn setup() -> (std::sync::MutexGuard<'static, ()>, usize) {
    let lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        log::set_logger(&Logger).unwrap();
        log::set_max_level(log::LevelFilter::Trace);
    });
    #[cfg(not(target_vendor = "apple"))]
    {
        assert_eq!(
            std::env::var("VK_INSTANCE_LAYERS").as_deref(),
            Ok("VK_LAYER_KHRONOS_validation"),
            "run with the real Khronos validation layer; do not silently skip validation"
        );
        assert!(
            std::env::var("VK_LAYER_ENABLES")
                .unwrap_or_default()
                .contains("VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
        );
        static CANARY: Once = Once::new();
        CANARY.call_once(|| {
            let before = SYNC_HAZARDS.load(Ordering::SeqCst);
            let errors = ERRORS.load(Ordering::SeqCst);
            let mut gpu = open();
            gpu.synchronization_validation_canary().unwrap();
            drop(gpu);
            assert!(
                SYNC_HAZARDS.load(Ordering::SeqCst) > before,
                "synchronization validator is not active"
            );
            assert!(
                ERRORS.load(Ordering::SeqCst) > errors,
                "validation logger is not active"
            );
            eprintln!(
                "Expected discarded-canary hazard observed; real tests must add zero errors."
            );
        });
    }
    (lock, ERRORS.load(Ordering::SeqCst))
}
fn finish(before: usize) {
    assert_eq!(
        ERRORS.load(Ordering::SeqCst),
        before,
        "GPU validation errors"
    );
}

fn open() -> Gpu<Platform> {
    open_config(Config::default())
}
fn open_config(mut config: Config) -> Gpu<Platform> {
    config.validation = true;
    let gpu = Gpu::<Platform>::open_with_config(config).unwrap();
    #[cfg(target_os = "linux")]
    assert!(
        std::fs::read_to_string("/proc/self/maps")
            .unwrap()
            .contains("libVkLayer_khronos_validation.so"),
        "the configured Vulkan validation layer was not actually loaded"
    );
    gpu
}

#[test]
fn uploads_recorded_after_commands_are_ordered_before_them_and_resources_survive_drop() {
    let (_lock, before) = setup();
    let mut gpu = open();
    eprintln!("adapter {}", gpu.adapter_name);
    let src = gpu
        .create_buffer("input", 64, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let first = gpu
        .create_buffer("first output", 64, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let second = gpu
        .create_buffer("second output", 64, U::COPY_DST | U::MAP_READ)
        .unwrap();
    gpu.write_buffer(&src, 0, &[0x11; 64]).unwrap();
    let mut commands = gpu.encoder().unwrap();
    commands.copy_buffer(&src, 0, &first, 0, 64).unwrap();
    gpu.write_buffer(&src, 0, &[0x22; 64]).unwrap();
    let one = gpu.submit(vec![commands.finish().unwrap()]).unwrap();
    let mut commands = gpu.encoder().unwrap();
    commands.copy_buffer(&src, 0, &second, 0, 64).unwrap();
    gpu.write_buffer(&src, 0, &[0x33; 64]).unwrap();
    drop(src); // pending upload + already recorded command both retain it
    let two = gpu.submit(vec![commands.finish().unwrap()]).unwrap();
    assert_eq!(two, one + 1);
    gpu.wait(two, DEADLINE).unwrap();
    assert_eq!(
        gpu.read_buffer(&first, 0..64, DEADLINE).unwrap(),
        [0x22; 64]
    );
    assert_eq!(
        gpu.read_buffer(&second, 0..64, DEADLINE).unwrap(),
        [0x33; 64]
    );
    drop(first);
    drop(second);
    gpu.poll().unwrap();
    assert_eq!(gpu.live_buffer_count(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn initial_zeroing_and_submission_order_are_independent_of_encoder_creation_order() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let a = gpu
        .create_buffer("a", 256, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let b = gpu
        .create_buffer("b", 256, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let out = gpu
        .create_buffer("out", 256, U::MAP_READ | U::COPY_DST)
        .unwrap();
    assert_eq!(
        gpu.read_buffer(&out, 0..256, DEADLINE).unwrap(),
        vec![0; 256]
    );
    // Record B before A. Their actual execution order must be A then B.
    let mut last = gpu.encoder().unwrap();
    last.copy_buffer(&b, 0, &out, 0, 256).unwrap();
    let mut first = gpu.encoder().unwrap();
    first.copy_buffer(&a, 0, &b, 0, 256).unwrap();
    let data: Vec<u8> = (0..256).map(|n| (n ^ 0x56) as u8).collect();
    gpu.write_buffer(&a, 0, &data).unwrap();
    gpu.submit(vec![first.finish().unwrap(), last.finish().unwrap()])
        .unwrap();
    drop(a);
    drop(b);
    assert_eq!(gpu.read_buffer(&out, 0..256, DEADLINE).unwrap(), data);
    drop(out);
    drop(gpu);
    finish(before);
}

#[test]
fn empty_submissions_complete_and_invalid_ranges_do_not_mutate_a_valid_encoder() {
    let (_lock, before) = setup();
    let mut gpu = open();
    for serial in 1..=20 {
        assert_eq!(gpu.submit(Vec::new()).unwrap(), serial);
        gpu.wait(serial, DEADLINE).unwrap();
    }
    let a = gpu
        .create_buffer("source", 16, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let b = gpu
        .create_buffer("output", 16, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let read_only = gpu.create_buffer("read only", 16, U::MAP_READ).unwrap();
    assert!(gpu.write_buffer(&read_only, 0, &[1; 16]).is_err());
    let mut cmd = gpu.encoder().unwrap();
    assert!(matches!(
        cmd.copy_buffer(&a, 1, &b, 0, 4),
        Err(Error::Rejected(_))
    ));
    assert!(cmd.copy_buffer(&a, 0, &b, 0, 20).is_err());
    assert!(cmd.copy_buffer(&a, u64::MAX - 3, &b, 0, 8).is_err());
    assert!(cmd.copy_buffer(&a, 0, &a, 8, 4).is_err());
    gpu.write_buffer(&a, 0, &[0xab; 16]).unwrap();
    cmd.copy_buffer(&a, 0, &b, 0, 16).unwrap();
    gpu.submit(vec![cmd.finish().unwrap()]).unwrap();
    assert_eq!(gpu.read_buffer(&b, 0..16, DEADLINE).unwrap(), [0xab; 16]);
    drop(a);
    drop(b);
    drop(read_only);
    drop(gpu);
    finish(before);
}

#[test]
fn repeated_upload_drop_and_retirement_does_not_retain_native_buffers() {
    let (_lock, before) = setup();
    let mut gpu = open();
    for round in 0..100 {
        let src = gpu
            .create_buffer("churn source", 1024, U::COPY_SRC | U::COPY_DST)
            .unwrap();
        let dst = gpu
            .create_buffer("churn result", 1024, U::COPY_DST | U::MAP_READ)
            .unwrap();
        gpu.write_buffer(&src, 0, &vec![round as u8; 1024]).unwrap();
        let mut cmd = gpu.encoder().unwrap();
        cmd.copy_buffer(&src, 0, &dst, 0, 1024).unwrap();
        drop(src);
        gpu.submit(vec![cmd.finish().unwrap()]).unwrap();
        assert_eq!(
            gpu.read_buffer(&dst, 0..1024, DEADLINE).unwrap(),
            vec![round as u8; 1024]
        );
        drop(dst);
        gpu.poll().unwrap();
        assert_eq!(
            gpu.live_buffer_count(),
            0,
            "retained buffers at round {round}"
        );
    }
    drop(gpu);
    finish(before);
}

#[test]
fn compute_dependencies_and_layouts_survive_drop_and_invalid_binding_is_rejected() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = "@group(0) @binding(0) var<storage,read> src: array<u32>;\n\
        @group(0) @binding(1) var<storage,read_write> dst: array<u32>;\n\
        @compute @workgroup_size(4) fn main(@builtin(global_invocation_id) id:vec3<u32>) { dst[id.x]=src[id.x]*3u+7u; }";
    let pipeline = gpu.create_compute(shader, "main").unwrap();
    let equivalent = gpu.create_compute(shader, "main").unwrap();
    let input = gpu
        .create_buffer("compute input", 16, U::STORAGE_READ_ONLY | U::COPY_DST)
        .unwrap();
    let output = gpu
        .create_buffer("compute output", 16, U::STORAGE_READ_WRITE | U::COPY_SRC)
        .unwrap();
    let readback = gpu
        .create_buffer("compute readback", 16, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let aliased = gpu
        .create_buffer(
            "rejected alias",
            16,
            U::STORAGE_READ_ONLY | U::STORAGE_READ_WRITE,
        )
        .unwrap();
    assert!(
        gpu.bind_compute(
            &pipeline,
            &[
                StorageBinding {
                    binding: 0,
                    buffer: &aliased,
                    offset: 0,
                    size: 16
                },
                StorageBinding {
                    binding: 1,
                    buffer: &aliased,
                    offset: 0,
                    size: 16
                },
            ]
        )
        .is_err()
    );
    assert!(
        gpu.bind_compute(
            &pipeline,
            &[
                StorageBinding {
                    binding: 0,
                    buffer: &input,
                    offset: 4,
                    size: 16
                },
                StorageBinding {
                    binding: 1,
                    buffer: &output,
                    offset: 0,
                    size: 16
                },
            ]
        )
        .is_err()
    );
    let bindings = gpu
        .bind_compute(
            &pipeline,
            &[
                StorageBinding {
                    binding: 0,
                    buffer: &input,
                    offset: 0,
                    size: 16,
                },
                StorageBinding {
                    binding: 1,
                    buffer: &output,
                    offset: 0,
                    size: 16,
                },
            ],
        )
        .unwrap();
    let mut commands = gpu.encoder().unwrap();
    assert!(
        commands
            .dispatch(&equivalent, &bindings, [u32::MAX, 1, 1])
            .is_err()
    );
    commands
        .dispatch(&equivalent, &bindings, [1, 1, 1])
        .unwrap();
    commands.copy_buffer(&output, 0, &readback, 0, 16).unwrap();
    let words = [1u32, 7, 100, 0x7fc00001];
    gpu.write_buffer(
        &input,
        0,
        &words
            .iter()
            .flat_map(|n| n.to_le_bytes())
            .collect::<Vec<_>>(),
    )
    .unwrap();
    drop(input);
    drop(output);
    drop(aliased);
    drop(bindings);
    drop(pipeline);
    drop(equivalent);
    gpu.submit(vec![commands.finish().unwrap()]).unwrap();
    let expected: Vec<_> = words
        .iter()
        .flat_map(|n| n.wrapping_mul(3).wrapping_add(7).to_le_bytes())
        .collect();
    assert_eq!(
        gpu.read_buffer(&readback, 0..16, DEADLINE).unwrap(),
        expected
    );
    drop(readback);
    gpu.poll().unwrap();
    assert_eq!(gpu.live_buffer_count(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn production_constant_kernel_matches_an_independent_integer_oracle_on_native_backend() {
    let (_lock, before) = setup();
    let mut gpu = open();
    // This is public renderer source, not a game's translated shader or payload.
    let shader = xenos_gfx::constants::BINNED_SHADER;
    let pipeline = gpu.create_compute(shader, "main").unwrap();
    for count in [6usize, 27, 68, 136, 1024] {
        let (records, values, expected) = authored_constant_batch(count);
        let words: Vec<_> = records
            .chunks_exact(4)
            .map(|v| u32::from_le_bytes(v.try_into().unwrap()))
            .collect();
        let mut cpu = vec![0xA5; expected.len()];
        xenos_gfx::constants::ConstantPlan::validate(&words, &values, cpu.len())
            .unwrap()
            .execute(&mut cpu)
            .unwrap();
        assert_eq!(cpu, expected, "independent CPU constants records={count}");
        let input = gpu
            .create_buffer(
                "patch records",
                records.len() as u64,
                U::STORAGE_READ_ONLY | U::COPY_DST,
            )
            .unwrap();
        let changed = gpu
            .create_buffer(
                "patch values",
                values.len() as u64,
                U::STORAGE_READ_ONLY | U::COPY_DST,
            )
            .unwrap();
        let output = gpu
            .create_buffer(
                "patch result",
                expected.len() as u64,
                U::STORAGE_READ_WRITE | U::COPY_DST | U::COPY_SRC,
            )
            .unwrap();
        let readback = gpu
            .create_buffer(
                "patch readback",
                expected.len() as u64,
                U::MAP_READ | U::COPY_DST,
            )
            .unwrap();
        let bindings = gpu
            .bind_compute(
                &pipeline,
                &[
                    StorageBinding {
                        binding: 0,
                        buffer: &input,
                        offset: 0,
                        size: input.size(),
                    },
                    StorageBinding {
                        binding: 1,
                        buffer: &changed,
                        offset: 0,
                        size: changed.size(),
                    },
                    StorageBinding {
                        binding: 2,
                        buffer: &output,
                        offset: 0,
                        size: output.size(),
                    },
                ],
            )
            .unwrap();
        for _ in 0..2 {
            // Reset BOTH output and readback; a no-op/stale result cannot pass.
            gpu.write_buffer(&output, 0, &vec![0xa5; expected.len()])
                .unwrap();
            gpu.write_buffer(&readback, 0, &vec![0x5a; expected.len()])
                .unwrap();
            let mut commands = gpu.encoder().unwrap();
            commands.dispatch(&pipeline, &bindings, [5, 1, 1]).unwrap();
            commands
                .copy_buffer(&output, 0, &readback, 0, output.size())
                .unwrap();
            gpu.write_buffer(&input, 0, &records).unwrap();
            gpu.write_buffer(&changed, 0, &values).unwrap();
            gpu.submit(vec![commands.finish().unwrap()]).unwrap();
            assert_eq!(
                gpu.read_buffer(&readback, 0..readback.size(), DEADLINE)
                    .unwrap(),
                expected,
                "records={count}"
            );
        }
        drop(bindings);
        drop(input);
        drop(changed);
        drop(output);
        drop(readback);
        gpu.poll().unwrap();
        assert_eq!(gpu.live_buffer_count(), 0);
    }
    drop(pipeline);
    drop(gpu);
    finish(before);
}

#[path = "../../fixtures/constants.rs"]
mod constant_fixture;
fn authored_constant_batch(count: usize) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    constant_fixture::batch(count, 0)
}

#[test]
fn bounded_uploads_and_native_pool_reuse_have_independent_hal_counters() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 2 << 20;
    let mut gpu = open_config(config);
    let buffer = gpu
        .create_buffer("budget", 1 << 20, U::COPY_DST | U::MAP_READ)
        .unwrap();
    assert_eq!(gpu.native_counters().buffers.read(), 1);
    gpu.write_buffer(&buffer, 0, &vec![0x11; 1 << 20]).unwrap();
    gpu.write_buffer(&buffer, 0, &vec![0x22; 1 << 20]).unwrap();
    assert!(matches!(
        gpu.write_buffer(&buffer, 0, &vec![0x33; 1 << 20]),
        Err(Error::Rejected(_))
    ));
    assert_eq!(
        gpu.read_buffer(&buffer, 0..64, DEADLINE).unwrap(),
        [0x22; 64]
    );
    drop(buffer);
    gpu.poll().unwrap();
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    // Warm up one explicit empty submission and the pool, then repeat identical
    // work. HAL allocations must stay flat rather than leak one per submit.
    let s = gpu.submit(vec![]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    let created = gpu.encoder_stats().0;
    for _ in 0..100 {
        let s = gpu.submit(vec![]).unwrap();
        gpu.wait(s, DEADLINE).unwrap();
    }
    assert_eq!(gpu.encoder_stats().0, created);
    assert!(gpu.encoder_stats().1 >= 100);
    gpu.trim_encoder_pool();
    assert_eq!(gpu.native_counters().command_encoders.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn intra_encoder_read_after_write_and_write_after_write_are_synchronized() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let a = gpu
        .create_buffer("a", 64, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let b = gpu
        .create_buffer("b", 64, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let c = gpu
        .create_buffer("c", 64, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let out = gpu
        .create_buffer("readback", 64, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let mut commands = gpu.encoder().unwrap();
    commands.copy_buffer(&a, 0, &b, 0, 64).unwrap();
    commands.copy_buffer(&b, 0, &c, 0, 64).unwrap(); // RAW
    commands.clear_buffer(&b, 0..64).unwrap();
    commands.copy_buffer(&c, 0, &b, 0, 64).unwrap(); // WAW
    commands.copy_buffer(&b, 0, &out, 0, 64).unwrap();
    gpu.write_buffer(&a, 0, &[0x91; 64]).unwrap();
    gpu.submit(vec![commands.finish().unwrap()]).unwrap();
    assert_eq!(gpu.read_buffer(&out, 0..64, DEADLINE).unwrap(), [0x91; 64]);
    drop(a);
    drop(b);
    drop(c);
    drop(out);
    gpu.poll().unwrap();
    gpu.trim_encoder_pool();
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    assert_eq!(gpu.native_counters().command_encoders.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn failure_preserves_the_earlier_prefix_and_retires_terminal_resources() {
    let (_lock, before) = setup();
    let mut gpu = open();
    for _ in 0..3 {
        let buffer = gpu
            .create_buffer("terminal ownership", 64, U::COPY_DST)
            .unwrap();
        gpu.write_buffer(&buffer, 0, &[0x71; 64]).unwrap();
        gpu.submit(vec![]).unwrap();
        drop(buffer);
    }
    gpu.fail_completion(2);
    gpu.wait(1, DEADLINE).unwrap();
    assert!(matches!(
        gpu.wait(2, DEADLINE),
        Err(Error::Failed { serial: 2, .. })
    ));
    assert!(matches!(
        gpu.wait(3, DEADLINE),
        Err(Error::Failed { serial: 2, .. })
    ));
    assert!(matches!(
        gpu.submit(vec![]),
        Err(Error::Failed { serial: 2, .. })
    ));
    assert!(matches!(
        gpu.create_buffer("rejected", 64, U::COPY_DST),
        Err(Error::Failed { serial: 2, .. })
    ));
    let deadline = std::time::Instant::now() + DEADLINE;
    while gpu.in_flight_count() != 0 && std::time::Instant::now() < deadline {
        let _ = gpu.poll();
    }
    assert_eq!(gpu.in_flight_count(), 0);
    assert_eq!(gpu.verified_serial(), 1);
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    gpu.trim_encoder_pool();
    assert_eq!(gpu.native_counters().command_encoders.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn backpressure_timeout_is_fail_stop_and_does_not_lose_a_retryable_submission() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_in_flight = 2;
    config.backpressure_timeout = Duration::from_millis(10);
    let mut gpu = open_config(config);
    gpu.delay_fence_observation(Some(0));
    gpu.submit(vec![]).unwrap();
    gpu.submit(vec![]).unwrap();
    let buffer = gpu.create_buffer("pending", 64, U::COPY_DST).unwrap();
    gpu.write_buffer(&buffer, 0, &[0x83; 64]).unwrap();
    assert!(matches!(
        gpu.submit(vec![]),
        Err(Error::Failed { serial: 3, .. })
    ));
    assert_eq!(gpu.verified_serial(), 0);
    assert_eq!(gpu.in_flight_count(), 2);
    assert!(gpu.write_buffer(&buffer, 0, &[0; 64]).is_err());
    gpu.delay_fence_observation(None);
    gpu.wait(2, DEADLINE).unwrap();
    assert_eq!(gpu.in_flight_count(), 0);
    assert_eq!(gpu.verified_serial(), 2);
    // Pending writes are still owned and are discarded only by failed-session
    // teardown; callers can never continue while silently assuming they ran.
    drop(buffer);
    drop(gpu);
    finish(before);
}

#[test]
fn closed_encoder_failure_and_all_unsubmitted_drop_paths_are_safe() {
    let (_lock, before) = setup();
    let mut gpu = open();
    {
        let buffer = gpu
            .create_buffer("dropped recording", 64, U::COPY_DST)
            .unwrap();
        let mut encoder = gpu.encoder().unwrap();
        encoder.clear_buffer(&buffer, 0..64).unwrap();
    }
    let encoder = gpu.encoder().unwrap();
    drop(encoder.finish().unwrap());
    let s = gpu.submit(vec![]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    gpu.trim_encoder_pool();
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    assert_eq!(gpu.native_counters().command_encoders.read(), 0);
    let buffer = gpu
        .create_buffer("failed pending finish", 64, U::COPY_DST | U::MAP_READ)
        .unwrap();
    gpu.fail_next_end_encoding();
    assert!(matches!(gpu.submit(vec![]), Err(Error::Failed { .. })));
    assert!(gpu.write_buffer(&buffer, 0, &[0; 64]).is_err());
    assert!(gpu.read_buffer(&buffer, 0..64, DEADLINE).is_err());
    drop(buffer);
    gpu.trim_encoder_pool();
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    assert_eq!(gpu.native_counters().command_encoders.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn submit_error_quarantines_only_unknown_work_with_bounded_teardown() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.drop_timeout = Duration::from_millis(20);
    let mut gpu = open_config(config);
    let buffer = gpu.create_buffer("quarantined", 64, U::COPY_DST).unwrap();
    gpu.write_buffer(&buffer, 0, &[0x37; 64]).unwrap();
    drop(buffer);
    gpu.fail_next_submit();
    assert!(matches!(
        gpu.submit(vec![]),
        Err(Error::Failed { serial: 1, .. })
    ));
    assert_eq!(gpu.in_flight_count(), 1);
    assert_eq!(gpu.verified_serial(), 0);
    // The deliberate fault sends nothing, but production must treat ambiguous
    // submit errors conservatively. Keep native references instead of freeing.
    assert_eq!(gpu.native_counters().buffers.read(), 2);
    let started = std::time::Instant::now();
    drop(gpu);
    assert!(started.elapsed() < Duration::from_secs(1));
    finish(before);
}

#[test]
fn textures_preserve_mips_layers_copy_order_and_row_padding() {
    use skyrim_native_gpu::{
        texture::TextureDescriptor,
        types::{
            Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let desc = TextureDescriptor {
        size: Extent3d {
            width: 7,
            height: 5,
            depth_or_array_layers: 3,
        },
        mip_levels: 3,
        sample_count: 1,
        dimension: D::D2,
        format: F::Rgba8Unorm,
        usage: T::COPY_SRC | T::COPY_DST | T::RESOURCE,
    };
    let a = gpu.create_texture("mipped array a", desc.clone()).unwrap();
    let b = gpu.create_texture("mipped array b", desc).unwrap();
    let view = gpu.texture_view(&b, V::D2Array, 1..3, 1..3).unwrap();
    assert!(gpu.texture_view(&b, V::Cube, 0..1, 0..3).is_err());
    assert!(gpu.texture_view(&b, V::D2, 0..1, 0..2).is_err());
    assert!(gpu.texture_view(&b, V::D2Array, 2..4, 0..1).is_err());
    let mut readbacks = Vec::new();
    for mip in 0..3 {
        let size = a.mip_size(mip).unwrap();
        let bytes = 256 * size.height * size.depth_or_array_layers;
        let upload = gpu
            .create_buffer("pixels", u64::from(bytes), U::COPY_DST | U::COPY_SRC)
            .unwrap();
        let readback = gpu
            .create_buffer("readback", u64::from(bytes), U::COPY_DST | U::MAP_READ)
            .unwrap();
        let layout = L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(size.height),
        };
        // Build the later copy first: states must follow submission, not creation.
        let mut second = gpu.encoder().unwrap();
        second
            .copy_texture(&a, mip, Origin3d::ZERO, &b, mip, Origin3d::ZERO, size)
            .unwrap();
        second
            .copy_texture_to_buffer(&b, mip, Origin3d::ZERO, size, &readback, layout)
            .unwrap();
        let mut first = gpu.encoder().unwrap();
        first
            .copy_buffer_to_texture(&upload, layout, &a, mip, Origin3d::ZERO, size)
            .unwrap();
        assert!(
            first
                .copy_texture(&a, mip, Origin3d::ZERO, &a, mip, Origin3d::ZERO, size)
                .is_err()
        );
        assert!(
            first
                .copy_buffer_to_texture(
                    &upload,
                    L {
                        bytes_per_row: Some(28),
                        ..layout
                    },
                    &a,
                    mip,
                    Origin3d::ZERO,
                    size
                )
                .is_err()
        );
        let mut payload = vec![0x33; bytes as usize];
        let mut expected = vec![0x5a; bytes as usize];
        for z in 0..size.depth_or_array_layers {
            for y in 0..size.height {
                for x in 0..size.width {
                    let offset = (z * size.height * 256 + y * 256 + x * 4) as usize;
                    let pixel = [mip as u8 + 1, z as u8 + 10, x as u8, y as u8 + 20];
                    payload[offset..offset + 4].copy_from_slice(&pixel);
                    expected[offset..offset + 4].copy_from_slice(&pixel);
                }
            }
        }
        gpu.write_buffer(&upload, 0, &payload).unwrap();
        gpu.write_buffer(&readback, 0, &vec![0x5a; bytes as usize])
            .unwrap();
        drop(upload);
        gpu.submit(vec![first.finish().unwrap(), second.finish().unwrap()])
            .unwrap();
        readbacks.push((readback, expected));
    }
    drop(a);
    drop(b);
    drop(view);
    for (readback, expected) in readbacks {
        assert_eq!(
            gpu.read_buffer(&readback, 0..readback.size(), DEADLINE)
                .unwrap(),
            expected
        );
    }
    gpu.poll().unwrap();
    assert_eq!(gpu.native_counters().texture_memory.read(), 0);
    assert_eq!(gpu.native_counters().texture_views.read(), 0);
    // The documented immutable 1 MiB zero source remains session-owned.
    assert_eq!(gpu.live_buffer_count(), 1);
    drop(gpu);
    finish(before);
}

#[test]
fn volume_and_one_dimensional_textures_are_zeroed_and_copied_without_layer_confusion() {
    use skyrim_native_gpu::{
        texture::TextureDescriptor,
        types::{
            Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    for (dimension, size, mips) in [
        (
            D::D3,
            Extent3d {
                width: 9,
                height: 7,
                depth_or_array_layers: 5,
            },
            4,
        ),
        (
            D::D1,
            Extent3d {
                width: 19,
                height: 1,
                depth_or_array_layers: 1,
            },
            1,
        ),
    ] {
        let texture = gpu
            .create_texture(
                "volume/line",
                TextureDescriptor {
                    size,
                    mip_levels: mips,
                    sample_count: 1,
                    dimension,
                    format: F::R8Unorm,
                    usage: T::COPY_SRC | T::COPY_DST,
                },
            )
            .unwrap();
        for mip in 0..mips {
            let size = texture.mip_size(mip).unwrap();
            let bytes = 256 * size.height * size.depth_or_array_layers;
            let readback = gpu
                .create_buffer("zero readback", u64::from(bytes), U::COPY_DST | U::MAP_READ)
                .unwrap();
            let layout = L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(size.height),
            };
            let mut c = gpu.encoder().unwrap();
            c.copy_texture_to_buffer(&texture, mip, Origin3d::ZERO, size, &readback, layout)
                .unwrap();
            gpu.write_buffer(&readback, 0, &vec![0x5a; bytes as usize])
                .unwrap();
            gpu.submit(vec![c.finish().unwrap()]).unwrap();
            let mut expected = vec![0x5a; bytes as usize];
            for z in 0..size.depth_or_array_layers {
                for y in 0..size.height {
                    expected[(z * size.height * 256 + y * 256) as usize
                        ..(z * size.height * 256 + y * 256 + size.width) as usize]
                        .fill(0);
                }
            }
            assert_eq!(
                gpu.read_buffer(&readback, 0..u64::from(bytes), DEADLINE)
                    .unwrap(),
                expected
            );
        }
        if dimension == D::D3 {
            let upload = gpu
                .create_buffer("one volume patch", 256 * 3 * 2, U::COPY_DST | U::COPY_SRC)
                .unwrap();
            let out = gpu
                .create_buffer("one volume read", 256 * 3 * 2, U::COPY_DST | U::MAP_READ)
                .unwrap();
            let patch = Extent3d {
                width: 5,
                height: 3,
                depth_or_array_layers: 2,
            };
            let origin = Origin3d { x: 2, y: 1, z: 2 };
            let layout = L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(3),
            };
            let mut c = gpu.encoder().unwrap();
            c.copy_buffer_to_texture(&upload, layout, &texture, 0, origin, patch)
                .unwrap();
            c.copy_texture_to_buffer(&texture, 0, origin, patch, &out, layout)
                .unwrap();
            gpu.write_buffer(&upload, 0, &vec![0x92; 256 * 3 * 2])
                .unwrap();
            gpu.submit(vec![c.finish().unwrap()]).unwrap();
            let bytes = gpu.read_buffer(&out, 0..out.size(), DEADLINE).unwrap();
            for row in bytes.chunks(256) {
                assert_eq!(&row[..5], &[0x92; 5]);
                assert!(row[5..].iter().all(|&n| n == 0));
            }
        }
    }
    gpu.poll().unwrap();
    assert_eq!(gpu.native_counters().texture_memory.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn native_render_plan_samples_texture_uses_dynamic_uniforms_holes_scissors_and_retains_resources() {
    use skyrim_native_gpu::{
        bindings::{Entry, Resource},
        render::{BoundGroup, ColorAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    use std::num::NonZeroU64;
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader=gpu.shader(r#"
struct VertexOut { @builtin(position) position:vec4f, @location(0) uv:vec2f }
@group(0) @binding(0) var<uniform> tint:vec4f;
@group(1) @binding(0) var pixels:texture_2d<f32>;
@group(1) @binding(1) var point:sampler;
@vertex fn vs(@builtin(vertex_index) index:u32)->VertexOut {
 let pos=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));
 var out:VertexOut;out.position=vec4f(pos[index],0.,1.);out.uv=pos[index]*vec2f(0.5,-0.5)+0.5;return out;
}
@fragment fn fs(in:VertexOut)->@location(1) vec4f {return textureSample(pixels,point,in.uv)*tint;}
"#).unwrap();
    let uniforms = gpu
        .group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Buffer {
                ty: types::BufferBindingType::Uniform,
                has_dynamic_offset: true,
                min_binding_size: NonZeroU64::new(16),
            },
            count: None,
        }])
        .unwrap();
    assert_eq!(
        uniforms.id(),
        gpu.group_layout(uniforms.entries()).unwrap().id()
    );
    let sampling = gpu
        .group_layout(&[
            types::BindGroupLayoutEntry {
                binding: 0,
                visibility: types::ShaderStages::FRAGMENT,
                ty: types::BindingType::Texture {
                    sample_type: types::TextureSampleType::Float { filterable: true },
                    view_dimension: V::D2,
                    multisampled: false,
                },
                count: None,
            },
            types::BindGroupLayoutEntry {
                binding: 1,
                visibility: types::ShaderStages::FRAGMENT,
                ty: types::BindingType::Sampler(types::SamplerBindingType::Filtering),
                count: None,
            },
        ])
        .unwrap();
    let layout = gpu
        .pipeline_layout(&[uniforms.clone(), sampling.clone()])
        .unwrap();
    let colors = [
        None,
        Some(types::ColorTargetState {
            format: F::Rgba8Unorm,
            blend: None,
            write_mask: types::ColorWrites::ALL,
        }),
    ];
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &colors,
            depth_stencil: None,
            multisample: Default::default(),
            primitive: Default::default(),
        })
        .unwrap();
    let constants = gpu
        .create_buffer(
            "dynamic colors",
            512,
            U::UNIFORM | U::COPY_DST | U::STORAGE_READ_WRITE,
        )
        .unwrap();
    // Use the renderer's actual PUBLIC constant kernel as the uniform producer.
    // The authored fixture contains no game's translated shader/data.
    let producer = gpu
        .create_compute(xenos_gfx::constants::BINNED_SHADER, "main")
        .unwrap();
    let records = gpu
        .create_buffer(
            "producer records",
            42 * 4,
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )
        .unwrap();
    let values = gpu
        .create_buffer("producer values", 32, U::STORAGE_READ_ONLY | U::COPY_DST)
        .unwrap();
    let producer_bindings = gpu
        .bind_compute(
            &producer,
            &[
                StorageBinding {
                    binding: 0,
                    buffer: &records,
                    offset: 0,
                    size: records.size(),
                },
                StorageBinding {
                    binding: 1,
                    buffer: &values,
                    offset: 0,
                    size: values.size(),
                },
                StorageBinding {
                    binding: 2,
                    buffer: &constants,
                    offset: 0,
                    size: constants.size(),
                },
            ],
        )
        .unwrap();
    let group0 = gpu
        .bind_group(
            &uniforms,
            &[Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: &constants,
                    offset: 0,
                    size: 16,
                },
            }],
        )
        .unwrap();
    let texture = gpu
        .create_texture(
            "sample white",
            TextureDescriptor {
                size: Extent3d {
                    width: 2,
                    height: 2,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::RESOURCE | T::COPY_DST,
            },
        )
        .unwrap();
    let sampleview = gpu.texture_view(&texture, V::D2, 0..1, 0..1).unwrap();
    let sampler = gpu.sampler(&Default::default()).unwrap();
    let group1 = gpu
        .bind_group(
            &sampling,
            &[
                Entry {
                    binding: 0,
                    resource: Resource::Texture(&sampleview),
                },
                Entry {
                    binding: 1,
                    resource: Resource::Sampler(&sampler),
                },
            ],
        )
        .unwrap();
    let target = gpu
        .create_texture(
            "render target",
            TextureDescriptor {
                size: Extent3d {
                    width: 8,
                    height: 4,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let pixels = gpu
        .create_buffer("texture upload", 512, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let readback = gpu
        .create_buffer("pixels readback", 256 * 4, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut encoder = gpu.encoder().unwrap();
    encoder
        .copy_buffer_to_texture(
            &pixels,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(2),
            },
            &texture,
            0,
            Origin3d::ZERO,
            texture.descriptor().size,
        )
        .unwrap();
    encoder
        .dispatch(&producer, &producer_bindings, [5, 1, 1])
        .unwrap();
    let groups_a = [
        BoundGroup {
            group: &group0,
            dynamic_offsets: &[0],
        },
        BoundGroup {
            group: &group1,
            dynamic_offsets: &[],
        },
    ];
    let groups_b = [
        BoundGroup {
            group: &group0,
            dynamic_offsets: &[256],
        },
        BoundGroup {
            group: &group1,
            dynamic_offsets: &[],
        },
    ];
    let draws = [
        Draw {
            pipeline: &pipeline,
            groups: &groups_a,
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: Some([0, 0, 4, 4]),
            stencil_reference: 0,
            blend: types::Color::TRANSPARENT,
        },
        Draw {
            pipeline: &pipeline,
            groups: &groups_b,
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: Some([4, 0, 4, 4]),
            stencil_reference: 0,
            blend: types::Color::TRANSPARENT,
        },
    ];
    let stats = encoder
        .render(
            &[
                None,
                Some(ColorAttachment {
                    view: &view,
                    clear: Some(types::Color::BLACK),
                }),
            ],
            &draws,
        )
        .unwrap();
    assert_eq!(
        (
            stats.draws,
            stats.unique_groups,
            stats.pipeline_binds,
            stats.group_binds
        ),
        (2, 2, 1, 3)
    );
    encoder
        .copy_texture_to_buffer(
            &target,
            0,
            Origin3d::ZERO,
            target.descriptor().size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(4),
            },
        )
        .unwrap();
    gpu.write_buffer(&pixels, 0, &[0xff; 512]).unwrap();
    let mut record_words = [0u32; 42];
    record_words[..4].copy_from_slice(&[2, 40, 2, 1]);
    record_words[16..21].copy_from_slice(&[0, 0, 1, 0, 1]);
    record_words[28..33].copy_from_slice(&[0, 16, 1, 1, 1]);
    record_words[40..].copy_from_slice(&[0, 1]);
    gpu.write_buffer(
        &records,
        0,
        &record_words
            .iter()
            .flat_map(|n| n.to_le_bytes())
            .collect::<Vec<_>>(),
    )
    .unwrap();
    gpu.write_buffer(
        &values,
        0,
        &[1f32, 0., 0., 1., 0., 1., 0., 1.]
            .iter()
            .flat_map(|n| n.to_le_bytes())
            .collect::<Vec<_>>(),
    )
    .unwrap();
    // All shader, pipeline, group, view and upload handles may die before submit.
    drop(producer_bindings);
    drop(producer);
    drop(records);
    drop(values);
    drop(draws);
    drop(groups_a);
    drop(groups_b);
    drop(group0);
    drop(group1);
    drop(constants);
    drop(pixels);
    drop(view);
    drop(target);
    drop(sampleview);
    drop(texture);
    drop(sampler);
    drop(pipeline);
    drop(layout);
    drop(shader);
    drop(uniforms);
    drop(sampling);
    gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
    let bytes = gpu
        .read_buffer(&readback, 0..readback.size(), DEADLINE)
        .unwrap();
    for row in bytes.chunks(256) {
        for x in 0..8 {
            assert_eq!(
                &row[x * 4..x * 4 + 4],
                if x < 4 {
                    &[255, 0, 0, 255]
                } else {
                    &[0, 255, 0, 255]
                }
            );
        }
    }
    drop(readback);
    gpu.poll().unwrap();
    gpu.trim_encoder_pool();
    let c = gpu.native_counters();
    assert_eq!(c.texture_memory.read(), 0);
    assert_eq!(c.texture_views.read(), 0);
    assert_eq!(c.bind_groups.read(), 0);
    assert_eq!(c.render_pipelines.read(), 0);
    assert_eq!(c.shader_modules.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn indexed_render_loads_prior_target_and_preserves_unwritten_color_components() {
    use skyrim_native_gpu::{
        render::{ColorAttachment, Draw, Indices, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = gpu
        .shader(
            r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);
}
@fragment fn red()->@location(0) vec4f {return vec4f(1.,0.,0.,0.);}
@fragment fn green()->@location(0) vec4f {return vec4f(0.,1.,0.,0.);}
"#,
        )
        .unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::RED | types::ColorWrites::GREEN,
    })];
    let make = |entry| {
        gpu.render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, entry)),
            colors: &colors,
            depth_stencil: None,
            multisample: Default::default(),
            primitive: Default::default(),
        })
        .unwrap()
    };
    let red = make("red");
    let green = make("green");
    let target = gpu
        .create_texture(
            "mask target",
            TextureDescriptor {
                size: Extent3d {
                    width: 4,
                    height: 4,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let indices = gpu
        .create_buffer("indices", 8, U::INDEX | U::COPY_DST)
        .unwrap();
    let readback = gpu
        .create_buffer("readback", 1024, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let mut encoder = gpu.encoder().unwrap();
    let make_draw = |p| Draw {
        pipeline: p,
        groups: &[],
        vertices: 0..3,
        instances: 0..1,
        indices: Some(Indices {
            buffer: &indices,
            format: types::IndexFormat::Uint16,
            offset: 0,
            size: 6,
            base_vertex: 0,
        }),
        viewport: None,
        scissor: None,
        stencil_reference: 0,
        blend: types::Color::TRANSPARENT,
    };
    encoder
        .render(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color {
                    r: 0.,
                    g: 0.,
                    b: 1.,
                    a: 1.,
                }),
            })],
            &[make_draw(&red)],
        )
        .unwrap();
    let mut draw = make_draw(&green);
    draw.scissor = Some([2, 0, 2, 4]);
    encoder
        .render(
            &[Some(ColorAttachment {
                view: &view,
                clear: None,
            })],
            &[draw],
        )
        .unwrap();
    encoder
        .copy_texture_to_buffer(
            &target,
            0,
            Origin3d::ZERO,
            target.descriptor().size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(4),
            },
        )
        .unwrap();
    gpu.write_buffer(&indices, 0, &[0, 0, 1, 0, 2, 0, 0, 0])
        .unwrap();
    gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
    let bytes = gpu.read_buffer(&readback, 0..1024, DEADLINE).unwrap();
    for row in bytes.chunks(256) {
        for x in 0..4 {
            assert_eq!(
                &row[x * 4..x * 4 + 4],
                if x < 2 {
                    &[255, 0, 255, 255]
                } else {
                    &[0, 255, 255, 255]
                }
            );
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn rejected_render_plan_has_no_partial_clear_or_draw_effect() {
    use skyrim_native_gpu::{
        render::{ColorAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = gpu
        .shader(
            r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);
}
@fragment fn fs()->@location(0) vec4f {return vec4f(1.,0.,0.,1.);}
"#,
        )
        .unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &colors,
            depth_stencil: None,
            multisample: Default::default(),
            primitive: Default::default(),
        })
        .unwrap();
    let target = gpu
        .create_texture(
            "unmodified target",
            TextureDescriptor {
                size: Extent3d {
                    width: 2,
                    height: 2,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let readback = gpu
        .create_buffer("zero check", 512, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let mut encoder = gpu.encoder().unwrap();
    let make = |scissor| Draw {
        pipeline: &pipeline,
        groups: &[],
        vertices: 0..3,
        instances: 0..1,
        indices: None,
        viewport: None,
        scissor,
        stencil_reference: 0,
        blend: types::Color::TRANSPARENT,
    };
    // First draw is valid; the second must reject BEFORE even the clear runs.
    assert!(
        encoder
            .render(
                &[Some(ColorAttachment {
                    view: &view,
                    clear: Some(types::Color::WHITE)
                })],
                &[make(None), make(Some([0, 0, 99, 2]))]
            )
            .is_err()
    );
    encoder
        .copy_texture_to_buffer(
            &target,
            0,
            Origin3d::ZERO,
            target.descriptor().size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(2),
            },
        )
        .unwrap();
    gpu.write_buffer(&readback, 0, &[0x53; 512]).unwrap();
    gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
    let bytes = gpu.read_buffer(&readback, 0..512, DEADLINE).unwrap();
    for row in bytes.chunks(256) {
        assert_eq!(&row[..8], &[0; 8]);
        assert!(row[8..].iter().all(|&n| n == 0x53));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn four_command_batches_share_one_fixup_encoder_under_a_six_encoder_budget() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_native_encoders = 6;
    config.retained_encoders = 6;
    let mut gpu = open_config(config);
    let src = gpu
        .create_buffer("batch source", 64, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let targets: Vec<_> = (0..4)
        .map(|_| {
            gpu.create_buffer("batch out", 64, U::COPY_DST | U::MAP_READ)
                .unwrap()
        })
        .collect();
    for generation in 1..=30u8 {
        gpu.write_buffer(&src, 0, &[generation; 64]).unwrap();
        let mut commands = Vec::new();
        for target in &targets {
            let mut c = gpu.encoder().unwrap();
            c.copy_buffer(&src, 0, target, 0, 64).unwrap();
            commands.push(c.finish().unwrap());
        }
        gpu.submit(commands).unwrap();
        // Acquiring this readback encoder must retire old flights for capacity,
        // rather than killing the session because five fixup pools were needed.
        assert_eq!(
            gpu.read_buffer(&targets[3], 0..64, DEADLINE).unwrap(),
            [generation; 64]
        );
    }
    assert!(gpu.encoder_stats().0 <= 6);
    drop(src);
    drop(targets);
    gpu.poll().unwrap();
    gpu.trim_encoder_pool();
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    assert_eq!(gpu.native_counters().command_encoders.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn cube_views_require_the_parent_cube_flag_and_valid_layer_shape() {
    use skyrim_native_gpu::{
        texture::TextureDescriptor,
        types::{
            Extent3d, TextureDimension as D, TextureFormat as F, TextureUses as T,
            TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    for layers in [7, 12] {
        let t = gpu
            .create_texture(
                "cube parent shape",
                TextureDescriptor {
                    size: Extent3d {
                        width: 4,
                        height: 4,
                        depth_or_array_layers: layers,
                    },
                    mip_levels: 1,
                    sample_count: 1,
                    dimension: D::D2,
                    format: F::Rgba8Unorm,
                    usage: T::RESOURCE,
                },
            )
            .unwrap();
        if layers == 7 {
            assert!(gpu.texture_view(&t, V::Cube, 0..1, 0..6).is_err());
        } else {
            gpu.texture_view(&t, V::CubeArray, 0..1, 0..12).unwrap();
        }
    }
    let s = gpu.submit(vec![]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    assert_eq!(gpu.native_counters().texture_memory.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn texture_initialization_replaces_poison_in_every_chunk_and_mutant_is_detected() {
    use skyrim_native_gpu::{
        texture::TextureDescriptor,
        types::{
            Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    for (dimension, size, mips, format) in [
        (
            D::D2,
            Extent3d {
                width: 4096,
                height: 40,
                depth_or_array_layers: 1,
            },
            1,
            F::Rgba32Float,
        ), // 16+16+8 row chunks
        (
            D::D2,
            Extent3d {
                width: 7,
                height: 5,
                depth_or_array_layers: 3,
            },
            3,
            F::Rgba8Unorm,
        ),
        (
            D::D3,
            Extent3d {
                width: 9,
                height: 7,
                depth_or_array_layers: 5,
            },
            4,
            F::R8Unorm,
        ),
    ] {
        for skip_last in [false, true] {
            gpu.test_texture_initialization(true, skip_last);
            let t = gpu
                .create_texture(
                    "poisoned allocation",
                    TextureDescriptor {
                        size,
                        mip_levels: mips,
                        sample_count: 1,
                        dimension,
                        format,
                        usage: T::COPY_SRC,
                    },
                )
                .unwrap();
            let mut detected = false;
            for mip in 0..mips {
                let size = t.mip_size(mip).unwrap();
                let pixel = format.block_copy_size(None).unwrap();
                let pitch = (size.width * pixel).next_multiple_of(256);
                let bytes = u64::from(pitch)
                    * u64::from(size.height)
                    * u64::from(size.depth_or_array_layers);
                let out = gpu
                    .create_buffer("poison readback", bytes, U::MAP_READ | U::COPY_DST)
                    .unwrap();
                let mut c = gpu.encoder().unwrap();
                c.copy_texture_to_buffer(
                    &t,
                    mip,
                    Origin3d::ZERO,
                    size,
                    &out,
                    L {
                        offset: 0,
                        bytes_per_row: Some(pitch),
                        rows_per_image: Some(size.height),
                    },
                )
                .unwrap();
                gpu.submit(vec![c.finish().unwrap()]).unwrap();
                let data = gpu.read_buffer(&out, 0..bytes, DEADLINE).unwrap();
                for row in data.chunks(pitch as usize) {
                    for &byte in &row[..(size.width * pixel) as usize] {
                        if byte != 0 {
                            assert_eq!(byte, 0xa5);
                            detected = true;
                        }
                    }
                }
            }
            assert_eq!(
                detected, skip_last,
                "oracle must catch skipped zeroing; shape={size:?}"
            );
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn depth_stencil_loads_and_multisample_resolves_preserve_nearest_pixels() {
    use skyrim_native_gpu::{
        render::{ColorAttachment, DepthAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader=gpu.shader(r#"
fn pos(i:u32,z:f32)->vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],z,1.);}
@vertex fn near(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {return pos(i,0.25);}
@vertex fn far(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {return pos(i,0.75);}
@vertex fn nearest(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {return pos(i,0.125);}
@fragment fn red()->@location(0) vec4f {return vec4f(1.,0.,0.,1.);}
@fragment fn green()->@location(0) vec4f {return vec4f(0.,1.,0.,1.);}
@fragment fn blue()->@location(0) vec4f {return vec4f(0.,0.,1.,1.);}
"#).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    for format in [F::Depth32Float, F::Depth24Plus, F::Depth24PlusStencil8] {
        for samples in [1, 4] {
            let size = Extent3d {
                width: 4,
                height: 4,
                depth_or_array_layers: 1,
            };
            let color = gpu
                .create_texture(
                    "depth result",
                    TextureDescriptor {
                        size,
                        mip_levels: 1,
                        sample_count: samples,
                        dimension: D::D2,
                        format: F::Rgba8Unorm,
                        usage: T::COLOR_TARGET
                            | if samples == 1 {
                                T::COPY_SRC
                            } else {
                                T::empty()
                            },
                    },
                )
                .unwrap();
            let view = gpu.texture_view(&color, V::D2, 0..1, 0..1).unwrap();
            let depth = gpu
                .create_texture(
                    "depth stencil",
                    TextureDescriptor {
                        size,
                        mip_levels: 1,
                        sample_count: samples,
                        dimension: D::D2,
                        format,
                        usage: T::DEPTH_STENCIL_WRITE,
                    },
                )
                .unwrap();
            let depthview = gpu.texture_view(&depth, V::D2, 0..1, 0..1).unwrap();
            let resolved = gpu
                .create_texture(
                    "resolve",
                    TextureDescriptor {
                        size,
                        mip_levels: 1,
                        sample_count: 1,
                        dimension: D::D2,
                        format: F::Rgba8Unorm,
                        usage: T::COLOR_TARGET | T::COPY_SRC,
                    },
                )
                .unwrap();
            let resolvedview = gpu.texture_view(&resolved, V::D2, 0..1, 0..1).unwrap();
            let readback = gpu
                .create_buffer("depth readback", 1024, U::MAP_READ | U::COPY_DST)
                .unwrap();
            let stencil = |write| {
                if !format.has_stencil_aspect() {
                    types::StencilState::default()
                } else {
                    types::StencilState {
                        front: types::StencilFaceState {
                            compare: if write {
                                types::CompareFunction::Always
                            } else {
                                types::CompareFunction::Equal
                            },
                            fail_op: types::StencilOperation::Keep,
                            depth_fail_op: types::StencilOperation::Keep,
                            pass_op: if write {
                                types::StencilOperation::Replace
                            } else {
                                types::StencilOperation::Keep
                            },
                        },
                        back: types::StencilFaceState::IGNORE,
                        read_mask: 255,
                        write_mask: if write { 255 } else { 0 },
                    }
                }
            };
            let make = |vs, fs, write| {
                gpu.render_pipeline(&PipelineDescriptor {
                    layout: &layout,
                    vertex: &shader,
                    vertex_entry: vs,
                    fragment: Some((&shader, fs)),
                    colors: &colors,
                    primitive: Default::default(),
                    multisample: types::MultisampleState {
                        count: samples,
                        ..Default::default()
                    },
                    depth_stencil: Some(types::DepthStencilState {
                        format,
                        depth_write_enabled: Some(true),
                        depth_compare: Some(types::CompareFunction::Less),
                        stencil: stencil(write),
                        bias: Default::default(),
                    }),
                })
                .unwrap()
            };
            let red = make("near", "red", true);
            let green = make("far", "green", false);
            let blue = make("nearest", "blue", false);
            let draw = |p, scissor| Draw {
                pipeline: p,
                groups: &[],
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor,
                blend: types::Color::TRANSPARENT,
                stencil_reference: 5,
            };
            let mut encoder = gpu.encoder().unwrap();
            encoder
                .render_full(
                    &[Some(ColorAttachment {
                        view: &view,
                        clear: Some(types::Color::BLACK),
                    })],
                    Some(DepthAttachment {
                        view: &depthview,
                        clear_depth: Some(1.),
                        clear_stencil: format.has_stencil_aspect().then_some(0),
                    }),
                    &[],
                    &[draw(&red, None), draw(&green, None)],
                )
                .unwrap();
            let resolves = if samples == 4 {
                vec![Some(&resolvedview)]
            } else {
                vec![]
            };
            encoder
                .render_full(
                    &[Some(ColorAttachment {
                        view: &view,
                        clear: None,
                    })],
                    Some(DepthAttachment {
                        view: &depthview,
                        clear_depth: None,
                        clear_stencil: None,
                    }),
                    &resolves,
                    &[draw(&blue, Some([0, 0, 2, 4]))],
                )
                .unwrap();
            let source = if samples == 4 { &resolved } else { &color };
            encoder
                .copy_texture_to_buffer(
                    source,
                    0,
                    Origin3d::ZERO,
                    size,
                    &readback,
                    L {
                        offset: 0,
                        bytes_per_row: Some(256),
                        rows_per_image: Some(4),
                    },
                )
                .unwrap();
            gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
            let bytes = gpu.read_buffer(&readback, 0..1024, DEADLINE).unwrap();
            for row in bytes.chunks(256) {
                for x in 0..4 {
                    assert_eq!(
                        &row[x * 4..x * 4 + 4],
                        if x < 2 {
                            &[0, 0, 255, 255]
                        } else {
                            &[255, 0, 0, 255]
                        },
                        "{format:?} x{samples} x{x}"
                    );
                }
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn depth_subresources_initialize_to_zero_and_depth_sampling_is_aspect_checked() {
    use skyrim_native_gpu::{
        bindings::{Entry, Resource},
        render::{BoundGroup, ColorAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader=gpu.shader(r#"
@group(0) @binding(0) var depth:texture_depth_2d;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f {let d=textureLoad(depth,vec2i(0,0),0);return vec4f(d,d,d,1.);}
"#).unwrap();
    let group_layout = gpu
        .group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Texture {
                sample_type: types::TextureSampleType::Depth,
                view_dimension: V::D2,
                multisampled: false,
            },
            count: None,
        }])
        .unwrap();
    let layout = gpu.pipeline_layout(&[group_layout.clone()]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &colors,
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })
        .unwrap();
    let size = Extent3d {
        width: 2,
        height: 2,
        depth_or_array_layers: 1,
    };
    let color = gpu
        .create_texture(
            "sample result",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let cv = gpu.texture_view(&color, V::D2, 0..1, 0..1).unwrap();
    let readback = gpu
        .create_buffer("sample result", 512, U::MAP_READ | U::COPY_DST)
        .unwrap();
    for format in [F::Depth32Float, F::Depth24Plus, F::Depth24PlusStencil8] {
        for mutant in [false, true] {
            gpu.test_texture_initialization(true, mutant);
            let t = gpu
                .create_texture(
                    "all depth mips and layers",
                    TextureDescriptor {
                        size: Extent3d {
                            width: 8,
                            height: 4,
                            depth_or_array_layers: 3,
                        },
                        mip_levels: 3,
                        sample_count: 1,
                        dimension: D::D2,
                        format,
                        usage: T::RESOURCE,
                    },
                )
                .unwrap();
            gpu.test_texture_initialization(false, false);
            for mip in 0..3 {
                for layer in 0..3 {
                    let v = gpu
                        .texture_view_aspect(
                            &t,
                            V::D2,
                            mip..mip + 1,
                            layer..layer + 1,
                            types::TextureAspect::DepthOnly,
                        )
                        .unwrap();
                    let group = gpu
                        .bind_group(
                            &group_layout,
                            &[Entry {
                                binding: 0,
                                resource: Resource::Texture(&v),
                            }],
                        )
                        .unwrap();
                    let groups = [BoundGroup {
                        group: &group,
                        dynamic_offsets: &[],
                    }];
                    let draw = Draw {
                        pipeline: &pipeline,
                        groups: &groups,
                        vertices: 0..3,
                        instances: 0..1,
                        indices: None,
                        viewport: None,
                        scissor: None,
                        blend: types::Color::TRANSPARENT,
                        stencil_reference: 0,
                    };
                    let mut e = gpu.encoder().unwrap();
                    e.render(
                        &[Some(ColorAttachment {
                            view: &cv,
                            clear: Some(types::Color::WHITE),
                        })],
                        &[draw],
                    )
                    .unwrap();
                    e.copy_texture_to_buffer(
                        &color,
                        0,
                        Origin3d::ZERO,
                        size,
                        &readback,
                        L {
                            offset: 0,
                            bytes_per_row: Some(256),
                            rows_per_image: Some(2),
                        },
                    )
                    .unwrap();
                    drop(group);
                    drop(v);
                    gpu.submit(vec![e.finish().unwrap()]).unwrap();
                    let bytes = gpu.read_buffer(&readback, 0..512, DEADLINE).unwrap();
                    for row in bytes.chunks(256) {
                        if mutant && mip == 2 && layer == 2 {
                            assert!(
                                row[0] > 0 && row[4] > 0,
                                "mutant must leave observable poison"
                            );
                        } else {
                            assert_eq!(
                                &row[..8],
                                &[0, 0, 0, 255, 0, 0, 0, 255],
                                "{format:?} mip{mip} layer{layer}"
                            );
                        }
                    }
                }
            }
            if format.has_stencil_aspect() {
                let invalid = gpu.texture_view(&t, V::D2, 0..1, 0..1).unwrap();
                assert!(
                    gpu.bind_group(
                        &group_layout,
                        &[Entry {
                            binding: 0,
                            resource: Resource::Texture(&invalid)
                        }]
                    )
                    .is_err()
                );
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn alpha_coverage_endpoints_and_sample_masks_resolve_exactly() {
    use skyrim_native_gpu::{
        render::{ColorAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = gpu
        .shader(
            r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn zero()->@location(0) vec4f {return vec4f(1.,0.,0.,0.);}
@fragment fn one()->@location(0) vec4f {return vec4f(1.,0.,0.,1.);}
@fragment fn partial()->@location(0) vec4f {return vec4f(0.8,0.,0.,1.);}
"#,
        )
        .unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    let size = Extent3d {
        width: 2,
        height: 2,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "MSAA4",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 4,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let resolved = gpu
        .create_texture(
            "resolved",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let rv = gpu.texture_view(&resolved, V::D2, 0..1, 0..1).unwrap();
    let readback = gpu
        .create_buffer("MSAA result", 512, U::MAP_READ | U::COPY_DST)
        .unwrap();
    for (entry, mask, a2c, expected) in [
        ("zero", !0, true, [0, 0, 0, 255]),
        ("one", !0, true, [255, 0, 0, 255]),
        ("one", 0, false, [0, 0, 0, 255]),
        ("partial", 3, false, [102, 0, 0, 255]),
    ] {
        let result_pipeline = gpu.render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, entry)),
            colors: &colors,
            primitive: Default::default(),
            depth_stencil: None,
            multisample: types::MultisampleState {
                count: 4,
                mask,
                alpha_to_coverage_enabled: a2c,
            },
        });
        if !<Platform as skyrim_native_gpu::backend::Backend>::STATIC_SAMPLE_MASK
            && mask != !0
            && !cfg!(feature = "metal-fixed-sample-mask")
        {
            assert!(result_pipeline.is_err());
            continue;
        }
        let p = result_pipeline.unwrap();
        let draw = Draw {
            pipeline: &p,
            groups: &[],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: types::Color::TRANSPARENT,
            stencil_reference: 0,
        };
        let mut e = gpu.encoder().unwrap();
        e.render_full(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color::BLACK),
            })],
            None,
            &[Some(&rv)],
            &[draw],
        )
        .unwrap();
        e.copy_texture_to_buffer(
            &resolved,
            0,
            Origin3d::ZERO,
            size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(2),
            },
        )
        .unwrap();
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        let bytes = gpu.read_buffer(&readback, 0..512, DEADLINE).unwrap();
        for row in bytes.chunks(256) {
            for x in 0..2 {
                assert_eq!(
                    &row[x * 4..x * 4 + 4],
                    &expected,
                    "{entry} mask{mask} A2C{a2c}"
                );
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn production_xenos_translator_and_binding_abi_draw_through_native_backend() {
    use skyrim_native_gpu::{
        bindings::{Entry, Resource},
        render::{BoundGroup, ColorAttachment, Draw},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    use std::num::NonZeroU64;
    use xenos_shader::{Shader, ShaderStage, TranslationOptions, bindings};
    let (_lock, before) = setup();
    let mut gpu = open();
    // Identical hand-authored instruction fixtures in public frame_probe.rs:
    // fetch position, export position/color, copy interpolated color. No game's
    // shader, shader dump, or microcode extraction is used in this test.
    const VS: [u32; 12] = [
        0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100,
        0xC80F8000, 0, 0x22000000,
    ];
    const PS: [u32; 6] = [0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000];
    let options = TranslationOptions {
        vs_output_interpolators: 1,
        ps_interpolator_count: Some(1),
        ..Default::default()
    };
    let vs = Shader::new(ShaderStage::Vertex, &VS)
        .unwrap()
        .translate(&options);
    let ps = Shader::new(ShaderStage::Pixel, &PS)
        .unwrap()
        .translate(&options);
    assert!(vs.warnings.is_empty() && ps.warnings.is_empty());
    let vertex = gpu.shader(&vs.wgsl).unwrap();
    let fragment = gpu.shader(&ps.wgsl).unwrap();
    let both = types::ShaderStages::VERTEX | types::ShaderStages::FRAGMENT;
    let sizes = [4096, 160, 768, 112, 4096];
    let entries: Vec<_> = sizes
        .iter()
        .enumerate()
        .map(|(binding, &size)| types::BindGroupLayoutEntry {
            binding: binding as u32,
            visibility: match binding {
                0 => types::ShaderStages::VERTEX,
                4 => types::ShaderStages::FRAGMENT,
                _ => both,
            },
            ty: types::BindingType::Buffer {
                ty: if binding == 0 || binding == 4 {
                    types::BufferBindingType::Storage { read_only: true }
                } else {
                    types::BufferBindingType::Uniform
                },
                has_dynamic_offset: true,
                min_binding_size: NonZeroU64::new(size),
            },
            count: None,
        })
        .collect();
    let constants_layout = gpu.group_layout(&entries).unwrap();
    let shared_layout = gpu
        .group_layout(
            &(0..5)
                .map(|binding| types::BindGroupLayoutEntry {
                    binding,
                    visibility: both,
                    ty: types::BindingType::Buffer {
                        ty: types::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: NonZeroU64::new(4),
                    },
                    count: None,
                })
                .collect::<Vec<_>>(),
        )
        .unwrap();
    let empty = gpu.group_layout(&[]).unwrap();
    let layout = gpu
        .pipeline_layout(&[
            constants_layout.clone(),
            shared_layout.clone(),
            empty.clone(),
        ])
        .unwrap();
    let mut constants = Vec::new();
    for (binding, &size) in sizes.iter().enumerate() {
        let usage = if binding == 0 || binding == 4 {
            U::STORAGE_READ_ONLY
        } else {
            U::UNIFORM
        };
        let buffer = gpu
            .create_buffer("xenos constant ABI", size, usage | U::COPY_DST)
            .unwrap();
        let mut data = vec![0u8; size as usize];
        match binding {
            0 => {
                for (i, v) in [0.0f32, 1., 0., 1.].into_iter().enumerate() {
                    data[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
                }
            }
            2 => {
                for (i, v) in [0x10000u32 | 3, 2 | 9 * 4 << 2].into_iter().enumerate() {
                    data[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
                }
            }
            3 => data.copy_from_slice(bindings::SystemConstants::default().as_bytes()),
            _ => {}
        }
        gpu.write_buffer(&buffer, 0, &data).unwrap();
        constants.push(buffer);
    }
    let constants_group = gpu
        .bind_group(
            &constants_layout,
            &constants
                .iter()
                .enumerate()
                .map(|(i, b)| Entry {
                    binding: i as u32,
                    resource: Resource::Buffer {
                        buffer: b,
                        offset: 0,
                        size: b.size(),
                    },
                })
                .collect::<Vec<_>>(),
        )
        .unwrap();
    let mut shared = Vec::new();
    for slot in 0..5 {
        let size = if slot == 0 {
            65536
        } else if slot == 4 {
            u64::from(bindings::SHARED_MEMORY_BLOCKS) * 4
        } else {
            4
        };
        let buffer = gpu
            .create_buffer(
                "xenos sparse shared ABI",
                size,
                U::STORAGE_READ_ONLY | U::COPY_DST,
            )
            .unwrap();
        if slot == 0 {
            let bytes: Vec<_> = [[-1.0f32, -1., 0.5], [3., -1., 0.5], [-1., 3., 0.5]]
                .into_iter()
                .flatten()
                .flat_map(f32::to_be_bytes)
                .collect();
            gpu.write_buffer(&buffer, 0, &bytes).unwrap();
        } else if slot == 4 {
            gpu.write_buffer(&buffer, 4, &1u32.to_le_bytes()).unwrap();
        }
        shared.push(buffer);
    }
    let shared_group = gpu
        .bind_group(
            &shared_layout,
            &shared
                .iter()
                .enumerate()
                .map(|(i, b)| Entry {
                    binding: i as u32,
                    resource: Resource::Buffer {
                        buffer: b,
                        offset: 0,
                        size: b.size(),
                    },
                })
                .collect::<Vec<_>>(),
        )
        .unwrap();
    let empty_group = gpu.bind_group(&empty, &[]).unwrap();
    let key = pipeline_cases::normalized_key();
    let mut cache = skyrim_native_gpu::pipeline::PipelineCache::new(8).unwrap();
    let pipeline = cache
        .get_or_create(
            &gpu,
            &key,
            &layout,
            &vertex,
            vs.entry_point,
            &fragment,
            ps.entry_point,
        )
        .unwrap();
    let _same = cache
        .get_or_create(
            &gpu,
            &key,
            &layout,
            &vertex,
            vs.entry_point,
            &fragment,
            ps.entry_point,
        )
        .unwrap();
    assert_eq!((cache.stats().created, cache.stats().hits), (1, 1));
    let size = Extent3d {
        width: 32,
        height: 32,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "xenos native result",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let readback = gpu
        .create_buffer("xenos result", 256 * 32, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let groups = [
        BoundGroup {
            group: &constants_group,
            dynamic_offsets: &[0; 5],
        },
        BoundGroup {
            group: &shared_group,
            dynamic_offsets: &[],
        },
        BoundGroup {
            group: &empty_group,
            dynamic_offsets: &[],
        },
    ];
    let draw = || Draw {
        pipeline: &pipeline,
        groups: &groups,
        vertices: 0..3,
        instances: 0..1,
        indices: None,
        viewport: None,
        scissor: None,
        blend: types::Color::TRANSPARENT,
        stencil_reference: 0,
    };
    let draws: Vec<_> = (0..10000).map(|_| draw()).collect();
    let mut encoder = gpu.encoder().unwrap();
    let stats = encoder
        .render(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color::BLACK),
            })],
            &draws,
        )
        .unwrap();
    assert_eq!(
        (
            stats.draws,
            stats.unique_groups,
            stats.pipeline_binds,
            stats.group_binds
        ),
        (10000, 3, 1, 3)
    );
    encoder
        .copy_texture_to_buffer(
            &target,
            0,
            Origin3d::ZERO,
            size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(32),
            },
        )
        .unwrap();
    gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
    let bytes = gpu
        .read_buffer(&readback, 0..readback.size(), DEADLINE)
        .unwrap();
    for row in bytes.chunks(256) {
        for pixel in row[..128].chunks(4) {
            assert_eq!(pixel, &[0, 255, 0, 255]);
        }
    }
    // Exercise the exact public Executor packet/dedup protocol against the
    // native recorder, rather than constructing only native Draw objects.
    use skyrim_native_gpu::{render::NativeTypes, sink};
    let index = gpu
        .create_buffer("unused packet index rings", 4, U::INDEX)
        .unwrap();
    let packets: Vec<_> = (0..10000)
        .map(|_| sink::DrawPacket::<NativeTypes<Platform>> {
            pipeline: pipeline.clone(),
            textures: empty_group.clone(),
            constant_offsets: [0; 5],
            state: sink::DrawState::new([0., 0., 32., 32., 0., 1.], [0, 0, 32, 32], [0.; 4], 0),
            indices: None,
            count: 3,
        })
        .collect();
    let resources = sink::Resources {
        constants: &constants_group,
        shared: &shared_group,
        indices: &index,
        gathered: &index,
        shared_buffers: &shared,
    };
    let attachments = [Some(ColorAttachment {
        view: &view,
        clear: Some(types::Color {
            r: 1.,
            g: 0.,
            b: 1.,
            a: 1.,
        }),
    })];
    let mut record = gpu.render_recorder(&attachments, None, &[]).unwrap();
    sink::emit(&packets, &resources, Default::default(), &mut record).unwrap();
    let storage = record.storage();
    let mut encoder = gpu.encoder().unwrap();
    let stats = encoder.render_recorded(&record).unwrap();
    assert_eq!(
        (
            stats.draws,
            stats.unique_groups,
            stats.pipeline_binds,
            stats.group_binds
        ),
        (10000, 3, 1, 3)
    );
    // Reset immediately, while the encoder still references the previous pass.
    record.reset(&attachments, None, &[]).unwrap();
    sink::emit(&packets, &resources, Default::default(), &mut record).unwrap();
    assert_eq!(record.storage(), storage);
    drop(record);
    drop(packets);
    encoder
        .copy_texture_to_buffer(
            &target,
            0,
            Origin3d::ZERO,
            size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(32),
            },
        )
        .unwrap();
    gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
    let actual = gpu
        .read_buffer(&readback, 0..readback.size(), DEADLINE)
        .unwrap();
    assert_eq!(actual, bytes);
    drop(gpu);
    finish(before);
}

#[test]
fn shader_sample_mask_inputs_and_struct_outputs_remain_valid_and_keep_exact_coverage() {
    use skyrim_native_gpu::{
        render::{ColorAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = gpu
        .shader(
            r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
struct Out { @location(0) color:vec4f, @builtin(sample_mask) mask:u32 }
struct In { @builtin(sample_mask) mask:u32 }
@fragment fn direct(@builtin(sample_mask) mask:u32)->Out {return Out(vec4f(0.,0.8,0.,1.),mask&3u);}
@fragment fn structured(input:In)->Out {return Out(vec4f(0.,0.8,0.,1.),input.mask&3u);}
"#,
        )
        .unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    let size = Extent3d {
        width: 2,
        height: 2,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "shader mask",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 4,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let result = gpu
        .create_texture(
            "shader mask resolve",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let rv = gpu.texture_view(&result, V::D2, 0..1, 0..1).unwrap();
    let readback = gpu
        .create_buffer("mask result", 512, U::MAP_READ | U::COPY_DST)
        .unwrap();
    for entry in ["direct", "structured"] {
        let p = gpu
            .render_pipeline(&PipelineDescriptor {
                layout: &layout,
                vertex: &shader,
                vertex_entry: "vs",
                fragment: Some((&shader, entry)),
                colors: &colors,
                primitive: Default::default(),
                depth_stencil: None,
                multisample: types::MultisampleState {
                    count: 4,
                    ..Default::default()
                },
            })
            .unwrap();
        let draw = Draw {
            pipeline: &p,
            groups: &[],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: types::Color::TRANSPARENT,
            stencil_reference: 0,
        };
        let mut e = gpu.encoder().unwrap();
        e.render_full(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color::BLACK),
            })],
            None,
            &[Some(&rv)],
            &[draw],
        )
        .unwrap();
        e.copy_texture_to_buffer(
            &result,
            0,
            Origin3d::ZERO,
            size,
            &readback,
            L {
                offset: 0,
                bytes_per_row: Some(256),
                rows_per_image: Some(2),
            },
        )
        .unwrap();
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        let bytes = gpu.read_buffer(&readback, 0..512, DEADLINE).unwrap();
        for row in bytes.chunks(256) {
            for x in 0..2 {
                assert_eq!(&row[x * 4..x * 4 + 4], &[0, 102, 0, 255], "{entry}");
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn multisample_and_stencil_initialization_replace_poison_and_detect_skipped_clear() {
    use skyrim_native_gpu::{
        render::{ColorAttachment, DepthAttachment, Draw, PipelineDescriptor},
        texture::TextureDescriptor,
        types::{
            self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
            TextureFormat as F, TextureUses as T, TextureViewDimension as V,
        },
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    let size = Extent3d {
        width: 2,
        height: 2,
        depth_or_array_layers: 1,
    };
    let result = gpu
        .create_texture(
            "observable init",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let rv = gpu.texture_view(&result, V::D2, 0..1, 0..1).unwrap();
    let readback = gpu
        .create_buffer("init proof", 512, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let shader=gpu.shader(r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f{return vec4f(1.,0.,0.,1.);}
"#).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    let face = types::StencilFaceState {
        compare: types::CompareFunction::Equal,
        fail_op: types::StencilOperation::Keep,
        depth_fail_op: types::StencilOperation::Keep,
        pass_op: types::StencilOperation::Keep,
    };
    let p = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &colors,
            primitive: Default::default(),
            multisample: Default::default(),
            depth_stencil: Some(types::DepthStencilState {
                format: F::Depth24PlusStencil8,
                depth_write_enabled: Some(false),
                depth_compare: Some(types::CompareFunction::Always),
                stencil: types::StencilState {
                    front: face,
                    back: face,
                    read_mask: 255,
                    write_mask: 0,
                },
                bias: Default::default(),
            }),
        })
        .unwrap();
    for stencil in [false, true] {
        for mutant in [false, true] {
            gpu.test_texture_initialization(true, mutant);
            let target = gpu
                .create_texture(
                    "poison then real initialization",
                    TextureDescriptor {
                        size,
                        mip_levels: 1,
                        sample_count: if stencil { 1 } else { 4 },
                        dimension: D::D2,
                        format: if stencil {
                            F::Depth24PlusStencil8
                        } else {
                            F::Rgba8Unorm
                        },
                        usage: if stencil {
                            T::DEPTH_STENCIL_WRITE
                        } else {
                            T::COLOR_TARGET
                        },
                    },
                )
                .unwrap();
            gpu.test_texture_initialization(false, false);
            let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
            let mut e = gpu.encoder().unwrap();
            if stencil {
                let draw = Draw {
                    pipeline: &p,
                    groups: &[],
                    vertices: 0..3,
                    instances: 0..1,
                    indices: None,
                    viewport: None,
                    scissor: None,
                    blend: types::Color::TRANSPARENT,
                    stencil_reference: 0,
                };
                e.render_full(
                    &[Some(ColorAttachment {
                        view: &rv,
                        clear: Some(types::Color::BLACK),
                    })],
                    Some(DepthAttachment {
                        view: &view,
                        clear_depth: None,
                        clear_stencil: None,
                    }),
                    &[],
                    &[draw],
                )
                .unwrap();
            } else {
                // No caller clear and no draws: only the initialization is observed.
                e.render_full(
                    &[Some(ColorAttachment {
                        view: &view,
                        clear: None,
                    })],
                    None,
                    &[Some(&rv)],
                    &[],
                )
                .unwrap();
            }
            e.copy_texture_to_buffer(
                &result,
                0,
                Origin3d::ZERO,
                size,
                &readback,
                L {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(2),
                },
            )
            .unwrap();
            gpu.submit(vec![e.finish().unwrap()]).unwrap();
            let bytes = gpu.read_buffer(&readback, 0..512, DEADLINE).unwrap();
            let expected = match (stencil, mutant) {
                (false, false) => [0, 0, 0, 0],
                (false, true) => [255, 0, 255, 255],
                (true, false) => [255, 0, 0, 255],
                (true, true) => [0, 0, 0, 255],
            };
            for row in bytes.chunks(256) {
                for x in 0..2 {
                    assert_eq!(
                        &row[x * 4..x * 4 + 4],
                        &expected,
                        "stencil{stencil} mutant{mutant}"
                    );
                }
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[path = "cases/recorder.rs"]
mod recorder_cases;

#[path = "cases/staging.rs"]
mod staging_cases;

#[path = "cases/pipeline.rs"]
mod pipeline_cases;

#[path = "cases/depth_only.rs"]
mod depth_only_cases;

#[path = "cases/compute.rs"]
mod shared_compute_cases;

#[path = "cases/parity.rs"]
mod parity_cases;

#[path = "cases/edram.rs"]
mod edram_cases;

#[path = "cases/compressed.rs"]
mod compressed_cases;

#[path = "cases/edram_transfer.rs"]
mod edram_transfer_cases;

#[path = "cases/target_cache.rs"]
mod target_cache_cases;

#[path = "cases/presentation.rs"]
mod presentation_cases;

#[path = "cases/target_scope.rs"]
mod target_scope_cases;

#[path = "cases/sample_lowering.rs"]
mod sample_lowering_cases;

#[path = "cases/resolve_cache.rs"]
mod resolve_cache_cases;

#[path = "cases/color_outputs.rs"]
mod color_output_cases;

#[path = "cases/compute_batch.rs"]
mod compute_batch_cases;

#[path = "cases/store_kernel.rs"]
mod store_kernel_cases;

#[path = "cases/ordered_upload.rs"]
mod ordered_upload_cases;

#[path = "cases/constant_rollover.rs"]
mod constant_rollover_cases;

#[path = "cases/depth_codec.rs"]
mod depth_codec_cases;

#[path = "cases/reservation.rs"]
mod reservation_cases;

#[path = "cases/constant_frame.rs"]
mod constant_frame_cases;

#[path = "cases/guest_frame.rs"]
mod guest_frame_cases;

#[path = "cases/guest_memory.rs"]
mod guest_memory_cases;

#[path = "cases/guest_store.rs"]
mod guest_store_cases;
#[path = "cases/guest_texture.rs"]
mod guest_texture_cases;

#[path = "cases/guest_clear.rs"]
mod guest_clear_cases;

#[path = "cases/guest_journal.rs"]
mod guest_journal_cases;

#[path = "cases/session.rs"]
mod session_cases;

#[path="cases/deferred_bindings.rs"]
mod deferred_binding_cases;
