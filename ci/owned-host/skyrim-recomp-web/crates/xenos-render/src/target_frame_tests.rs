use super::*;
use crate::{frame_plan as fp, prepared::DrawState};
use std::sync::Arc;
fn v(id: u32, generation: u64) -> Version {
    Version { id, generation }
}
fn target(version: Version, kind: Kind) -> TargetVersion {
    TargetVersion { version, kind, width: 4, height: 4 }
}
fn draw() -> fp::Draw {
    fp::Draw {
        state: DrawState::new([0., 0., 4., 4., 0., 1.], [0, 0, 4, 4], [0.; 4], 0),
        topology: fp::Topology::Triangles,
        count: 3,
        indices: None,
        bindings: vec![],
        blend: false,
        depth_less_write: false,
    }
}
struct Draft {
    passes: Vec<fp::Pass>,
    textures: Vec<sf::TextureVersion>,
    samplers: Vec<sf::SamplerVersion>,
    targets: Vec<TargetVersion>,
    ops: Vec<Operation>,
    bindings: Vec<Vec<sf::Binding>>,
    output: Version,
}
impl Draft {
    fn new() -> Self {
        Self {
            passes: vec![
                fp::Pass { clear_color: Some([0., 0., 0., 1.]), clear_depth: Some(1.), draws: vec![] },
                fp::Pass { clear_color: None, clear_depth: None, draws: vec![] },
            ],
            textures: vec![],
            samplers: vec![],
            targets: vec![
                target(v(1, 1), Kind::Color),
                target(v(2, 1), Kind::Depth),
                target(v(1, 2), Kind::Color),
                target(v(2, 2), Kind::Depth),
            ],
            ops: vec![
                Operation::Render(Render {
                    pass: 0,
                    color: v(1, 1),
                    depth: v(2, 1),
                    color_load: None,
                    depth_load: None,
                    store_color: true,
                    store_depth: true,
                }),
                Operation::Render(Render {
                    pass: 1,
                    color: v(1, 2),
                    depth: v(2, 2),
                    color_load: Some(v(1, 1)),
                    depth_load: Some(v(2, 1)),
                    store_color: true,
                    store_depth: true,
                }),
            ],
            bindings: vec![],
            output: v(1, 2),
        }
    }
    fn render(&mut self, i: usize) -> &mut Render {
        let Operation::Render(r) = &mut self.ops[i] else { panic!() };
        r
    }
    fn read(&mut self, pass: usize, version: Version) {
        self.passes[pass].draws.push(draw());
        self.bindings.push(vec![sf::Binding { binding: 0, resource: sf::Resource::Texture(version) }]);
    }
    fn build(self) -> Result<TargetFrame, Error> {
        TargetFrame::new(
            FramePlan::new(4, 4, vec![], self.passes)?,
            self.textures,
            self.samplers,
            self.targets,
            self.ops,
            self.bindings,
            self.output,
        )
    }
    fn build_contextual(self) -> Result<TargetFrame, Error> {
        TargetFrame::from_geometry(
            Geometry { width: 4, height: 4, resources: vec![], passes: self.passes },
            self.textures,
            self.samplers,
            self.targets,
            self.ops,
            self.bindings,
            self.output,
        )
    }
    fn precopy() -> Self {
        let mut d = Self::new();
        d.passes.truncate(1);
        d.passes[0].clear_color = None;
        d.targets.truncate(2);
        d.targets.push(target(v(3, 1), Kind::Color));
        d.ops.truncate(1);
        d.render(0).color_load = Some(v(3, 1));
        d.ops.insert(
            0,
            Operation::Copy(Copy {
                source: ImageSlice { version: v(50, 1), mip_level: 0 },
                origin: [0, 0],
                destination: v(3, 1),
            }),
        );
        d.textures.push(sf::TextureVersion {
            version: v(50, 1),
            width: 4,
            height: 4,
            mips: vec![sf::Mip { bytes_per_row: 20, bytes: Arc::from([0x71; 80]) }],
        });
        d.output = v(1, 1);
        d
    }
}
fn texture() -> sf::TextureVersion {
    sf::TextureVersion {
        version: v(50, 1),
        width: 3,
        height: 5,
        mips: vec![
            sf::Mip { bytes_per_row: 16, bytes: Arc::from([0x31; 80]) },
            sf::Mip { bytes_per_row: 8, bytes: Arc::from([0x72; 16]) },
            sf::Mip { bytes_per_row: 4, bytes: Arc::from([0xd5; 4]) },
        ],
    }
}
#[test]
fn clear_produces_first_images_without_dummy_cpu_textures_or_samplers() {
    let frame = Draft::new().build().unwrap();
    assert!(frame.textures().is_empty() && frame.samplers().is_empty());
    assert_eq!(frame.output().version, v(1, 2));
    assert_eq!(frame.packet(), frame.packet());
    let p = frame.packet();
    assert_eq!(&p[..8], b"OHRT0001");
    assert_eq!(&p[80..88], b"OHST0001");
    assert_eq!(u32::from_le_bytes(p[12..16].try_into().unwrap()) as usize, p.len());
}
#[test]
fn precopy_initializes_first_color_load_only_inside_the_target_graph() {
    let frame = Draft::precopy().build_contextual().unwrap();
    assert_eq!(frame.geometry().require_first_clear(), Err(Error::Uninitialized));
    assert_eq!(frame.operations().len(), 2);
    assert_eq!(frame.output().version, v(1, 1));
    assert_eq!(Draft::precopy().build().unwrap_err(), Error::Uninitialized);
}
#[test]
fn first_load_still_refuses_missing_late_self_and_ambiguous_initialization() {
    let mut d = Draft::precopy();
    d.ops.remove(0);
    assert_eq!(d.build_contextual().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::precopy();
    d.ops.swap(0, 1);
    assert_eq!(d.build_contextual().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::precopy();
    d.render(1).color_load = None;
    assert_eq!(d.build_contextual().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::precopy();
    d.render(1).color_load = Some(v(1, 1));
    assert_eq!(d.build_contextual().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::precopy();
    d.passes[0].clear_color = Some([0.; 4]);
    assert_eq!(d.build_contextual().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::precopy();
    d.passes[0].clear_depth = None;
    assert_eq!(d.build_contextual().unwrap_err(), Error::Uninitialized);
}
#[test]
fn contextual_geometry_keeps_image_kind_size_and_draw_bounds_validation() {
    let mut d = Draft::precopy();
    d.targets[2].kind = Kind::Depth;
    assert_eq!(d.build_contextual().unwrap_err(), Error::State);
    let mut d = Draft::precopy();
    d.targets[2].width = 3;
    assert_eq!(d.build_contextual().unwrap_err(), Error::State);
    let mut d = Draft::precopy();
    let mut invalid = draw();
    invalid.state = DrawState::new([0., 0., 5., 4., 0., 1.], [0, 0, 4, 4], [0.; 4], 0);
    d.passes[0].draws.push(invalid);
    d.bindings.push(vec![]);
    assert_eq!(d.build_contextual().unwrap_err(), Error::State);
}
#[test]
fn earlier_target_generation_can_be_sampled_while_new_generation_is_written() {
    let mut d = Draft::new();
    d.read(1, v(1, 1));
    let f = d.build().unwrap();
    assert_eq!(f.targets()[0].version.id, f.targets()[2].version.id);
    assert_ne!(f.targets()[0].version.generation, f.targets()[2].version.generation);
}
#[test]
fn feedback_and_future_generation_reads_are_refused() {
    for (pass, version) in [(0, v(1, 1)), (0, v(1, 2)), (1, v(1, 2))] {
        let mut d = Draft::new();
        d.read(pass, version);
        assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    }
}
#[test]
fn discarded_color_and_depth_versions_cannot_be_loaded_or_returned() {
    for depth in [false, true] {
        let mut d = Draft::new();
        if depth {
            d.render(0).store_depth = false
        } else {
            d.render(0).store_color = false
        }
        assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    }
    let mut d = Draft::new();
    d.render(1).store_color = false;
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.render(1).store_depth = false;
    d.build().unwrap(); // dead discarded depth is legal; only later reads are forbidden.
}
#[test]
fn discarded_images_cannot_be_sampled_or_copied_even_if_cleared() {
    let mut d = Draft::new();
    d.render(0).store_color = false;
    d.passes[1].clear_color = Some([0.; 4]);
    d.render(1).color_load = None;
    d.read(1, v(1, 1));
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.render(1).store_color = false;
    d.targets.push(target(v(3, 1), Kind::Color));
    d.ops.push(Operation::Copy(Copy {
        source: ImageSlice { version: v(1, 2), mip_level: 0 },
        origin: [0, 0],
        destination: v(3, 1),
    }));
    d.output = v(3, 1);
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
}
#[test]
fn clear_and_load_are_mutually_exclusive_and_a_load_must_exist() {
    let mut d = Draft::new();
    d.render(0).color_load = Some(v(1, 2));
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.render(1).color_load = None;
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.render(1).depth_load = None;
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
}
#[test]
fn load_requires_stored_target_of_same_kind_and_extent() {
    let mut d = Draft::new();
    d.render(1).depth_load = Some(v(1, 1));
    assert_eq!(d.build().unwrap_err(), Error::State);
    let mut d = Draft::new();
    d.render(0).color_load = Some(v(1, 2));
    d.passes[0].clear_color = None;
    // The unchanged base contract also refuses an uninitialized first pass.
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.targets[0].width = 3;
    assert_eq!(d.build().unwrap_err(), Error::State);
}
#[test]
fn target_and_input_identity_collisions_and_zero_generations_refuse() {
    let mut d = Draft::new();
    d.targets[2].version = v(1, 1);
    assert_eq!(d.build().unwrap_err(), Error::Identity);
    let mut d = Draft::new();
    d.targets[0].version.generation = 0;
    assert_eq!(d.build().unwrap_err(), Error::Identity);
    let mut d = Draft::new();
    d.textures.push(texture());
    d.targets[0].version = v(50, 1);
    assert_eq!(d.build().unwrap_err(), Error::Identity);
}
#[test]
fn every_target_has_one_producer_and_every_pass_runs_once_in_original_order() {
    let mut d = Draft::new();
    d.render(1).color = v(1, 1);
    assert_eq!(d.build().unwrap_err(), Error::Identity);
    let mut d = Draft::new();
    d.ops.swap(0, 1);
    assert_eq!(d.build().unwrap_err(), Error::State);
    let mut d = Draft::new();
    d.ops.pop();
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.targets.push(target(v(3, 1), Kind::Color));
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
}
#[test]
fn partial_color_copy_fully_initializes_destination_and_checks_mip_pitch_extent() {
    let mut d = Draft::new();
    d.textures.push(texture());
    d.targets.push(TargetVersion { version: v(3, 1), kind: Kind::Color, width: 1, height: 2 });
    d.ops.push(Operation::Copy(Copy {
        source: ImageSlice { version: v(50, 1), mip_level: 1 },
        origin: [0, 0],
        destination: v(3, 1),
    }));
    d.output = v(3, 1);
    let f = d.build().unwrap();
    assert_eq!((f.output().width, f.output().height), (1, 2));
    for (mip, origin) in [(1, [1, 0]), (2, [0, 0]), (3, [0, 0]), (0, [u32::MAX, 0])] {
        let mut d = Draft::new();
        d.textures.push(texture());
        d.targets.push(TargetVersion { version: v(3, 1), kind: Kind::Color, width: 1, height: 2 });
        d.ops.push(Operation::Copy(Copy {
            source: ImageSlice { version: v(50, 1), mip_level: mip },
            origin,
            destination: v(3, 1),
        }));
        d.output = v(3, 1);
        assert_eq!(d.build().unwrap_err(), Error::Range);
    }
}
#[test]
fn copies_keep_source_generation_available_and_forbid_rewriting_destination() {
    let mut d = Draft::new();
    d.targets.push(target(v(1, 3), Kind::Color));
    let op = Operation::Copy(Copy {
        source: ImageSlice { version: v(1, 1), mip_level: 0 },
        origin: [0, 0],
        destination: v(1, 3),
    });
    d.ops.insert(1, op);
    d.read(1, v(1, 3));
    d.output = v(1, 1);
    d.build().unwrap();
    let mut d = Draft::new();
    d.ops.push(Operation::Copy(Copy {
        source: ImageSlice { version: v(1, 1), mip_level: 0 },
        origin: [0, 0],
        destination: v(1, 2),
    }));
    assert_eq!(d.build().unwrap_err(), Error::Identity);
}
#[test]
fn depth_can_only_be_copied_in_full_and_cannot_be_sampled_as_color_or_returned() {
    let mut d = Draft::new();
    d.read(1, v(2, 1));
    assert_eq!(d.build().unwrap_err(), Error::Binding);
    let mut d = Draft::new();
    d.output = v(2, 2);
    assert_eq!(d.build().unwrap_err(), Error::Uninitialized);
    let mut d = Draft::new();
    d.targets.push(target(v(2, 3), Kind::Depth));
    d.ops.push(Operation::Copy(Copy {
        source: ImageSlice { version: v(2, 1), mip_level: 0 },
        origin: [0, 0],
        destination: v(2, 3),
    }));
    d.build().unwrap();
    let mut d = Draft::new();
    d.targets.push(TargetVersion { version: v(2, 3), kind: Kind::Depth, width: 2, height: 4 });
    d.ops.push(Operation::Copy(Copy {
        source: ImageSlice { version: v(2, 1), mip_level: 0 },
        origin: [0, 0],
        destination: v(2, 3),
    }));
    assert_eq!(d.build().unwrap_err(), Error::Range);
}
#[test]
fn unknown_versions_and_binding_aliases_refuse_before_recording() {
    let mut d = Draft::new();
    d.read(1, v(33, 1));
    assert_eq!(d.build().unwrap_err(), Error::MissingVersion);
    let mut d = Draft::new();
    d.read(1, v(1, 1));
    let binding = d.bindings[0][0];
    d.bindings[0].push(binding);
    assert_eq!(d.build().unwrap_err(), Error::Binding);
    let mut d = Draft::new();
    d.read(1, v(1, 1));
    d.bindings[0][0].resource = sf::Resource::Sampler(v(5, 1));
    assert_eq!(d.build().unwrap_err(), Error::MissingVersion);
}
#[test]
fn target_allocation_and_extent_caps_apply_without_allocating_gpu_images() {
    let mut d = Draft::new();
    d.targets[0].width = 0;
    assert_eq!(d.build().unwrap_err(), Error::Limit);
    let mut d = Draft::new();
    d.targets[0].height = 1025;
    assert_eq!(d.build().unwrap_err(), Error::Limit);
    let mut d = Draft::new();
    d.targets =
        (0..17).map(|i| TargetVersion { version: v(i, 10), kind: Kind::Color, width: 1024, height: 1024 }).collect();
    assert_eq!(d.build().unwrap_err(), Error::Limit);
    let mut d = Draft::new();
    d.ops = vec![d.ops[0]; 65];
    assert_eq!(d.build().unwrap_err(), Error::Limit);
}
