use super::*;
use crate::{frame_plan as fp, prepared::DrawState};
fn v(id: u32, generation: u64) -> Version {
    Version { id, generation }
}
fn geometry(count: usize) -> FramePlan {
    FramePlan::new(
        4,
        4,
        vec![],
        vec![fp::Pass {
            clear_color: Some([0., 0., 0., 1.]),
            clear_depth: Some(1.),
            draws: (0..count)
                .map(|_| fp::Draw {
                    state: DrawState::new([0., 0., 4., 4., 0., 1.], [0, 0, 4, 4], [0.; 4], 0),
                    topology: fp::Topology::Triangles,
                    count: 3,
                    indices: None,
                    bindings: vec![],
                    blend: false,
                    depth_less_write: false,
                })
                .collect(),
        }],
    )
    .unwrap()
}
fn texture() -> TextureVersion {
    TextureVersion {
        version: v(1, 1),
        width: 2,
        height: 2,
        mips: vec![Mip { bytes_per_row: 12, bytes: Arc::from([1u8; 24]) }],
    }
}
fn sampler() -> SamplerVersion {
    SamplerVersion { version: v(2, 1), state: SamplerState::default() }
}
fn bindings() -> Vec<Binding> {
    vec![
        Binding { binding: 0, resource: Resource::Texture(v(1, 1)) },
        Binding { binding: 1, resource: Resource::Sampler(v(2, 1)) },
    ]
}
#[test]
fn standalone_sampled_contract_still_requires_cpu_texture_and_sampler() {
    assert!(SampledFrame::new(geometry(0), vec![], vec![], vec![]).is_err());
    assert!(SampledFrame::new(geometry(0), vec![texture()], vec![], vec![]).is_err());
    assert!(SampledFrame::new(geometry(0), vec![], vec![sampler()], vec![]).is_err());
}
#[test]
fn standalone_sampled_contract_refuses_external_first_load_with_valid_resources() {
    let mut passes = geometry(1).passes().to_vec();
    passes[0].clear_color = None;
    let contextual = FramePlan::target_geometry(4, 4, vec![], passes).unwrap();
    assert_eq!(
        SampledFrame::new(contextual, vec![texture()], vec![sampler()], vec![bindings()]).unwrap_err(),
        Error::Uninitialized,
    );
}
fn word(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

#[test]
fn canonical_packet_preserves_padding_and_mip_payload() {
    let f = SampledFrame::new(geometry(1), vec![texture()], vec![sampler()], vec![bindings()]).unwrap();
    let p = f.packet();
    assert_eq!(p, f.packet());
    assert_eq!(&p[..8], b"OHST0001");
    assert_eq!(word(&p, 12) as usize, p.len());
    assert_eq!(word(&p, 16), 80);
    let mo = word(&p, 44) as usize;
    let start = word(&p, mo + 12) as usize;
    assert_eq!(word(&p, mo + 8), 12);
    assert_eq!(&p[start..start + 24], &[1u8; 24]);
    assert_eq!(&p[80..88], b"OHFP0001");
}
#[test]
fn texture_versions_and_sampler_versions_keep_earlier_draws_immutable() {
    let a = texture();
    let mut b = a.clone();
    b.version.generation = 2;
    b.mips[0].bytes = Arc::from([2u8; 24]);
    let s = sampler();
    let mut later = s.clone();
    later.version.generation = 2;
    later.state.address[0] = AddressMode::Repeat;
    let mut last = bindings();
    last[0].resource = Resource::Texture(b.version);
    last[1].resource = Resource::Sampler(later.version);
    let f = SampledFrame::new(geometry(2), vec![a, b], vec![s, later], vec![bindings(), last]).unwrap();
    assert_eq!(f.textures()[0].mips[0].bytes[0], 1);
    assert_eq!(f.textures()[1].mips[0].bytes[0], 2);
    assert_eq!(f.samplers()[0].state.address[0], AddressMode::ClampToEdge);
    assert_eq!(f.samplers()[1].state.address[0], AddressMode::Repeat);
}
#[test]
fn invalid_mip_pitch_size_and_level_count_are_refused() {
    let base = texture();
    for (width, height, pitch, n) in
        [(0, 2, 8, 16), (2, 1025, 8, 8200), (2, 2, 7, 14), (2, 2, 4, 8), (2, 2, 8, 15), (2, 2, u32::MAX, 0)]
    {
        let mut t = base.clone();
        t.width = width;
        t.height = height;
        t.mips[0] = Mip { bytes_per_row: pitch, bytes: Arc::from(vec![0; n]) };
        assert!(t.validate().is_err());
    }
    let mut t = base;
    t.mips = vec![];
    assert_eq!(t.validate(), Err(Error::Limit));
    t.mips = vec![Mip { bytes_per_row: 8, bytes: Arc::from([0; 16]) }; 3];
    assert_eq!(t.validate(), Err(Error::Limit));
}
#[test]
fn odd_dimensions_mip_rows_are_not_rounded_to_base_size() {
    let t = TextureVersion {
        version: v(1, 1),
        width: 3,
        height: 5,
        mips: vec![
            Mip { bytes_per_row: 16, bytes: Arc::from([0; 80]) },
            Mip { bytes_per_row: 8, bytes: Arc::from([0; 16]) },
            Mip { bytes_per_row: 4, bytes: Arc::from([0; 4]) },
        ],
    };
    t.validate().unwrap();
    SampledFrame::new(geometry(1), vec![t], vec![sampler()], vec![bindings()]).unwrap();
}
#[test]
fn nonfinite_sampler_and_incompatible_anisotropy_refuse() {
    for lod in [[f32::NAN, 1.], [0., f32::INFINITY], [-1., 0.], [2., 1.], [0., 33.]] {
        assert_eq!(SamplerState { lod, ..Default::default() }.validate(), Err(Error::State));
    }
    for anisotropy in [0, 3, 17, 65535] {
        assert!(SamplerState { anisotropy, ..Default::default() }.validate().is_err());
    }
    assert!(SamplerState { anisotropy: 2, ..Default::default() }.validate().is_err());
    SamplerState { anisotropy: 16, filters: [FilterMode::Linear; 3], ..Default::default() }.validate().unwrap();
}
#[test]
fn duplicate_missing_versions_and_cross_kind_binding_aliases_refuse() {
    let f = |ts, ss, bs| SampledFrame::new(geometry(1), ts, ss, bs);
    assert_eq!(f(vec![texture(), texture()], vec![sampler()], vec![bindings()]).unwrap_err(), Error::Identity);
    assert_eq!(f(vec![texture()], vec![sampler(), sampler()], vec![bindings()]).unwrap_err(), Error::Identity);
    let mut b = bindings();
    b[0].resource = Resource::Texture(v(1, 2));
    assert_eq!(f(vec![texture()], vec![sampler()], vec![b]).unwrap_err(), Error::MissingVersion);
    let mut b = bindings();
    b[1].binding = 0;
    assert_eq!(f(vec![texture()], vec![sampler()], vec![b]).unwrap_err(), Error::Binding);
    let mut b = bindings();
    b[0].resource = Resource::Sampler(v(1, 1));
    assert_eq!(f(vec![texture()], vec![sampler()], vec![b]).unwrap_err(), Error::MissingVersion);
    assert_eq!(f(vec![texture()], vec![sampler()], vec![]).unwrap_err(), Error::Limit);
}
#[test]
fn aggregate_packet_budget_includes_texture_bytes() {
    let t = TextureVersion {
        version: v(1, 1),
        width: 1024,
        height: 1024,
        mips: vec![Mip { bytes_per_row: 4096, bytes: Arc::from(vec![0; MAX_PACKET]) }],
    };
    assert_eq!(SampledFrame::new(geometry(1), vec![t], vec![sampler()], vec![bindings()]).unwrap_err(), Error::Limit);
}
#[test]
fn existing_cpu_loader_and_sampler_mapping_are_reused_without_gamma_or_bias_duplication() {
    use xenos_texture::*;
    let mut fetch = TextureFetchConstant::new_2d(TextureFormat::k_8_8_8_8, 2, 2);
    fetch.base_page = 1;
    fetch.tiled = false;
    let mut mem = vec![0u8; 65536];
    mem[4096..].fill(127);
    let loaded = load_texture(&fetch, &mem, &LoadOptions::default()).unwrap();
    let original = loaded.levels[0].data.clone();
    let t = TextureVersion::from_loaded(v(1, 1), loaded.clone()).unwrap();
    assert_eq!(&*t.mips[0].bytes, original);
    let mut bad = loaded.clone();
    bad.mapping.host_format = HostFormat::Rgba8UnormSrgb;
    assert!(TextureVersion::from_loaded(v(1, 1), bad).is_err());
    let mut bad = loaded.clone();
    bad.levels.clear();
    assert!(TextureVersion::from_loaded(v(1, 1), bad).is_err());
    let mut bad = loaded;
    bad.mapping.component_map[0] = HostChannel::A;
    assert!(TextureVersion::from_loaded(v(1, 1), bad).is_err());
    fetch.lod_bias = 16;
    let mut host = sampler_descriptor(&fetch, &FetchInstructionFilters::default());
    let state = SamplerState::from_host(&host).unwrap();
    assert_eq!(host.lod_bias, 0.5);
    assert_eq!(state.lod[0], host.lod_min_clamp);
    host.losses.push(SamplerLoss::BorderClampToEdge { axis: 0 });
    assert!(SamplerState::from_host(&host).is_err());
}
