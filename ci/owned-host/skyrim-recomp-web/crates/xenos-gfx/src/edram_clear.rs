//! Backend-independent legacy/Xenia resolve-clear geometry. Device/resource
//! ownership and input validation belong to the caller; this planner preserves
//! the established clipping, 64bpp tile units and simultaneous depth/color rule.
use xenos_state::{
    edram::{EDRAM_TILE_COUNT, EDRAM_TILE_HEIGHT_SAMPLES, EDRAM_TILE_WIDTH_SAMPLES, RenderTargetKey},
    resolve::ResolveInfo,
    xenos::ColorRenderTargetFormat,
};
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidMsaa(u32),
    UnsupportedFormat(u32),
    PitchTooLarge { width: u32, max: u32 },
}
#[derive(Clone, Debug)]
pub struct ClearPlan {
    pub key: RenderTargetKey,
    /// Absolute first tile and tile count claimed by the clear.
    pub start: u32,
    pub length: u32,
    pub rect: [u32; 4],
    pub value: [u32; 2],
}

pub fn plan_clears(info: &ResolveInfo, max_dimension: u32) -> Result<Vec<ClearPlan>, Error> {
    let depth = info.depth_edram.filter(|_| info.is_clearing_depth());
    let color = info.color_edram.filter(|_| info.is_clearing_color());
    let (pitch32, offset32, msaa) = if let Some(d) = depth {
        (d.pitch_tiles, d.base_tiles.wrapping_sub(info.depth_original_base), d.msaa_samples)
    } else if let Some(c) = color {
        let s = c.format_is_64bpp as u32;
        (c.pitch_tiles >> s, c.base_tiles.wrapping_sub(info.color_original_base) >> s, c.msaa_samples)
    } else {
        return Ok(Vec::new());
    };
    if msaa.count().is_none() {
        return Err(Error::InvalidMsaa(msaa.raw()));
    }
    if pitch32 == 0 {
        return Ok(Vec::new());
    }
    let (sx, sy) = (msaa.samples_x_log2(), msaa.samples_y_log2());
    let pitch_pixels = pitch32 * (EDRAM_TILE_WIDTH_SAMPLES >> sx);
    if pitch_pixels > max_dimension {
        return Err(Error::PitchTooLarge { width: pitch_pixels, max: max_dimension });
    }
    let tile_h = EDRAM_TILE_HEIGHT_SAMPLES >> msaa.samples_y_log2();
    let height = (EDRAM_TILE_COUNT.div_ceil(pitch32) * tile_h).min(max_dimension / tile_h * tile_h);
    let offset_rows = offset32 / pitch32;
    let x = ((offset32 - offset_rows * pitch32) * (EDRAM_TILE_WIDTH_SAMPLES >> sx) + (info.edram_offset_x_div_8 << 3))
        .min(pitch_pixels);
    let y = (offset_rows * (EDRAM_TILE_HEIGHT_SAMPLES >> sy) + (info.edram_offset_y_div_8 << 3)).min(height);
    let w = info.rect.width.min(pitch_pixels - x);
    let h = info.rect.height.min(height - y);
    if w == 0 || h == 0 {
        return Ok(Vec::new());
    }
    let start32 = ((y << sy) / EDRAM_TILE_HEIGHT_SAMPLES) * pitch32 + (x << sx) / EDRAM_TILE_WIDTH_SAMPLES;
    let length32 = (((y + h - 1) << sy) / EDRAM_TILE_HEIGHT_SAMPLES) * pitch32
        + ((x + w - 1) << sx) / EDRAM_TILE_WIDTH_SAMPLES
        + 1
        - start32;
    let span = |is_64: bool| {
        let s = is_64 as u32;
        let start = (start32 << s).min(EDRAM_TILE_COUNT);
        (start, ((start32 + length32) << s).min(EDRAM_TILE_COUNT) - start)
    };
    let (depth_start, mut depth_len) = depth.map_or((0, 0), |_| span(false));
    let (color_start, mut color_len) = color.map_or((0, 0), |c| span(c.format_is_64bpp));
    let depth_wrapped = info.depth_original_base.wrapping_add(depth_start) & (EDRAM_TILE_COUNT - 1);
    let color_wrapped = info.color_original_base.wrapping_add(color_start) & (EDRAM_TILE_COUNT - 1);
    if depth_len != 0 && color_len != 0 {
        let distance = |from: u32, to: u32| if to < from { EDRAM_TILE_COUNT } else { 0 } + to - from;
        depth_len = depth_len.min(distance(depth_wrapped, color_wrapped));
        color_len = color_len.min(distance(color_wrapped, depth_wrapped));
    }
    let rect = [x, y, x + w, y + h];
    let mut plans = Vec::new();
    if let Some(d) = depth.filter(|_| depth_len != 0) {
        plans.push(ClearPlan {
            key: RenderTargetKey {
                base_tiles: info.depth_original_base & (EDRAM_TILE_COUNT - 1),
                pitch_tiles_at_32bpp: pitch32,
                msaa_samples: msaa,
                is_depth: true,
                format: d.format,
            },
            start: depth_wrapped,
            length: depth_len,
            rect,
            value: [info.depth_clear, 0],
        });
    }
    if let Some(c) = color.filter(|_| color_len != 0) {
        let format = ColorRenderTargetFormat::from_raw(c.format).storage_format();
        if super::edram_transfer::color_storage(format).is_none() {
            return Err(Error::UnsupportedFormat(c.format));
        }
        plans.push(ClearPlan {
            key: RenderTargetKey {
                base_tiles: info.color_original_base & (EDRAM_TILE_COUNT - 1),
                pitch_tiles_at_32bpp: pitch32,
                msaa_samples: msaa,
                is_depth: false,
                format: format.raw(),
            },
            start: color_wrapped,
            length: color_len,
            rect,
            value: info.color_clear_value(),
        });
    }
    Ok(plans)
}
