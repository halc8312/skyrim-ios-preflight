//! Independent pixel oracles for authored Xenos 2D texture fetches.
//! No original game shader/data input. Not a renderer benchmark.
use crate::{
    frame_plan as fp,
    frame_probe::{self, Builder, SIZE},
    sampled_frame as sf,
};
use std::sync::Arc;
use xenos_shader::{Shader, ShaderStage, Translation, TranslationOptions};

pub fn shaders() -> Result<[Translation; 2], String> {
    let [vertex, _] = frame_probe::shaders()?;
    // EXEC_END: one 2D texture fetch fc1 r0.xy -> r0, then export r0.
    // All instruction words are authored here from the public Xenos bitfields.
    let fetch1 = 0x688 | 3 << 12 | 3 << 14 | 3 << 16 | 7 << 18 | 7 << 21 | 3 << 24 | 3 << 26;
    let words =
        [0x00012001, 0x00002200, 0, 1 | 1 << 19 | 1 << 20 | 4 << 26, fetch1, 1 << 14, 0xC80F8000, 0, 0xE2000000];
    let decoded =
        xenos_shader::parsed::parse_texture_fetch(&xenos_shader::ucode::RawFetch([words[3], words[4], words[5]]));
    if decoded.dimension != xenos_shader::parsed::FetchDimension::D2
        || decoded.attributes.use_computed_lod
        || decoded.attributes.use_register_lod
        || decoded.attributes.use_register_gradients
        || decoded.attributes.offset != [0.; 3]
        || decoded.attributes.lod_bias != 0.
    {
        return Err("explicit authored LOD/offset decode differs".into());
    }
    let pixel = Shader::new(ShaderStage::Pixel, &words).map_err(|_| "authored texture decode")?.translate(
        &TranslationOptions { vs_output_interpolators: 1, ps_interpolator_count: Some(1), ..Default::default() },
    );
    if pixel.texture_bindings.len() != 1 || pixel.texture_bindings[0].fetch_constant != 1 {
        return Err("authored texture ABI".into());
    }
    if !pixel.wgsl.contains("textureSampleLevel(") || pixel.wgsl.contains("textureSampleBias(") {
        return Err("explicit authored LOD lowering differs".into());
    }
    Ok([vertex, pixel])
}
pub struct Fixture {
    pub id: u32,
    pub frame: sf::SampledFrame,
    pub expected: Vec<u8>,
    pub tolerance: u8,
}
const BLACK: [u8; 4] = [0, 0, 0, 255];
const RED: [u8; 4] = [255, 0, 0, 255];
const GREEN: [u8; 4] = [0, 255, 0, 255];
const BLUE: [u8; 4] = [0, 0, 255, 255];
const WHITE: [u8; 4] = [255, 255, 255, 255];
fn version(id: u32, generation: u64) -> fp::Version {
    fp::Version { id, generation }
}
fn texture(generation: u64, width: u32, height: u32, levels: &[Vec<[u8; 4]>], padding: u32) -> sf::TextureVersion {
    let mips = levels
        .iter()
        .enumerate()
        .map(|(level, pixels)| {
            let w = (width >> level).max(1);
            let h = (height >> level).max(1);
            assert_eq!(pixels.len(), (w * h) as usize);
            let pitch = w * 4 + padding;
            let mut bytes = vec![0xdc; (pitch * h) as usize];
            for y in 0..h {
                for x in 0..w {
                    let at = (y * pitch + x * 4) as usize;
                    bytes[at..at + 4].copy_from_slice(&pixels[(y * w + x) as usize]);
                }
            }
            sf::Mip { bytes_per_row: pitch, bytes: Arc::from(bytes) }
        })
        .collect();
    sf::TextureVersion { version: version(100, generation), width, height, mips }
}
fn sampler(generation: u64, state: sf::SamplerState) -> sf::SamplerVersion {
    sf::SamplerVersion { version: version(200, generation), state }
}
#[derive(Clone, Copy)]
struct Sample {
    texture: usize,
    sampler: usize,
    uv: [f32; 2],
    lod: f32,
    rect: [u32; 4],
    blend: bool,
    swizzle: Option<[xenos_texture::SwizzleSource; 4]>,
}
fn sample(texture: usize, sampler: usize, uv: [f32; 2], lod: f32, rect: [u32; 4]) -> Sample {
    Sample { texture, sampler, uv, lod, rect, blend: false, swizzle: None }
}
fn scene(
    id: u32,
    textures: Vec<sf::TextureVersion>,
    mut samplers: Vec<sf::SamplerVersion>,
    samples: Vec<Sample>,
    clear: [u8; 4],
    expected: impl Fn(u32, u32) -> [u8; 4],
    tolerance: u8,
) -> Fixture {
    let mut b = Builder::new();
    let mut draws = Vec::new();
    let mut bindings = Vec::new();
    let initial_fetch = b.resources.iter().find(|r| r.version.id == 12).unwrap().bytes.to_vec();
    for (i, sampler) in samplers.iter_mut().enumerate() {
        let max = samples.iter().filter(|s| s.sampler == i).map(|s| textures[s.texture].mips.len() - 1).min().unwrap();
        sampler.state.lod[1] = sampler.state.lod[1].min(max as f32);
    }
    for (ordinal, s) in samples.iter().enumerate() {
        let mut draw = b.draw([s.uv[0], s.uv[1], 0., 1.], 0.5, s.rect, false, s.blend, false, ordinal as u64 + 1);
        let t = &textures[s.texture];
        let mut fetch =
            xenos_texture::TextureFetchConstant::new_2d(xenos_texture::TextureFormat::k_8_8_8_8, t.width, t.height);
        fetch.base_page = 1;
        fetch.lod_bias = (s.lod * 32.) as i32;
        let state = samplers[s.sampler].state;
        fetch.clamp = state.address.map(|a| match a {
            sf::AddressMode::ClampToEdge => xenos_texture::ClampMode::ClampToEdge,
            sf::AddressMode::Repeat => xenos_texture::ClampMode::Repeat,
            sf::AddressMode::MirrorRepeat => xenos_texture::ClampMode::MirroredRepeat,
        });
        let filter = |f| {
            if f == sf::FilterMode::Nearest {
                xenos_texture::TextureFilter::Point
            } else {
                xenos_texture::TextureFilter::Linear
            }
        };
        fetch.min_filter = filter(state.filters[0]);
        fetch.mag_filter = filter(state.filters[1]);
        fetch.mip_filter = filter(state.filters[2]);
        fetch.aniso_filter = xenos_texture::AnisoFilter::Disabled;
        fetch.mip_min_level = state.lod[0] as u32;
        fetch.mip_max_level = state.lod[1] as u32;
        if fetch.mip_max_level > 0 {
            fetch.mip_page = 2;
        }
        if let Some(swizzle) = s.swizzle {
            fetch.swizzle = swizzle;
        }
        let host = xenos_texture::sampler_descriptor(&fetch, &xenos_texture::FetchInstructionFilters::default());
        assert_eq!(
            sf::SamplerState::from_host(&host).unwrap(),
            state,
            "shader fetch and sampler must describe the same state"
        );
        let mut raw = initial_fetch.clone();
        for (i, w) in fetch.encode().into_iter().enumerate() {
            raw[24 + i * 4..28 + i * 4].copy_from_slice(&w.to_le_bytes());
        }
        let slice = b.resource(12, ordinal as u64 + 2, raw);
        draw.bindings.iter_mut().find(|b| b.group == 0 && b.binding == 2).unwrap().slice = slice;
        draws.push(draw);
        bindings.push(vec![
            sf::Binding { binding: 0, resource: sf::Resource::Texture(t.version) },
            sf::Binding { binding: 1, resource: sf::Resource::Sampler(samplers[s.sampler].version) },
        ]);
    }
    let geometry = fp::FramePlan::new(
        SIZE,
        SIZE,
        b.resources,
        vec![fp::Pass { clear_color: Some(clear.map(|v| f64::from(v) / 255.)), clear_depth: Some(1.), draws }],
    )
    .unwrap();
    let frame = sf::SampledFrame::new(geometry, textures, samplers, bindings).unwrap();
    let mut pixels = Vec::new();
    for y in 0..SIZE {
        for x in 0..SIZE {
            pixels.extend_from_slice(&expected(x, y));
        }
    }
    Fixture { id, frame, expected: pixels, tolerance }
}
pub fn fixtures() -> Vec<Fixture> {
    use sf::{AddressMode as A, FilterMode as F, SamplerState as S};
    const FULL: [u32; 4] = [0, 0, SIZE, SIZE];
    let nearest = || vec![sampler(1, S::default())];
    let pattern = || texture(1, 2, 2, &[vec![RED, GREEN, BLUE, WHITE]], 8);
    let mip = || texture(1, 4, 4, &[vec![RED; 16], vec![GREEN; 4], vec![BLUE; 1]], 4);
    let one = |tex, samp, uv, lod| vec![sample(tex, samp, uv, lod, FULL)];
    let mut result = Vec::new();
    // Orientation and all channels, not a uniform-color shader masquerading as a texture.
    result.push(scene(
        1,
        vec![pattern()],
        nearest(),
        vec![
            sample(0, 0, [0.25, 0.25], 0., [0, 0, 16, 16]),
            sample(0, 0, [0.75, 0.25], 0., [16, 0, 16, 16]),
            sample(0, 0, [0.25, 0.75], 0., [0, 16, 16, 16]),
            sample(0, 0, [0.75, 0.75], 0., [16, 16, 16, 16]),
        ],
        BLACK,
        |x, y| [RED, GREEN, BLUE, WHITE][usize::from(x >= 16) + 2 * usize::from(y >= 16)],
        0,
    ));
    result.push(scene(
        2,
        vec![texture(1, 1, 1, &[vec![RED]], 0), texture(2, 1, 1, &[vec![BLUE]], 0)],
        nearest(),
        vec![
            sample(0, 0, [0.5; 2], 0., FULL),
            sample(1, 0, [0.5; 2], 0., [16, 0, 16, 32]),
            sample(0, 0, [0.5; 2], 0., [16, 0, 16, 16]),
        ],
        BLACK,
        |x, y| if x >= 16 && y >= 16 { BLUE } else { RED },
        0,
    ));
    result.push(scene(
        3,
        vec![pattern()],
        vec![
            sampler(1, S::default()),
            sampler(2, S { address: [A::Repeat, A::ClampToEdge, A::ClampToEdge], ..Default::default() }),
        ],
        vec![sample(0, 1, [1.25, 0.25], 0., [0, 0, 16, 32]), sample(0, 0, [1.25, 0.25], 0., [16, 0, 16, 32])],
        BLACK,
        |x, _| if x < 16 { RED } else { GREEN },
        0,
    ));
    result.push(scene(
        4,
        vec![pattern()],
        vec![sampler(1, S { address: [A::MirrorRepeat, A::MirrorRepeat, A::ClampToEdge], ..Default::default() })],
        [-0.25, -0.75, 1.25, 1.75]
            .into_iter()
            .enumerate()
            .map(|(i, u)| sample(0, 0, [u, 0.25], 0., [i as u32 * 8, 0, 8, 32]))
            .collect(),
        BLACK,
        |x, _| if x < 8 || x >= 24 { RED } else { GREEN },
        0,
    ));
    result.push(scene(
        5,
        vec![pattern()],
        vec![sampler(1, S { filters: [F::Linear; 3], ..Default::default() })],
        one(0, 0, [0.5; 2], 0.),
        BLACK,
        |_, _| [128, 128, 128, 255],
        1,
    ));
    result.push(scene(
        6,
        vec![mip()],
        nearest(),
        [0., 1., 2.]
            .into_iter()
            .enumerate()
            .map(|(i, lod)| {
                sample(
                    0,
                    0,
                    [0.25; 2],
                    lod,
                    [
                        if i == 0 {
                            0
                        } else if i == 1 {
                            10
                        } else {
                            21
                        },
                        0,
                        if i == 0 { 10 } else { 11 },
                        32,
                    ],
                )
            })
            .collect(),
        BLACK,
        |x, _| {
            if x < 10 {
                RED
            } else if x < 21 {
                GREEN
            } else {
                BLUE
            }
        },
        0,
    ));
    result.push(scene(
        7,
        vec![mip()],
        vec![sampler(1, S { lod: [1., 1.], ..Default::default() })],
        vec![sample(0, 0, [0.25; 2], 0., [0, 0, 16, 32]), sample(0, 0, [0.25; 2], 2., [16, 0, 16, 32])],
        BLACK,
        |_, _| GREEN,
        0,
    ));
    result.push(scene(
        8,
        vec![mip()],
        vec![sampler(1, S { filters: [F::Linear; 3], ..Default::default() })],
        one(0, 0, [0.5; 2], 0.5),
        BLACK,
        |_, _| [128, 128, 0, 255],
        1,
    ));
    result.push(scene(
        9,
        vec![mip()],
        vec![sampler(1, S { filters: [F::Linear; 3], ..Default::default() })],
        one(0, 0, [0.5; 2], 1.5),
        BLACK,
        |_, _| [0, 128, 128, 255],
        1,
    ));
    let colors = [RED, GREEN, BLUE, WHITE, [255, 0, 255, 255], [0, 255, 255, 255]];
    result.push(scene(
        10,
        vec![texture(1, 3, 2, &[colors.to_vec()], 20)],
        nearest(),
        (0..6)
            .map(|i| {
                sample(
                    0,
                    0,
                    [(i % 3) as f32 / 3. + 1. / 6., (i / 3) as f32 / 2. + 0.25],
                    0.,
                    [
                        if i % 3 == 0 {
                            0
                        } else if i % 3 == 1 {
                            10
                        } else {
                            21
                        },
                        if i < 3 { 0 } else { 16 },
                        if i % 3 == 0 { 10 } else { 11 },
                        16,
                    ],
                )
            })
            .collect(),
        BLACK,
        |x, y| {
            colors[(if x < 10 {
                0
            } else if x < 21 {
                1
            } else {
                2
            }) + if y < 16 { 0 } else { 3 }]
        },
        0,
    ));
    let mut samples = one(0, 0, [0.5; 2], 0.);
    samples[0].blend = true;
    result.push(scene(
        11,
        vec![texture(1, 1, 1, &[vec![[0, 0, 255, 128]]], 4)],
        nearest(),
        samples,
        RED,
        |_, _| [127, 0, 128, 255],
        1,
    ));
    result.push(scene(
        12,
        vec![texture(1, 3, 5, &[vec![RED; 15], vec![GREEN; 2], vec![BLUE; 1]], 4)],
        nearest(),
        one(0, 0, [0.5; 2], 2.),
        BLACK,
        |_, _| BLUE,
        0,
    ));
    let mut swapped = one(0, 0, [0.25; 2], 0.);
    swapped[0].swizzle = Some([
        xenos_texture::SwizzleSource::Z,
        xenos_texture::SwizzleSource::Y,
        xenos_texture::SwizzleSource::X,
        xenos_texture::SwizzleSource::W,
    ]);
    result.push(scene(13, vec![pattern()], nearest(), swapped, BLACK, |_, _| BLUE, 0));
    result.push(scene(
        14,
        vec![pattern()],
        vec![sampler(1, S { filters: [F::Linear; 3], ..Default::default() })],
        one(0, 0, [0.375, 0.25], 0.),
        BLACK,
        |_, _| [191, 64, 0, 255],
        1,
    ));
    let mut source_fetch = xenos_texture::TextureFetchConstant::new_2d(xenos_texture::TextureFormat::k_8_8_8_8, 2, 2);
    source_fetch.base_page = 1;
    source_fetch.tiled = false;
    source_fetch.pitch_div_32 = 1;
    source_fetch.endianness = xenos_texture::Endian::K8in32;
    let mut memory = vec![0u8; 65536];
    for (i, color) in [RED, GREEN, BLUE, WHITE].into_iter().enumerate() {
        let mut bytes = color;
        source_fetch.endianness.swap_in_place(&mut bytes);
        let at = 4096 + (i / 2) * 128 + (i % 2) * 4;
        memory[at..at + 4].copy_from_slice(&bytes);
    }
    let loaded = xenos_texture::load_texture(&source_fetch, &memory, &xenos_texture::LoadOptions::default()).unwrap();
    let from_guest = sf::TextureVersion::from_loaded(version(100, 1), loaded).unwrap();
    result.push(scene(
        15,
        vec![from_guest],
        nearest(),
        vec![
            sample(0, 0, [0.25, 0.25], 0., [0, 0, 16, 16]),
            sample(0, 0, [0.75, 0.25], 0., [16, 0, 16, 16]),
            sample(0, 0, [0.25, 0.75], 0., [0, 16, 16, 16]),
            sample(0, 0, [0.75, 0.75], 0., [16, 16, 16, 16]),
        ],
        BLACK,
        |x, y| [RED, GREEN, BLUE, WHITE][usize::from(x >= 16) + 2 * usize::from(y >= 16)],
        0,
    ));
    result
}
pub fn verify(f: &Fixture, bytes: &[u8]) -> bool {
    bytes.len() == f.expected.len() && bytes.iter().zip(&f.expected).all(|(a, b)| a.abs_diff(*b) <= f.tolerance)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame_probe::{WebGpuFrames, device_descriptor};
    use gpu_webgpu::{Instance, RequestAdapterOptions};
    #[test]
    fn authored_xenos_texture_plans_match_independent_oracles() {
        let instance = Instance::new().unwrap();
        let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
        let device = instance.block_on(adapter.request_device(&device_descriptor(&adapter).unwrap())).unwrap();
        let backend = WebGpuFrames::new_sampled(device).unwrap();
        let fixtures = fixtures();
        assert_eq!(fixtures.len(), 15);
        for f in fixtures.iter().chain(fixtures.iter().rev()) {
            let bytes = backend.render_sampled(&f.frame).unwrap();
            let mismatch = bytes.iter().zip(&f.expected).position(|(a, b)| a.abs_diff(*b) > f.tolerance);
            assert!(
                verify(f, &bytes),
                "sampled case {} mismatch {:?}",
                f.id,
                mismatch.map(|i| (i, bytes[i], f.expected[i]))
            );
        }
        assert!(backend.render(fixtures[0].frame.geometry()).unwrap_err().contains("mode"));
    }
    #[test]
    fn sampled_texture_abi_and_oracles_are_independently_checked() {
        let shaders = shaders().unwrap();
        let m = xenos_shader::msl::lower(&shaders[1]).unwrap();
        let t = m.bindings.iter().find(|b| b.group == 2 && b.binding == 0).unwrap();
        assert!(t.used);
        assert_eq!(
            t.texture_type,
            Some(xenos_shader::msl::TextureType {
                shape: xenos_shader::msl::TextureShape::D2,
                scalar: xenos_shader::msl::TextureScalar::Float,
                multisampled: false
            })
        );
        let s = m.bindings.iter().find(|b| b.group == 2 && b.binding == 1).unwrap();
        assert!(s.used && !s.comparison_sampler);
        for f in fixtures() {
            assert!(f.frame.packet().len() < fp::MAX_PACKET);
            let mut corrupt = f.expected.clone();
            corrupt[0] = if corrupt[0] < 128 { 255 } else { 0 };
            assert!(!verify(&f, &corrupt));
        }
    }
}
