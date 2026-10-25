//! Authored write/load/copy/sample cases with independent per-pixel oracles.
use crate::{
    frame_plan as fp,
    frame_probe::{Builder, SIZE},
    sampled_frame as sf, target_frame as tf,
};
use std::sync::Arc;
pub struct Fixture {
    pub id: u32,
    pub frame: tf::TargetFrame,
    pub expected: Vec<u8>,
    pub tolerance: u8,
}
const BLACK: [u8; 4] = [0, 0, 0, 255];
const RED: [u8; 4] = [255, 0, 0, 255];
const GREEN: [u8; 4] = [0, 255, 0, 255];
const BLUE: [u8; 4] = [0, 0, 255, 255];
const WHITE: [u8; 4] = [255, 255, 255, 255];
const FULL: [u32; 4] = [0, 0, SIZE, SIZE];
fn v(id: u32, generation: u64) -> fp::Version {
    fp::Version { id, generation }
}
fn color(g: u64) -> fp::Version {
    v(1000, g)
}
fn depth(g: u64) -> fp::Version {
    v(2000, g)
}
fn cpu(id: u32) -> fp::Version {
    v(100 + id, 1)
}
fn texture(id: u32, width: u32, height: u32, levels: &[Vec<[u8; 4]>], padding: u32) -> sf::TextureVersion {
    sf::TextureVersion {
        version: cpu(id),
        width,
        height,
        mips: levels
            .iter()
            .enumerate()
            .map(|(level, pixels)| {
                let w = (width >> level).max(1);
                let h = (height >> level).max(1);
                assert_eq!(pixels.len(), (w * h) as usize);
                let pitch = w * 4 + padding;
                let mut data = vec![0xd7; (pitch * h) as usize];
                for y in 0..h {
                    for x in 0..w {
                        let start = (y * pitch + x * 4) as usize;
                        data[start..start + 4].copy_from_slice(&pixels[(y * w + x) as usize]);
                    }
                }
                sf::Mip { bytes_per_row: pitch, bytes: Arc::from(data) }
            })
            .collect(),
    }
}
#[derive(Clone, Copy)]
struct Sample {
    source: fp::Version,
    size: [u32; 2],
    uv: [f32; 2],
    rect: [u32; 4],
    z: f32,
    depth: bool,
    blend: bool,
}
fn sample(source: fp::Version, size: [u32; 2], uv: [f32; 2], rect: [u32; 4]) -> Sample {
    Sample { source, size, uv, rect, z: 0.5, depth: false, blend: false }
}
fn pattern(source: fp::Version, size: [u32; 2]) -> Vec<Sample> {
    [
        ([0.25, 0.25], [0, 0, 16, 16]),
        ([0.75, 0.25], [16, 0, 16, 16]),
        ([0.25, 0.75], [0, 16, 16, 16]),
        ([0.75, 0.75], [16, 16, 16, 16]),
    ]
    .into_iter()
    .map(|(uv, rect)| sample(source, size, uv, rect))
    .collect()
}
struct Scene {
    builder: Builder,
    passes: Vec<fp::Pass>,
    textures: Vec<sf::TextureVersion>,
    targets: Vec<tf::TargetVersion>,
    ops: Vec<tf::Operation>,
    bindings: Vec<Vec<sf::Binding>>,
}
impl Scene {
    fn new() -> Self {
        Self {
            builder: Builder::new(),
            passes: vec![],
            textures: vec![],
            targets: vec![],
            ops: vec![],
            bindings: vec![],
        }
    }
    fn standard() -> Self {
        let mut s = Self::new();
        s.textures = vec![
            texture(0, 2, 2, &[vec![RED, GREEN, BLUE, WHITE]], 12),
            texture(1, 1, 1, &[vec![RED]], 0),
            texture(2, 1, 1, &[vec![GREEN]], 0),
            texture(3, 1, 1, &[vec![BLUE]], 0),
            texture(4, 1, 1, &[vec![[0, 0, 255, 128]]], 0),
        ];
        s
    }
    fn target(&mut self, version: fp::Version, kind: tf::Kind, width: u32, height: u32) {
        assert!(!self.targets.iter().any(|t| t.version == version));
        self.targets.push(tf::TargetVersion { version, kind, width, height });
    }
    fn pass(
        &mut self,
        generation: u64,
        clear_color: Option<[u8; 4]>,
        clear_depth: Option<f64>,
        loads: [Option<fp::Version>; 2],
        stores: [bool; 2],
        samples: Vec<Sample>,
    ) {
        self.target(color(generation), tf::Kind::Color, SIZE, SIZE);
        self.target(depth(generation), tf::Kind::Depth, SIZE, SIZE);
        let initial = self.builder.resources.iter().find(|r| r.version.id == 12).unwrap().bytes.to_vec();
        let mut draws = Vec::new();
        for s in samples {
            let ordinal = self.bindings.len() as u64;
            let mut draw =
                self.builder.draw([s.uv[0], s.uv[1], 0., 1.], s.z, s.rect, false, s.blend, s.depth, ordinal + 1);
            let mut fetch = xenos_texture::TextureFetchConstant::new_2d(
                xenos_texture::TextureFormat::k_8_8_8_8,
                s.size[0],
                s.size[1],
            );
            fetch.base_page = 1;
            fetch.clamp = [xenos_texture::ClampMode::ClampToEdge; 3];
            fetch.min_filter = xenos_texture::TextureFilter::Point;
            fetch.mag_filter = xenos_texture::TextureFilter::Point;
            fetch.mip_filter = xenos_texture::TextureFilter::Point;
            fetch.aniso_filter = xenos_texture::AnisoFilter::Disabled;
            fetch.mip_min_level = 0;
            fetch.mip_max_level = 0;
            fetch.lod_bias = 0;
            let state =
                sf::SamplerState::from_host(&xenos_texture::sampler_descriptor(&fetch, &Default::default())).unwrap();
            assert_eq!(state, sf::SamplerState { lod: [0., 0.], ..Default::default() });
            let mut bytes = initial.clone();
            for (i, w) in fetch.encode().into_iter().enumerate() {
                bytes[24 + i * 4..28 + i * 4].copy_from_slice(&w.to_le_bytes());
            }
            let slice = self.builder.resource(12, ordinal + 2, bytes);
            draw.bindings.iter_mut().find(|b| b.group == 0 && b.binding == 2).unwrap().slice = slice;
            draws.push(draw);
            self.bindings.push(vec![
                sf::Binding { binding: 0, resource: sf::Resource::Texture(s.source) },
                sf::Binding { binding: 1, resource: sf::Resource::Sampler(v(3000, 1)) },
            ]);
        }
        self.ops.push(tf::Operation::Render(tf::Render {
            pass: self.passes.len() as u32,
            color: color(generation),
            depth: depth(generation),
            color_load: loads[0],
            depth_load: loads[1],
            store_color: stores[0],
            store_depth: stores[1],
        }));
        self.passes.push(fp::Pass {
            clear_color: clear_color.map(|c| c.map(|v| f64::from(v) / 255.)),
            clear_depth,
            draws,
        });
    }
    fn copy(
        &mut self,
        source: fp::Version,
        mip: u32,
        origin: [u32; 2],
        destination: fp::Version,
        kind: tf::Kind,
        size: [u32; 2],
    ) {
        self.target(destination, kind, size[0], size[1]);
        self.ops.push(tf::Operation::Copy(tf::Copy {
            source: tf::ImageSlice { version: source, mip_level: mip },
            origin,
            destination,
        }));
    }
    fn finish(self, id: u32, output: fp::Version, oracle: impl Fn(u32, u32) -> [u8; 4], tolerance: u8) -> Fixture {
        let samplers = if self.bindings.is_empty() {
            vec![]
        } else {
            vec![sf::SamplerVersion {
                version: v(3000, 1),
                state: sf::SamplerState { lod: [0., 0.], ..Default::default() },
            }]
        };
        let frame = if self.passes[0].clear_color.is_some() && self.passes[0].clear_depth.is_some() {
            let geometry = fp::FramePlan::new(SIZE, SIZE, self.builder.resources, self.passes).unwrap();
            tf::TargetFrame::new(geometry, self.textures, samplers, self.targets, self.ops, self.bindings, output)
        } else {
            tf::TargetFrame::from_geometry(
                tf::Geometry { width: SIZE, height: SIZE, resources: self.builder.resources, passes: self.passes },
                self.textures,
                samplers,
                self.targets,
                self.ops,
                self.bindings,
                output,
            )
        }
        .unwrap();
        let mut expected = Vec::new();
        for y in 0..frame.output().height {
            for x in 0..frame.output().width {
                expected.extend_from_slice(&oracle(x, y));
            }
        }
        Fixture { id, frame, expected, tolerance }
    }
}
pub fn fixtures() -> Vec<Fixture> {
    let mut out = Vec::new();
    let oracle = |x, y| [RED, GREEN, BLUE, WHITE][usize::from(x >= 16) + 2 * usize::from(y >= 16)];
    // CPU -> render target -> shader read. Both axes and all channels are asymmetric.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(cpu(0), [2, 2]));
    s.pass(2, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(color(1), [32, 32]));
    out.push(s.finish(1, color(2), oracle, 0));
    // New generation with the same owner ID must not change reads of the old one.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(cpu(0), [2, 2]));
    s.pass(2, Some(BLUE), Some(1.), [None; 2], [true; 2], vec![sample(cpu(2), [1, 1], [0.5; 2], [0, 0, 16, 32])]);
    s.pass(
        3,
        Some(BLACK),
        Some(1.),
        [None; 2],
        [true; 2],
        vec![
            sample(color(1), [32, 32], [0.25; 2], [0, 0, 16, 16]),
            sample(color(2), [32, 32], [0.25; 2], [16, 0, 16, 16]),
            sample(color(1), [32, 32], [0.75; 2], [0, 16, 16, 16]),
            sample(color(2), [32, 32], [0.75; 2], [16, 16, 16, 16]),
        ],
    );
    out.push(s.finish(
        2,
        color(3),
        |x, y| [RED, GREEN, WHITE, BLUE][usize::from(x >= 16) + 2 * usize::from(y >= 16)],
        0,
    ));
    // Load copies old color/depth into new objects. Only the small new draw changes.
    let mut s = Scene::standard();
    s.pass(1, Some(RED), Some(1.), [None; 2], [true; 2], vec![sample(cpu(2), [1, 1], [0.5; 2], [0, 0, 16, 32])]);
    s.pass(
        2,
        None,
        None,
        [Some(color(1)), Some(depth(1))],
        [true; 2],
        vec![sample(cpu(3), [1, 1], [0.5; 2], [16, 0, 16, 16])],
    );
    out.push(s.finish(
        3,
        color(2),
        |x, y| {
            if x < 16 {
                GREEN
            } else if y < 16 {
                BLUE
            } else {
                RED
            }
        },
        0,
    ));
    // Full color copy, then read the copy in another render pass.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(cpu(0), [2, 2]));
    s.copy(color(1), 0, [0, 0], color(2), tf::Kind::Color, [32, 32]);
    s.pass(3, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(color(2), [32, 32]));
    out.push(s.finish(4, color(3), oracle, 0));
    // A subrectangle fully initializes a smaller target; readback dimensions differ.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(cpu(0), [2, 2]));
    s.copy(color(1), 0, [16, 0], color(2), tf::Kind::Color, [16, 32]);
    out.push(s.finish(5, color(2), |_, y| if y < 16 { GREEN } else { WHITE }, 0));
    // Odd dimensions, mip 1 and padded rows reach an actual GPU copy operation.
    let mut s = Scene::new();
    s.textures.push(texture(5, 3, 5, &[vec![BLUE; 15], vec![RED, GREEN]], 4));
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [false; 2], vec![]);
    s.copy(cpu(5), 1, [0, 0], color(2), tf::Kind::Color, [1, 2]);
    out.push(s.finish(6, color(2), |_, y| if y == 0 { RED } else { GREEN }, 0));
    // Preserving depth across versions must still reject a later farther draw.
    let mut s = Scene::standard();
    let mut near = sample(cpu(1), [1, 1], [0.5; 2], FULL);
    near.z = 0.25;
    near.depth = true;
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], vec![near]);
    let mut far = sample(cpu(2), [1, 1], [0.5; 2], FULL);
    far.z = 0.75;
    far.depth = true;
    let mut front = sample(cpu(3), [1, 1], [0.5; 2], [0, 0, 16, 32]);
    front.z = 0.125;
    front.depth = true;
    s.pass(2, None, None, [Some(color(1)), Some(depth(1))], [true; 2], vec![far, front]);
    out.push(s.finish(7, color(2), |x, _| if x < 16 { BLUE } else { RED }, 0));
    // Explicit full-depth copy followed by a load copy into another depth version.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], vec![near]);
    s.copy(depth(1), 0, [0, 0], depth(2), tf::Kind::Depth, [32, 32]);
    s.pass(3, None, None, [Some(color(1)), Some(depth(2))], [true; 2], vec![far]);
    out.push(s.finish(8, color(3), |_, _| RED, 0));
    // Nonzero X/Y origin and non-256-byte pitch in a CPU-uploaded image.
    let mut s = Scene::new();
    let pixels = (0..15).map(|i| [i * 11, 255 - i * 7, i * 3, 255]).collect::<Vec<_>>();
    s.textures.push(texture(5, 3, 5, &[pixels], 8));
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [false; 2], vec![]);
    s.copy(cpu(5), 0, [1, 1], color(2), tf::Kind::Color, [2, 3]);
    out.push(s.finish(
        9,
        color(2),
        |x, y| {
            let i = ((y + 1) * 3 + x + 1) as u8;
            [i * 11, 255 - i * 7, i * 3, 255]
        },
        0,
    ));
    // A clear creates a shader-readable image without any uploaded CPU texture.
    let mut s = Scene::new();
    s.pass(1, Some(RED), Some(1.), [None; 2], [true; 2], vec![]);
    s.pass(2, Some(BLACK), Some(1.), [None; 2], [true; 2], vec![sample(color(1), [32, 32], [0.5; 2], FULL)]);
    out.push(s.finish(10, color(2), |_, _| RED, 0));
    // Discard is legal for dead versions; it must not change unrelated later clears.
    let mut s = Scene::new();
    s.pass(1, Some(GREEN), Some(1.), [None; 2], [false; 2], vec![]);
    s.pass(2, Some(BLUE), Some(1.), [None; 2], [true, false], vec![]);
    out.push(s.finish(11, color(2), |_, _| BLUE, 0));
    // Blending reads a loaded destination, not an implicitly cleared color.
    let mut s = Scene::standard();
    s.pass(1, Some(RED), Some(1.), [None; 2], [true; 2], vec![]);
    let mut alpha = sample(cpu(4), [1, 1], [0.5; 2], FULL);
    alpha.blend = true;
    s.pass(2, None, Some(1.), [Some(color(1)), None], [true; 2], vec![alpha]);
    out.push(s.finish(12, color(2), |_, _| [127, 0, 128, 255], 1));
    // Loading AND sampling the old color forbids in-place slot transfer.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(cpu(0), [2, 2]));
    s.pass(
        2,
        None,
        None,
        [Some(color(1)), Some(depth(1))],
        [true; 2],
        vec![sample(color(1), [32, 32], [0.25; 2], [16, 16, 16, 16])],
    );
    out.push(s.finish(13, color(2), |x, y| if x >= 16 && y >= 16 { RED } else { oracle(x, y) }, 0));
    // Declaration order is not execution order; version lookup must be explicit.
    let mut s = Scene::standard();
    s.pass(1, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(cpu(0), [2, 2]));
    s.pass(2, Some(BLACK), Some(1.), [None; 2], [true; 2], pattern(color(1), [32, 32]));
    s.targets.reverse();
    out.push(s.finish(14, color(2), oracle, 0));
    // A CPU copy precedes the first pass: load that image and blend only one
    // quadrant. An implicit first clear would corrupt both untouched and blended pixels.
    let pixels = (0..SIZE).flat_map(|y| (0..SIZE).map(move |x| oracle(x, y))).collect::<Vec<_>>();
    let mut s = Scene::standard();
    s.textures.push(texture(5, SIZE, SIZE, &[pixels.clone()], 4));
    s.copy(cpu(5), 0, [0, 0], color(1), tf::Kind::Color, [SIZE; 2]);
    let mut alpha = sample(cpu(4), [1, 1], [0.5; 2], [16, 0, 16, 16]);
    alpha.blend = true;
    s.pass(2, None, Some(1.), [Some(color(1)), None], [true; 2], vec![alpha]);
    out.push(s.finish(
        15,
        color(2),
        |x, y| {
            if x >= 16 && y < 16 { [0, 127, 128, 255] } else { oracle(x, y) }
        },
        1,
    ));
    // Mip 1 and padded CPU rows initialize the first load without any draws or
    // sampled shader bindings. Mip 0 is deliberately different.
    let mut s = Scene::new();
    s.textures.push(texture(5, SIZE * 2, SIZE * 2, &[vec![BLUE; (SIZE * SIZE * 4) as usize], pixels], 12));
    s.copy(cpu(5), 1, [0, 0], color(1), tf::Kind::Color, [SIZE; 2]);
    s.pass(2, None, Some(1.), [Some(color(1)), None], [true; 2], vec![]);
    out.push(s.finish(16, color(2), oracle, 0));
    out
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
    fn ordered_target_operations_match_independent_pixels_in_both_orders() {
        let instance = Instance::new().unwrap();
        let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
        let device = instance.block_on(adapter.request_device(&device_descriptor(&adapter).unwrap())).unwrap();
        let backend = WebGpuFrames::new_sampled(device).unwrap();
        let scenes = fixtures();
        assert_eq!(scenes.len(), 16);
        for f in scenes.iter().chain(scenes.iter().rev()) {
            if f.id >= 15 {
                assert!(backend.render(f.frame.geometry()).unwrap_err().contains("TargetFrame"));
                let geometry = f.frame.geometry();
                let inputs = sf::SampledFrame::inputs(
                    fp::FramePlan::target_geometry(
                        geometry.width(),
                        geometry.height(),
                        geometry.resources().to_vec(),
                        geometry.passes().to_vec(),
                    )
                    .unwrap(),
                    f.frame.textures().to_vec(),
                    f.frame.samplers().to_vec(),
                )
                .unwrap();
                assert!(backend.render_sampled(&inputs).unwrap_err().contains("TargetFrame"));
            }
            let actual = backend.render_targets(&f.frame).unwrap();
            let mismatch = actual.iter().zip(&f.expected).position(|(a, b)| a.abs_diff(*b) > f.tolerance);
            assert!(
                verify(f, &actual),
                "target case={} mismatch={:?}",
                f.id,
                mismatch.map(|i| (i, actual[i], f.expected[i]))
            );
            let reused = backend.render_targets_reusing(&f.frame).unwrap();
            assert!(verify(f, &reused), "reused target case={}", f.id);
            assert!(actual.iter().zip(&reused).all(|(a, b)| a.abs_diff(*b) <= f.tolerance));
            let allocation = tf::AllocationPlan::new(&f.frame, true);
            eprintln!("AUTHORED_TARGET_ORACLE case={} bytes={} dedicated_and_reused_match=true", f.id, actual.len());
            eprintln!(
                "AUTHORED_TARGET_ALLOCATION case={} dedicated_bytes={} reused_bytes={} dedicated_targets={} reused_slots={} skipped_loads={} skipped_copies={}",
                f.id,
                allocation.dedicated_bytes(),
                allocation.allocated_bytes(),
                f.frame.targets().len(),
                allocation.slots().len(),
                allocation.load_elided().iter().flatten().filter(|x| **x).count(),
                allocation.copy_elided().iter().filter(|x| **x).count()
            );
        }
    }
    #[test]
    fn oracle_detects_corruption_and_output_sizes_follow_the_selected_target() {
        let scenes = fixtures();
        assert_eq!(scenes.len(), 16);
        for f in scenes {
            assert_eq!(f.expected.len(), (f.frame.output().width * f.frame.output().height * 4) as usize);
            assert!(f.frame.packet().len() < fp::MAX_PACKET);
            let mut bytes = f.expected.clone();
            bytes[0] = if bytes[0] < 128 { 255 } else { 0 };
            assert!(!verify(&f, &bytes));
        }
    }
}
