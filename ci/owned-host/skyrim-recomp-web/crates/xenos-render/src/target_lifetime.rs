//! Frame-local physical reuse for an already sealed, sequential target graph.
//! Requires tracked hazards / ordered pass and copy boundaries in one submission.
//! Does not retire or reuse storage across asynchronous submissions or frames.
use super::*;

/// Fixed-size native receipt for the allocation actually used by a completed
/// authored render. No GPU handles or driver-memory-size claim.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AllocationAudit {
    pub version: u32,
    pub bytes: u32,
    pub policy: u32,
    pub slots: u32,
    pub targets: u32,
    pub operations: u32,
    pub reserved0: u32,
    pub reserved1: u32,
    pub dedicated_bytes: u64,
    pub allocated_bytes: u64,
    pub color_load_elided: u64,
    pub depth_load_elided: u64,
    pub copy_elided: u64,
    pub mapping: [u32; MAX_TARGETS],
}
const _: () = assert!(std::mem::size_of::<AllocationAudit>() == 200);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Slot {
    pub kind: Kind,
    pub width: u32,
    pub height: u32,
}
impl Slot {
    fn of(t: &TargetVersion) -> Self {
        Self { kind: t.kind, width: t.width, height: t.height }
    }
}
#[derive(Debug)]
pub struct AllocationPlan {
    reuse: bool,
    slots: Vec<Slot>,
    mapping: Vec<usize>,
    load_elided: Vec<[bool; 2]>,
    copy_elided: Vec<bool>,
    dedicated_bytes: u64,
}
impl AllocationPlan {
    pub fn expected_audit(&self) -> AllocationAudit {
        let mut audit = AllocationAudit {
            version: 0x10000,
            bytes: 200,
            policy: u32::from(self.reuse),
            slots: self.slots.len() as u32,
            targets: self.mapping.len() as u32,
            operations: self.copy_elided.len() as u32,
            dedicated_bytes: self.dedicated_bytes,
            allocated_bytes: self.allocated_bytes(),
            ..Default::default()
        };
        for (a, slot) in audit.mapping.iter_mut().zip(&self.mapping) {
            *a = *slot as u32;
        }
        for (i, (load, copy)) in self.load_elided.iter().zip(&self.copy_elided).enumerate() {
            audit.color_load_elided |= u64::from(load[0]) << i;
            audit.depth_load_elided |= u64::from(load[1]) << i;
            audit.copy_elided |= u64::from(*copy) << i;
        }
        audit
    }
    pub fn matches_audit(&self, actual: &AllocationAudit) -> bool {
        *actual == self.expected_audit()
    }
    pub fn slots(&self) -> &[Slot] {
        &self.slots
    }
    pub fn mapping(&self) -> &[usize] {
        &self.mapping
    }
    pub fn load_elided(&self) -> &[[bool; 2]] {
        &self.load_elided
    }
    pub fn copy_elided(&self) -> &[bool] {
        &self.copy_elided
    }
    pub fn dedicated_bytes(&self) -> u64 {
        self.dedicated_bytes
    }
    pub fn allocated_bytes(&self) -> u64 {
        self.slots.iter().map(|s| u64::from(s.width) * u64::from(s.height) * 4).sum()
    }
    pub fn new(frame: &TargetFrame, reuse: bool) -> Self {
        let index = |v| frame.targets().iter().position(|t| t.version == v).expect("sealed target");
        let mut last = vec![0; frame.targets().len()];
        let mut draw = 0;
        for (i, op) in frame.operations().iter().enumerate() {
            match *op {
                Operation::Render(r) => {
                    last[index(r.color)] = i;
                    last[index(r.depth)] = i;
                    for source in [r.color_load, r.depth_load].into_iter().flatten() {
                        last[index(source)] = i;
                    }
                    for _ in &frame.geometry().passes()[r.pass as usize].draws {
                        for b in &frame.bindings()[draw] {
                            if let sf::Resource::Texture(v) = b.resource {
                                if let Some(j) = frame.targets().iter().position(|t| t.version == v) {
                                    last[j] = i;
                                }
                            }
                        }
                        draw += 1;
                    }
                }
                Operation::Copy(c) => {
                    last[index(c.destination)] = i;
                    if let Some(j) = frame.targets().iter().position(|t| t.version == c.source.version) {
                        last[j] = i;
                    }
                }
            }
        }
        last[index(frame.output().version)] = frame.operations().len();
        let mut result = Self {
            reuse,
            slots: vec![],
            mapping: vec![usize::MAX; frame.targets().len()],
            load_elided: vec![[false; 2]; frame.operations().len()],
            copy_elided: vec![false; frame.operations().len()],
            dedicated_bytes: frame.targets().iter().map(|t| u64::from(t.width) * u64::from(t.height) * 4).sum(),
        };
        let mut owners: Vec<usize> = Vec::new();
        draw = 0;
        for (i, op) in frame.operations().iter().enumerate() {
            let mut allocate = |destination: Version, load: Option<Version>, can_transfer: bool| {
                let destination = index(destination);
                let descriptor = Slot::of(&frame.targets()[destination]);
                // A terminal load can transfer its slot only when the old image
                // is not also sampled by this pass (which would be feedback).
                let transfer =
                    load.filter(|v| reuse && can_transfer && last[index(*v)] == i).map(|v| result.mapping[index(v)]);
                let slot = transfer
                    .or_else(|| {
                        if reuse {
                            result
                                .slots
                                .iter()
                                .enumerate()
                                .find(|(slot, s)| **s == descriptor && last[owners[*slot]] < i)
                                .map(|(slot, _)| slot)
                        } else {
                            None
                        }
                    })
                    .unwrap_or_else(|| {
                        result.slots.push(descriptor);
                        owners.push(destination);
                        result.slots.len() - 1
                    });
                debug_assert_eq!(result.slots[slot], descriptor);
                result.mapping[destination] = slot;
                owners[slot] = destination;
                transfer.is_some()
            };
            match *op {
                Operation::Render(r) => {
                    let count = frame.geometry().passes()[r.pass as usize].draws.len();
                    let sampled = |v| {
                        frame.bindings()[draw..draw + count]
                            .iter()
                            .flatten()
                            .any(|b| matches!(b.resource,sf::Resource::Texture(x) if x==v))
                    };
                    let color = allocate(r.color, r.color_load, !r.color_load.is_some_and(sampled));
                    let depth = allocate(r.depth, r.depth_load, true);
                    result.load_elided[i] = [color, depth];
                    draw += count;
                }
                Operation::Copy(c) => {
                    let source = frame.targets().iter().find(|t| t.version == c.source.version);
                    let target = frame.target(c.destination).expect("sealed target");
                    let transfer = source
                        .filter(|t| Slot::of(t) == Slot::of(target) && c.origin == [0, 0] && c.source.mip_level == 0)
                        .map(|t| t.version);
                    result.copy_elided[i] = allocate(c.destination, transfer, true);
                }
            }
        }
        debug_assert!(result.mapping.iter().all(|i| *i < result.slots.len()));
        result
    }
}

#[cfg(all(test, feature = "public-probes", feature = "metal-shaders", not(target_os = "emscripten")))]
mod tests {
    use super::*;
    use crate::target_fixtures;
    #[test]
    fn native_allocation_evidence_refuses_dedicated_substitution_and_every_changed_word() {
        for f in target_fixtures::fixtures() {
            for reuse in [false, true] {
                let plan = AllocationPlan::new(&f.frame, reuse);
                let expected = plan.expected_audit();
                assert!(plan.matches_audit(&expected));
                // All fields (including unused mapping entries and reserved
                // words) are evidence, not merely the reported memory total.
                for word in 0..50 {
                    let mut changed = expected;
                    // The repr(C) record is exactly fifty initialized u32
                    // words; all bit patterns are valid integer values.
                    let bytes = unsafe {
                        std::slice::from_raw_parts_mut((&mut changed as *mut AllocationAudit).cast::<u8>(), 200)
                    };
                    bytes[word * 4] ^= 1;
                    assert!(!plan.matches_audit(&changed), "case={} word={word}", f.id);
                }
                if reuse && f.id == 3 {
                    let substituted = AllocationPlan::new(&f.frame, false).expected_audit();
                    assert!(expected.allocated_bytes < expected.dedicated_bytes);
                    assert!(!plan.matches_audit(&substituted));
                }
            }
        }
    }
    #[test]
    fn dedicated_policy_never_aliases_and_reuse_never_increases_storage() {
        for f in target_fixtures::fixtures() {
            let dedicated = AllocationPlan::new(&f.frame, false);
            let reuse = AllocationPlan::new(&f.frame, true);
            assert_eq!(
                dedicated.mapping().iter().copied().collect::<BTreeSet<_>>(),
                (0..f.frame.targets().len()).collect()
            );
            assert_eq!(dedicated.slots().len(), f.frame.targets().len());
            assert!(
                dedicated.copy_elided().iter().all(|v| !*v) && dedicated.load_elided().iter().all(|v| *v == [false; 2])
            );
            assert_eq!(dedicated.allocated_bytes(), dedicated.dedicated_bytes());
            assert!(reuse.allocated_bytes() <= reuse.dedicated_bytes());
        }
    }
    #[test]
    fn same_pass_sampled_source_keeps_its_own_storage() {
        let f = target_fixtures::fixtures().remove(0);
        let a = AllocationPlan::new(&f.frame, true);
        assert_ne!(a.mapping()[0], a.mapping()[2]); // first color sampled by second pass
        assert_eq!(a.mapping()[1], a.mapping()[3]); // dead depth can be reused
    }
    #[test]
    fn live_old_generations_survive_newer_writes() {
        let f = target_fixtures::fixtures().remove(1);
        let a = AllocationPlan::new(&f.frame, true);
        let slots = [a.mapping()[0], a.mapping()[2], a.mapping()[4]];
        assert_eq!(slots.into_iter().collect::<BTreeSet<_>>().len(), 3);
    }
    #[test]
    fn terminal_color_depth_loads_can_transfer_and_skip_copies() {
        let f = target_fixtures::fixtures().remove(2);
        let a = AllocationPlan::new(&f.frame, true);
        assert_eq!(a.load_elided()[1], [true, true]);
        assert_eq!(a.slots().len(), 2);
        assert_eq!(a.allocated_bytes() * 2, a.dedicated_bytes());
    }
    #[test]
    fn full_terminal_copy_transfers_but_subrectangle_copy_does_not() {
        let f = target_fixtures::fixtures().remove(3);
        let a = AllocationPlan::new(&f.frame, true);
        assert!(a.copy_elided()[1]);
        let f = target_fixtures::fixtures().remove(4);
        let a = AllocationPlan::new(&f.frame, true);
        assert!(!a.copy_elided()[1]);
    }
    #[test]
    fn a_load_also_sampled_in_the_same_pass_cannot_transfer_its_slot() {
        let f = target_fixtures::fixtures().remove(12);
        let a = AllocationPlan::new(&f.frame, true);
        assert!(!a.load_elided()[1][0]);
        assert!(a.load_elided()[1][1]);
        assert_ne!(a.mapping()[0], a.mapping()[2]);
    }
}
