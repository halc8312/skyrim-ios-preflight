//! Self-authored std/panic-containing staticlib, not the Skyrim runtime.
#[unsafe(no_mangle)]
pub extern "C" fn public_core_coexistence_probe() -> u64 {
    std::panic::catch_unwind(|| std::hint::black_box(vec![1u64, 2, 3, 4]).into_iter().sum())
        .unwrap_or(0)
}
