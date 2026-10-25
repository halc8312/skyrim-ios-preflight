use super::*;

fn resource(generation: u64, bytes: &[u8]) -> BufferVersion {
    BufferVersion { version: Version { id: 1, generation }, bytes: Arc::from(bytes) }
}
fn draw(version: Version) -> Draw {
    Draw {
        state: DrawState::new([0., 0., 4., 4., 0., 1.], [0, 0, 4, 4], [0.; 4], 0),
        topology: Topology::Triangles,
        count: 3,
        indices: None,
        bindings: vec![Binding { group: 0, binding: 0, slice: BufferSlice { version, offset: 0, bytes: 4 } }],
        blend: false,
        depth_less_write: false,
    }
}
fn pass(draws: Vec<Draw>) -> Pass {
    Pass { clear_color: Some([0., 0., 0., 1.]), clear_depth: Some(1.), draws }
}

#[test]
fn immutable_versions_retain_old_bytes_after_source_changes() {
    let mut source = vec![1u8; 4];
    let a = resource(1, &source);
    source.fill(2);
    let b = resource(2, &source);
    source.fill(3);
    let first = draw(a.version);
    let second = draw(b.version);
    let plan = FramePlan::new(4, 4, vec![a, b], vec![pass(vec![first, second])]).unwrap();
    assert_eq!(plan.buffer(plan.passes()[0].draws[0].bindings[0].slice).unwrap(), [1; 4]);
    assert_eq!(plan.buffer(plan.passes()[0].draws[1].bindings[0].slice).unwrap(), [2; 4]);
    let packet = plan.packet();
    assert_eq!(&packet[..8], b"OHFP0001");
    assert_eq!(u32::from_le_bytes(packet[56..60].try_into().unwrap()) as usize, packet.len());
    assert_eq!(packet, plan.packet());
}

#[test]
fn version_mismatch_duplicates_and_uninitialized_load_are_refused() {
    let a = resource(1, &[0; 4]);
    let mut missing = draw(a.version);
    missing.bindings[0].slice.version.generation = 2;
    assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![missing])]).unwrap_err(), Error::MissingVersion);
    assert_eq!(FramePlan::new(4, 4, vec![a.clone(), a.clone()], vec![pass(vec![])]).unwrap_err(), Error::Identity);
    let mut p = pass(vec![]);
    p.clear_color = None;
    assert_eq!(FramePlan::new(4, 4, vec![a], vec![p]).unwrap_err(), Error::Uninitialized);
}

#[test]
fn standalone_first_clear_is_required_for_both_attachments_and_context_is_explicit() {
    for (color, depth) in [(None, Some(1.)), (Some([0.; 4]), None), (None, None)] {
        let p = Pass { clear_color: color, clear_depth: depth, draws: vec![] };
        assert_eq!(FramePlan::new(4, 4, vec![], vec![p.clone()]).unwrap_err(), Error::Uninitialized);
        let geometry = FramePlan::target_geometry(4, 4, vec![], vec![p]).unwrap();
        assert_eq!(geometry.require_first_clear(), Err(Error::Uninitialized));
    }
    assert_eq!(FramePlan::target_geometry(4, 4, vec![], vec![]).unwrap_err(), Error::Limit);
    let p = Pass { clear_color: None, clear_depth: Some(f64::NAN), draws: vec![] };
    assert_eq!(FramePlan::target_geometry(4, 4, vec![], vec![p]).unwrap_err(), Error::State);
}

#[test]
fn out_of_range_buffers_scissors_and_nonfinite_state_are_refused() {
    let a = resource(1, &[0; 8]);
    let base = draw(a.version);
    let mut bad = base.clone();
    bad.bindings[0].slice.offset = u32::MAX;
    assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![bad])]).unwrap_err(), Error::Range);
    let mut bad = base.clone();
    bad.state.scissor = [u32::MAX, 0, 2, 2];
    assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![bad])]).unwrap_err(), Error::State);
    let mut bad = base.clone();
    bad.state.viewport_bits[2] = f32::NAN.to_bits();
    assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![bad])]).unwrap_err(), Error::State);
    let mut bad = base.clone();
    bad.bindings.push(bad.bindings[0]);
    assert_eq!(FramePlan::new(4, 4, vec![a], vec![pass(vec![bad])]).unwrap_err(), Error::Binding);
}

#[test]
fn ordered_passes_keep_clear_once_and_empty_clear_pass() {
    let a = resource(1, &[0; 4]);
    let later = Pass { clear_color: None, clear_depth: None, draws: vec![draw(a.version)] };
    let plan = FramePlan::new(4, 4, vec![a], vec![pass(vec![]), later]).unwrap();
    let bytes = plan.packet();
    let po = u32::from_le_bytes(bytes[40..44].try_into().unwrap()) as usize;
    let word = |o| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap());
    assert_eq!(word(po + 44), 0);
    assert_eq!(word(po + 48), 3);
    assert_eq!(word(po + 64 + 44), 1);
    assert_eq!(word(po + 64 + 48), 0);
}

#[test]
fn index_width_and_count_must_fit_captured_slice() {
    let a = resource(1, &[0; 8]);
    let mut d = draw(a.version);
    d.indices = Some(IndexSlice { slice: BufferSlice { version: a.version, offset: 0, bytes: 8 }, stride: 4 });
    assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![d.clone()])]).unwrap_err(), Error::Range);
    d.indices.as_mut().unwrap().stride = 2;
    assert!(FramePlan::new(4, 4, vec![a], vec![pass(vec![d])]).is_ok());
}

#[test]
fn shared_offsets_supported_groups_and_index_slice_survive_the_packet() {
    let a = resource(1, &[0; 512]);
    let mut d = draw(a.version);
    d.bindings[0].slice.offset = 256;
    d.indices = Some(IndexSlice { slice: BufferSlice { version: a.version, offset: 4, bytes: 6 }, stride: 2 });
    let plan = FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![d.clone()])]).unwrap();
    let bytes = plan.packet();
    let draw_at = u32::from_le_bytes(bytes[44..48].try_into().unwrap()) as usize;
    assert_eq!(u32::from_le_bytes(bytes[draw_at + 108..draw_at + 112].try_into().unwrap()), 4);
    assert_eq!(u32::from_le_bytes(bytes[draw_at + 112..draw_at + 116].try_into().unwrap()), 6);
    for offset in [4, 16, 128] {
        let mut bad = d.clone();
        bad.bindings[0].slice.offset = offset;
        assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![bad])]).unwrap_err(), Error::Range);
    }
    for (group, binding) in [(2, 0), (0, 5)] {
        let mut bad = d.clone();
        bad.bindings[0].group = group;
        bad.bindings[0].binding = binding;
        assert_eq!(FramePlan::new(4, 4, vec![a.clone()], vec![pass(vec![bad])]).unwrap_err(), Error::Binding);
    }
    let mut bad = d;
    bad.indices.as_mut().unwrap().slice.offset = 2;
    assert_eq!(FramePlan::new(4, 4, vec![a], vec![pass(vec![bad])]).unwrap_err(), Error::Range);
}
