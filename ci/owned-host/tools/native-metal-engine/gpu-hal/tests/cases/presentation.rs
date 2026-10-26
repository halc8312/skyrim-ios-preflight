use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    Gpu,
    backend::Platform,
    presentation::{Front, Options},
    texture::{TextureDescriptor, TextureView},
    types::{
        Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D, TextureFormat as F,
        TextureUses as T, TextureViewDimension as V,
    },
};
fn image(gpu: &mut Gpu<Platform>, width: u32, height: u32) -> TextureView<Platform> {
    let texture = gpu
        .create_texture(
            "presentation fixture",
            TextureDescriptor {
                size: Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COPY_SRC | T::COPY_DST | T::RESOURCE | T::COLOR_TARGET,
            },
        )
        .unwrap();
    gpu.texture_view(&texture, V::D2, 0..1, 0..1).unwrap()
}
fn upload(gpu: &mut Gpu<Platform>, source: &TextureView<Platform>, pixels: &[[u8; 4]]) {
    let d = source.texture().descriptor();
    assert_eq!(pixels.len(), (d.size.width * d.size.height) as usize);
    let pitch = (d.size.width * 4).div_ceil(256) * 256;
    let mut bytes = vec![0; (pitch * d.size.height) as usize];
    for y in 0..d.size.height {
        for x in 0..d.size.width {
            let offset = (y * pitch + x * 4) as usize;
            bytes[offset..offset + 4].copy_from_slice(&pixels[(y * d.size.width + x) as usize]);
        }
    }
    let buffer = gpu
        .create_buffer(
            "presentation upload",
            bytes.len() as u64,
            U::COPY_SRC | U::COPY_DST,
        )
        .unwrap();
    gpu.write_buffer(&buffer, 0, &bytes).unwrap();
    let mut e = gpu.encoder().unwrap();
    e.copy_buffer_to_texture(
        &buffer,
        L {
            offset: 0,
            bytes_per_row: Some(pitch),
            rows_per_image: Some(d.size.height),
        },
        source.texture(),
        0,
        Origin3d::ZERO,
        d.size,
    )
    .unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
}
fn read(gpu: &mut Gpu<Platform>, source: &TextureView<Platform>) -> Vec<[u8; 4]> {
    let size = source.texture().descriptor().size;
    let pitch = (size.width * 4).div_ceil(256) * 256;
    let bytes = u64::from(pitch * size.height);
    let out = gpu
        .create_buffer("presentation readback", bytes, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    e.copy_texture_to_buffer(
        source.texture(),
        0,
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
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    gpu.read_buffer(&out, 0..bytes, DEADLINE)
        .unwrap()
        .chunks(pitch as usize)
        .flat_map(|row| {
            row[..size.width as usize * 4]
                .chunks(4)
                .map(|p| p.try_into().unwrap())
        })
        .collect()
}
#[test]
fn native_present_uses_shared_filters_letterbox_and_immutable_parameter_generations() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let source = image(&mut gpu, 4, 2);
    let pixels: Vec<_> = (0..2)
        .flat_map(|y| (0..4).map(move |x| [x * 64, y * 128, 64, 19]))
        .collect();
    upload(&mut gpu, &source, &pixels);
    let a = image(&mut gpu, 8, 8);
    let b = image(&mut gpu, 8, 8);
    let c = image(&mut gpu, 8, 8);
    let mut presenter = gpu.presenter(F::Rgba8Unorm).unwrap();
    let front = Front {
        view: source.clone(),
        display_size: [4, 2],
        swizzle: [2, 1, 0, 3],
    };
    presenter
        .swap(&mut gpu, Some(front.clone()), Options::new(2, 0).unwrap())
        .unwrap();
    let counts = (
        gpu.native_counters().bind_groups.read(),
        gpu.native_counters().buffers.read(),
    );
    for _ in 0..100 {
        presenter
            .swap(&mut gpu, Some(front.clone()), Options::new(2, 0).unwrap())
            .unwrap();
    }
    assert_eq!(
        (
            gpu.native_counters().bind_groups.read(),
            gpu.native_counters().buffers.read()
        ),
        counts
    );
    let mut e = gpu.encoder().unwrap();
    presenter.encode(&mut e, &a).unwrap();
    presenter
        .swap(
            &mut gpu,
            Some(Front {
                swizzle: [4, 5, 0, 3],
                ..front.clone()
            }),
            Options::new(2, 0).unwrap(),
        )
        .unwrap();
    presenter.encode(&mut e, &b).unwrap();
    // No new swap image: keep the front but accept an output-option change.
    assert_eq!(
        presenter
            .swap(&mut gpu, None, Options::new(1, 0).unwrap())
            .unwrap(),
        Some([4, 2])
    );
    presenter.encode(&mut e, &c).unwrap();
    drop(presenter);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let actual_a = read(&mut gpu, &a);
    let actual_b = read(&mut gpu, &b);
    for y in 0..8 {
        for x in 0..8 {
            let active = (2..6).contains(&y);
            let p = if active {
                pixels[((y - 2) / 2 * 4 + x / 2) as usize]
            } else {
                [0; 4]
            };
            let i = (y * 8 + x) as usize;
            assert_eq!(
                actual_a[i],
                if active {
                    [p[2], p[1], p[0], 255]
                } else {
                    [0, 0, 0, 255]
                }
            );
            assert_eq!(
                actual_b[i],
                if active {
                    [0, 255, p[0], 255]
                } else {
                    [0, 0, 0, 255]
                }
            );
        }
    }
    let actual_c = read(&mut gpu, &c);
    assert_ne!(
        actual_b, actual_c,
        "bilinear option must change the interpolated gradient"
    );
    for p in actual_c
        .iter()
        .enumerate()
        .filter(|(i, _)| (2..6).contains(&(i / 8)))
        .map(|(_, p)| p)
    {
        assert_eq!(&p[..2], &[0, 255]);
        assert_eq!(p[3], 255);
    }
    drop(gpu);
    finish(before);
}
#[test]
fn all_native_present_filters_preserve_constant_pixels_and_reject_feedback_before_clear() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let source = image(&mut gpu, 3, 2);
    upload(&mut gpu, &source, &[[255, 0, 255, 0]; 6]);
    let target = image(&mut gpu, 6, 4);
    let mut presenter = gpu.presenter(F::Rgba8Unorm).unwrap();
    for filter in 0..4 {
        for sharpening in [0, 100] {
            presenter
                .swap(
                    &mut gpu,
                    Some(Front {
                        view: source.clone(),
                        display_size: [3, 2],
                        swizzle: [0, 1, 2, 3],
                    }),
                    Options::new(filter, sharpening).unwrap(),
                )
                .unwrap();
            let mut e = gpu.encoder().unwrap();
            presenter.encode(&mut e, &target).unwrap();
            gpu.submit(vec![e.finish().unwrap()]).unwrap();
            assert!(
                read(&mut gpu, &target)
                    .iter()
                    .all(|p| p == &[255, 0, 255, 255]),
                "filter{filter} sharp{sharpening}"
            );
        }
    }
    assert!(Options::new(4, 0).is_err());
    assert!(Options::new(0, 101).is_err());
    assert!(
        presenter
            .swap(
                &mut gpu,
                Some(Front {
                    view: source.clone(),
                    display_size: [0, 2],
                    swizzle: [0, 1, 2, 3]
                }),
                Options::default()
            )
            .is_err()
    );
    presenter
        .swap(
            &mut gpu,
            Some(Front {
                view: target.clone(),
                display_size: [6, 4],
                swizzle: [0, 1, 2, 3],
            }),
            Options::default(),
        )
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    assert!(presenter.encode(&mut e, &target).is_err());
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert!(
        read(&mut gpu, &target)
            .iter()
            .all(|p| p == &[255, 0, 255, 255])
    );
    let blank = image(&mut gpu, 2, 2);
    let empty = gpu.presenter(F::Rgba8Unorm).unwrap();
    let mut e = gpu.encoder().unwrap();
    assert_eq!(empty.encode(&mut e, &blank).unwrap().draws, 0);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert!(read(&mut gpu, &blank).iter().all(|p| p == &[0, 0, 0, 255]));
    drop(gpu);
    finish(before);
}
