//! Export only locally authored ordered target plans and independent RGBA bytes.
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::Path,
};
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    assert_eq!(args.len(), 2);
    let root = Path::new(&args[1]);
    assert!(!root.exists(), "fresh authored-only output required");
    fs::create_dir_all(root).unwrap();
    let fixtures = xenos_render::target_fixtures::fixtures();
    for f in &fixtures {
        let mut lifetime = String::from("authored-target-lifetimes-v1\n");
        for reuse in [false, true] {
            let plan = xenos_render::target_frame::AllocationPlan::new(&f.frame, reuse);
            lifetime.push_str(&format!(
                "{} {} {} {} {}\n",
                plan.slots().len(),
                plan.mapping().len(),
                plan.load_elided().len(),
                plan.dedicated_bytes(),
                plan.allocated_bytes()
            ));
            for s in plan.slots() {
                lifetime.push_str(&format!("{} {} {}\n", s.kind as u32, s.width, s.height));
            }
            for slot in plan.mapping() {
                lifetime.push_str(&format!("{slot}\n"));
            }
            for (load, copy) in plan.load_elided().iter().zip(plan.copy_elided()) {
                lifetime.push_str(&format!("{} {} {}\n", u32::from(load[0]), u32::from(load[1]), u32::from(*copy)));
            }
        }
        for (name, bytes) in [
            (format!("target-{}.bin", f.id), f.frame.packet()),
            (format!("target-{}.rgba", f.id), f.expected.clone()),
            (format!("target-{}.lifetimes", f.id), lifetime.into_bytes()),
        ] {
            OpenOptions::new().write(true).create_new(true).open(root.join(name)).unwrap().write_all(&bytes).unwrap();
        }
    }
    println!("authored target plans={}; no game or GPU execution", fixtures.len());
}
