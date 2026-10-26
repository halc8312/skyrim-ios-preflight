use super::{finish, open, setup};
use skyrim_native_gpu::{edram::targets::TargetOptions, render::ColorAttachment, types};
use xenos_state::{
    edram::{
        RenderTargetBinding as Binding, RenderTargetKey as Key, RenderTargetSetup as Setup,
        RenderTargetSlot as Slot,
    },
    xenos::{ColorRenderTargetFormat as C, EdramMode, MsaaSamples as M},
};
fn selection(base: u32) -> Setup {
    let key = Key {
        base_tiles: base,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: M::X1,
        is_depth: false,
        format: 0,
    };
    Setup {
        edram_mode: EdramMode::ColorDepth,
        surface_pitch: 80,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: M::X1,
        depth: None,
        color: [
            Some(Binding {
                slot: Slot::Color(0),
                key,
                guest_base_tiles: base,
                guest_color_format: Some(C::K8888),
                exp_bias: 0,
            }),
            None,
            None,
            None,
        ],
    }
}
#[test]
fn stale_borrowed_and_recorded_target_views_cannot_publish_wrong_edram_ownership() {
    let (_lock, before) = setup();
    let mut gpu = open();
    for recorded in [false, true] {
        let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
        let mut frame = cache.begin(&mut gpu).unwrap();
        let a = frame.targets(&selection(0), 16).unwrap();
        let colors = [Some(ColorAttachment {
            view: &a.selected[0].view,
            clear: Some(types::Color::WHITE),
        })];
        let rec = frame.gpu().render_recorder(&colors, None, &[]).unwrap();
        frame.targets(&selection(2), 16).unwrap();
        let result = if recorded {
            frame.render_recorded(&rec)
        } else {
            frame.render_full(&colors, None, &[], &[])
        };
        assert!(result.is_err());
        assert!(frame.submit().is_err());
        assert_eq!(cache.target_count(), 0);
        assert!(cache.ownership().is_empty());
    }
    drop(gpu);
    finish(before);
}
