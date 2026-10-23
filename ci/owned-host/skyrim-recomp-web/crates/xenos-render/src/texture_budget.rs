//! Startup-only texture policies. Native iOS uses bounded residency and existing
//! upload backpressure rather than substituting zero textures at a byte rate.
//! These limits are not an estimate of device RAM or an OS memory entitlement.
use std::time::Duration;
use xenos_texture_cache::CacheConfig;

pub(super) fn for_executor(base: CacheConfig, handheld: bool) -> CacheConfig {
    for_target(base, handheld, cfg!(all(target_os = "ios", target_arch = "aarch64")))
}

fn for_target(base: CacheConfig, handheld: bool, native_ios_arm64: bool) -> CacheConfig {
    if !handheld {
        return base;
    }
    CacheConfig {
        max_unused_frames: 120,
        max_unused_time: Duration::from_secs(4),
        max_host_bytes: if native_ios_arm64 { 128 << 20 } else { 56 << 20 },
        max_live_bytes: if native_ios_arm64 { 160 << 20 } else { 80 << 20 },
        pool_max_bytes: 16 << 20,
        // The browser rate cap can return transparent black until a later frame.
        // Native keeps the existing 32MiB in-flight upload backpressure instead.
        max_load_bytes_per_second: if native_ios_arm64 { u64::MAX } else { super::HANDHELD_LOAD_RATE },
        ..base
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn non_policy_options() -> CacheConfig {
        CacheConfig {
            pool_max_time: Duration::from_millis(1234),
            allow_invalid_fetch_constants: true,
            gpu_conversion: false,
            reuse_textures: false,
            ..CacheConfig::default()
        }
    }

    #[test]
    fn desktop_preserves_all_supplied_options_on_every_target() {
        let base = CacheConfig {
            max_unused_frames: 37,
            max_unused_time: Duration::from_secs(9),
            max_host_bytes: 7 << 20,
            max_live_bytes: 11 << 20,
            pool_max_bytes: 3 << 20,
            max_load_bytes_per_second: 12345,
            ..non_policy_options()
        };
        assert_eq!(for_target(base, false, false), base);
        assert_eq!(for_target(base, false, true), base);
        let default = CacheConfig::default();
        assert_eq!(for_target(default, false, true), default);
        assert_eq!(
            (default.max_host_bytes, default.max_live_bytes, default.pool_max_bytes),
            (512 << 20, u64::MAX, 256 << 20)
        );
    }

    #[test]
    fn browser_handheld_preserves_the_existing_full_policy() {
        let base = non_policy_options();
        assert_eq!(
            for_target(base, true, false),
            CacheConfig {
                max_unused_frames: 120,
                max_unused_time: Duration::from_secs(4),
                max_host_bytes: 56 << 20,
                max_live_bytes: 80 << 20,
                pool_max_bytes: 16 << 20,
                max_load_bytes_per_second: 16 << 20,
                ..base
            }
        );
    }

    #[test]
    fn native_handheld_matches_the_measured_profile_without_rate_fallback() {
        let base = non_policy_options();
        assert_eq!(
            for_target(base, true, true),
            CacheConfig {
                max_unused_frames: 120,
                max_unused_time: Duration::from_secs(4),
                max_host_bytes: 128 << 20,
                max_live_bytes: 160 << 20,
                pool_max_bytes: 16 << 20,
                max_load_bytes_per_second: u64::MAX,
                ..base
            }
        );
        assert_eq!(for_target(CacheConfig::default(), true, true).pool_max_time, Duration::from_secs(2));
    }

    #[test]
    fn constructor_uses_only_the_compiled_platform_and_explicit_handheld_choice() {
        let base = non_policy_options();
        assert_eq!(for_executor(base, false), base);
        let native = cfg!(all(target_os = "ios", target_arch = "aarch64"));
        assert_eq!(for_executor(base, true), for_target(base, true, native));
        assert_eq!(super::super::HANDHELD_UPLOAD_BUDGET, 32 << 20);
    }
}
