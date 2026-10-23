//! Validated startup settings. Existing renderers never change their EDRAM layout.
use std::sync::Mutex;
type Check<T> = Result<T, &'static str>;

pub const VERSION: u32 = 0x0001_0000;
pub const VIDEO_MODES: [(u32, u32); 4] = [(640, 360), (960, 540), (1280, 720), (1920, 1080)];
pub fn parse_video_mode(text: &str) -> Check<(u32, u32)> {
    VIDEO_MODES.into_iter().find(|(width, height)| text == format!("{width}x{height}"))
        .ok_or("unsupported video mode spelling")
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OHBootGraphicsV1 {
    pub version: u32,
    pub struct_bytes: u32,
    pub video_width: u32,
    pub video_height: u32,
    pub internal_scale: u32,
    pub mirror_mib: u32,
    pub mobile_texture_budget: u32,
    pub reserved: u32,
}
impl Default for OHBootGraphicsV1 {
    fn default() -> Self {
        Self { version: VERSION, struct_bytes: 32, video_width: 1280, video_height: 720,
               internal_scale: 1, mirror_mib: 0, mobile_texture_budget: 0, reserved: 0 }
    }
}
impl OHBootGraphicsV1 {
    pub fn validate(self) -> Check<Self> {
        if self.version != VERSION || self.struct_bytes != 32 || self.reserved != 0
            || !VIDEO_MODES.contains(&(self.video_width, self.video_height))
            || !(1..=3).contains(&self.internal_scale)
            || self.video_width * self.internal_scale > 3840
            || self.video_height * self.internal_scale > 2160
            || ![0, 64, 128, 192, 256].contains(&self.mirror_mib)
            || self.mobile_texture_budget > 1
        { return Err("unsupported startup graphics tuple"); }
        Ok(self)
    }
    pub fn packed_video_size(self) -> u32 { (self.video_width << 16) | self.video_height }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OHPresentationSettingsV1 {
    pub version: u32,
    pub struct_bytes: u32,
    pub frame_limit: u32,
    pub output_scale_percent: u32,
    pub filter: u32,
    pub sharpness: u32,
    pub reserved0: u32,
    pub reserved1: u32,
}
impl OHPresentationSettingsV1 {
    pub fn words(self) -> Check<[u32; 4]> {
        if self.version != VERSION || self.struct_bytes != 32 || self.reserved0 != 0 || self.reserved1 != 0 {
            return Err("presentation settings header refused");
        }
        Ok([self.frame_limit, self.output_scale_percent, self.filter, self.sharpness])
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct OHGraphicsStatusV1 {
    pub version: u32, pub struct_bytes: u32,
    pub video_width: u32, pub video_height: u32, pub internal_scale: u32,
    pub mirror_mib: u32, pub mobile_texture_budget: u32, pub boot_frozen: u32,
    pub requested_frame_limit: u32, pub requested_output_scale_percent: u32,
    pub requested_filter: u32, pub requested_sharpness: u32,
    pub applied_frame_limit: u32, pub applied_output_scale_percent: u32,
    pub applied_filter: u32, pub applied_sharpness: u32,
    pub requested_generation: u32, pub applied_generation: u32,
    pub output_width: u32, pub output_height: u32, pub source_width: u32, pub source_height: u32,
    pub presented_frames: u64, pub skipped_frames: u64, pub decoded_frames: u64, pub reused_frames: u64,
}

pub struct BootConfiguration(Mutex<(OHBootGraphicsV1, bool)>);
impl Default for BootConfiguration {
    fn default() -> Self { Self(Mutex::new((OHBootGraphicsV1::default(), false))) }
}
impl BootConfiguration {
    pub fn configure(&self, value: OHBootGraphicsV1) -> Check<()> {
        let valid = value.validate()?;
        let mut state = self.0.lock().map_err(|_| "startup settings lock unavailable")?;
        if state.1 { return Err("startup graphics are already frozen"); }
        state.0 = valid;
        Ok(())
    }
    pub fn freeze(&self) -> Check<OHBootGraphicsV1> {
        let mut state = self.0.lock().map_err(|_| "startup settings lock unavailable")?;
        if state.1 { return Err("startup graphics were already consumed"); }
        state.1 = true;
        Ok(state.0)
    }
    pub fn snapshot(&self) -> Check<(OHBootGraphicsV1, bool)> {
        self.0.lock().map(|state| *state).map_err(|_| "startup settings lock unavailable")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn startup_tuple_bounds_preserve_original_and_admit_bounded_supersampling() {
        let original = OHBootGraphicsV1::default();
        assert_eq!(original.validate(), Ok(original));
        assert_eq!(original.packed_video_size(), (1280 << 16) | 720);
        for (width, height) in VIDEO_MODES {
            for scale in 1..=3 {
                let value = OHBootGraphicsV1 { video_width: width, video_height: height, internal_scale: scale, ..original };
                assert_eq!(value.validate().is_ok(), width * scale <= 3840 && height * scale <= 2160);
            }
        }
        for value in [OHBootGraphicsV1 { video_width: 0, ..original },
                      OHBootGraphicsV1 { video_height: 721, ..original },
                      OHBootGraphicsV1 { internal_scale: u32::MAX, ..original },
                      OHBootGraphicsV1 { mirror_mib: 63, ..original },
                      OHBootGraphicsV1 { mobile_texture_budget: 2, ..original },
                      OHBootGraphicsV1 { reserved: 1, ..original },
                      OHBootGraphicsV1 { struct_bytes: 31, ..original },
                      OHBootGraphicsV1 { version: 0, ..original }] { assert!(value.validate().is_err()); }
    }
    #[test]
    fn frozen_configuration_cannot_be_replaced_or_consumed_twice() {
        let config = BootConfiguration::default();
        let selected = OHBootGraphicsV1 { video_width: 960, video_height: 540, mirror_mib: 128,
                                         mobile_texture_budget: 1, ..Default::default() };
        config.configure(selected).unwrap();
        assert_eq!(config.freeze().unwrap(), selected);
        assert!(config.configure(OHBootGraphicsV1::default()).is_err());
        assert!(config.freeze().is_err());
        assert_eq!(config.snapshot().unwrap(), (selected, true));
    }
    #[test]
    fn native_settings_layout_and_reserved_fields_are_exact() {
        use std::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<OHBootGraphicsV1>(), 32);
        assert_eq!(size_of::<OHPresentationSettingsV1>(), 32);
        assert_eq!(size_of::<OHGraphicsStatusV1>(), 120);
        assert_eq!(align_of::<OHGraphicsStatusV1>(), 8);
        assert_eq!(offset_of!(OHGraphicsStatusV1, presented_frames), 88);
        let value = OHPresentationSettingsV1 { version: VERSION, struct_bytes: 32,
            frame_limit: 30, output_scale_percent: 100, filter: 0, sharpness: 0, reserved0: 0, reserved1: 0 };
        assert_eq!(value.words().unwrap(), [30, 100, 0, 0]);
        assert!(OHPresentationSettingsV1 { reserved1: 1, ..value }.words().is_err());
    }
    #[test]
    fn video_mode_text_is_a_fixed_canonical_whitelist() {
        for (width, height) in VIDEO_MODES {
            assert_eq!(parse_video_mode(&format!("{width}x{height}")), Ok((width, height)));
        }
        for bad in ["", "0x0", "0640x0360", "640X360", "1280x720\n", "1920x1081", "999999999x1"] {
            assert!(parse_video_mode(bad).is_err());
        }
    }
}
