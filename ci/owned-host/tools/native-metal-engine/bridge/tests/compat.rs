//! Authored compatibility cases at the live renderer/backend boundary.
use gpu_native_bridge::*;

fn device() -> Device {
    let instance = Instance::new().unwrap();
    let adapter = instance
        .block_on(instance.request_adapter(&RequestAdapterOptions::default()))
        .unwrap();
    instance
        .block_on(adapter.request_device(&DeviceDescriptor::default()))
        .unwrap()
}
fn buffer(d: &Device, size: u64, usage: BufferUsage, mapped: bool) -> Buffer {
    d.create_buffer(&BufferDescriptor {
        label: "boundary oracle",
        size,
        usage,
        mapped_at_creation: mapped,
    })
}
fn read(d: &Device, b: &Buffer) -> Vec<u8> {
    d.block_on(b.map_async(MapMode::Read, 0, None)).unwrap();
    let mut bytes = vec![0; b.size() as usize];
    b.read_mapped(0, &mut bytes).unwrap();
    b.unmap();
    assert_eq!(d.error_count(), 0, "{:?}", d.take_errors());
    bytes
}

#[test]
fn mapped_staging_may_be_recorded_before_unmap_and_preserves_partial_updates() {
    let d = device();
    let src = buffer(&d, 16, BufferUsage::MapWrite | BufferUsage::CopySrc, true);
    let dst = buffer(&d, 16, BufferUsage::MapRead | BufferUsage::CopyDst, false);
    src.write_mapped(0, &[7; 16]).unwrap();
    let mut e = d.create_command_encoder("record before unmap");
    e.copy_buffer_to_buffer(&src, 0, &dst, 0, 16);
    let commands = e.finish("record before unmap");
    src.unmap();
    d.queue().submit([commands]);
    assert_eq!(read(&d, &dst), [7; 16]);
    d.block_on(src.map_async(MapMode::Write, 8, Some(8)))
        .unwrap();
    let mut before = [0; 8];
    src.read_mapped(8, &mut before).unwrap();
    assert_eq!(before, [7; 8]);
    src.write_mapped(8, &[9; 4]).unwrap();
    src.unmap();
    let mut e = d.create_command_encoder("preserved partial map");
    e.copy_buffer_to_buffer(&src, 0, &dst, 0, 16);
    d.queue().submit([e.finish("preserved partial map")]);
    assert_eq!(
        read(&d, &dst),
        [7, 7, 7, 7, 7, 7, 7, 7, 9, 9, 9, 9, 7, 7, 7, 7]
    );
}

#[test]
fn abandoned_mapping_does_not_strand_buffer_or_cancel_next_mapping() {
    let d = device();
    let b = buffer(&d, 16, BufferUsage::MapWrite | BufferUsage::CopySrc, false);
    drop(b.map_async(MapMode::Write, 0, None));
    d.block_on(b.map_async(MapMode::Write, 0, None)).unwrap();
    b.write_mapped(0, &[3; 16]).unwrap();
    b.unmap();
    d.block_on(b.map_async(MapMode::Write, 0, None)).unwrap();
    let mut bytes = [0; 16];
    b.read_mapped(0, &mut bytes).unwrap();
    assert_eq!(bytes, [3; 16]);
    b.unmap();
    assert_eq!(d.error_count(), 0);
}

#[test]
fn bounded_queue_uploads_keep_prefix_order_before_already_recorded_commands() {
    let d = device();
    let size = 36 << 20;
    let src = buffer(&d, size, BufferUsage::CopySrc | BufferUsage::CopyDst, false);
    let dst = buffer(&d, 16, BufferUsage::MapRead | BufferUsage::CopyDst, false);
    let mut e = d.create_command_encoder("prefix order");
    e.copy_buffer_to_buffer(&src, 0, &dst, 0, 8);
    e.copy_buffer_to_buffer(&src, size - 8, &dst, 8, 8);
    d.queue().write_buffer(&src, 0, &vec![17; size as usize]);
    d.queue().write_buffer(&src, 0, &[23; 8]);
    d.queue().submit([e.finish("prefix order")]);
    assert_eq!(
        read(&d, &dst),
        [
            23, 23, 23, 23, 23, 23, 23, 23, 17, 17, 17, 17, 17, 17, 17, 17
        ]
    );
}

#[test]
fn sequential_texture_uploads_and_omitted_single_image_rows_preserve_latest_bytes() {
    let d = device();
    let t = d.create_texture(&TextureDescriptor {
        size: extent(2, 2, 1),
        format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::CopySrc | TextureUsage::CopyDst,
        ..Default::default()
    });
    let dst = buffer(&d, 512, BufferUsage::MapRead | BufferUsage::CopyDst, false);
    let mut e = d.create_command_encoder("texture prefix");
    e.copy_texture_to_buffer(
        &TexelCopyTextureInfo::whole(&t),
        &TexelCopyBufferInfo {
            buffer: &dst,
            offset: 0,
            bytes_per_row: 256,
            rows_per_image: None,
        },
        extent(2, 2, 1),
    );
    d.queue().write_texture(
        &TexelCopyTextureInfo::whole(&t),
        &[41; 16],
        8,
        None,
        extent(2, 2, 1),
    );
    d.queue().write_texture(
        &TexelCopyTextureInfo::whole(&t),
        &[67; 16],
        8,
        None,
        extent(2, 2, 1),
    );
    d.queue().submit([e.finish("texture prefix")]);
    let actual = read(&d, &dst);
    assert_eq!(&actual[..8], &[67; 8]);
    assert_eq!(&actual[256..264], &[67; 8]);
}

#[test]
fn mapped_or_destroyed_after_recording_is_rejected_before_submission() {
    for destroy in [false, true] {
        let d = device();
        let src = buffer(&d, 16, BufferUsage::MapWrite | BufferUsage::CopySrc, false);
        let dst = buffer(&d, 16, BufferUsage::MapRead | BufferUsage::CopyDst, false);
        let mut e = d.create_command_encoder("late resource state");
        e.copy_buffer_to_buffer(&src, 0, &dst, 0, 16);
        let commands = e.finish("late resource state");
        if destroy {
            src.destroy();
        } else {
            d.block_on(src.map_async(MapMode::Write, 0, None)).unwrap();
        }
        d.queue().submit([commands]);
        assert!(d.lost_reason().is_some());
        assert_eq!(d.error_count(), 1);
    }
}

#[test]
fn two_devices_are_isolated_and_optional_features_are_not_implicitly_enabled() {
    let instance = Instance::new().unwrap();
    let adapter = instance
        .block_on(instance.request_adapter(&RequestAdapterOptions::default()))
        .unwrap();
    let a = instance
        .block_on(adapter.request_device(&DeviceDescriptor::default()))
        .unwrap();
    let b = instance
        .block_on(adapter.request_device(&DeviceDescriptor::default()))
        .unwrap();
    assert!(a.features().is_empty());
    assert!(b.features().is_empty());
    let src = buffer(&a, 16, BufferUsage::CopySrc | BufferUsage::CopyDst, false);
    b.queue().write_buffer(&src, 0, &[4; 16]);
    assert!(b.lost_reason().is_some());
    assert!(a.lost_reason().is_none());
}

#[test]
fn resource_ids_survive_allocator_reuse_without_cache_aliasing() {
    let d = device();
    let mut identities = std::collections::HashSet::new();
    for i in 0..256 {
        let s = d.create_sampler(&SamplerDescriptor {
            max_anisotropy: 1,
            ..Default::default()
        });
        assert!(identities.insert(s.identity()));
        drop(s);
        let b = buffer(&d, 16, BufferUsage::CopyDst, false);
        assert!(identities.insert(b.identity()));
        drop(b);
        if i % 16 == 15 {
            d.queue().submit([]);
        }
    }
    d.block_on(d.queue().on_submitted_work_done()).unwrap();
    assert_eq!(d.error_count(), 0);
}

#[test]
fn invalid_layout_and_device_loss_do_not_panic_or_flood_errors() {
    let d = device();
    let t = d.create_texture(&TextureDescriptor {
        size: extent(1, 1, 2),
        format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::CopyDst,
        ..Default::default()
    });
    d.queue().write_texture(
        &TexelCopyTextureInfo::whole(&t),
        &[],
        u32::MAX - 3,
        Some(u32::MAX),
        extent(1, 1, u32::MAX),
    );
    assert!(d.lost_reason().is_some());
    assert_eq!(d.take_errors().len(), 1);
    for _ in 0..1000 {
        drop(buffer(&d, 16, BufferUsage::CopyDst, false));
    }
    assert_eq!(d.error_count(), 1);
    assert!(d.take_errors().is_empty());
    let d = device();
    d.push_error_scope(ErrorFilter::Validation);
    drop(buffer(&d, 3, BufferUsage::CopySrc, true));
    assert!(d.block_on(d.pop_error_scope()).unwrap().is_some());
    assert!(d.lost_reason().is_none());
}

#[test]
fn staging_ring_fallback_preserves_the_last_write_and_remaps_without_stale_bytes() {
    let d = device();
    staging::enable(&d, 2, 16);
    let src = buffer(&d, 32, BufferUsage::CopySrc | BufferUsage::CopyDst, false);
    let dst = buffer(&d, 32, BufferUsage::MapRead | BufferUsage::CopyDst, false);
    d.queue().write_buffer(&src, 0, &[4; 8]);
    d.queue().write_buffer(&src, 0, &[8; 32]); // larger than a ring slot: flush previous staged copy
    d.queue().write_buffer(&src, 8, &[12; 8]);
    let mut e = d.create_command_encoder("mixed staging");
    e.copy_buffer_to_buffer(&src, 0, &dst, 0, 32);
    d.queue().submit([e.finish("mixed staging")]);
    let mut expected = [8; 32];
    expected[8..16].fill(12);
    assert_eq!(read(&d, &dst), expected);
}
