//! Shared resolve conversion/parameter contract, used by both GPU adapters.
//! Pixel conversion remains the established edram_shaders WGSL implementation.
use xenos_state::{resolve::{ResolveCopyKind as K, ResolveInfo}, edram::EDRAM_TILE_COUNT};

pub fn copy_encoding(kind: K) -> (u32,u32,u32) {
    match kind {
        K::Fast32bpp1x2xMsaa | K::Fast32bpp4xMsaa => (0,1,32),
        K::Fast64bpp1x2xMsaa | K::Fast64bpp4xMsaa => (1,2,64),
        K::Full8bpp => (2,1,8), K::Full16bpp => (2,1,16),
        K::Full32bpp => (2,1,32), K::Full64bpp => (2,2,64),
        K::Full128bpp => (2,4,128), K::Dxt3aAs1111 => (3,1,64),
    }
}
/// The production 96-byte uniform; offsets are source HOST-image offsets after
/// scaling, while the copy width/height and EDRAM offsets remain GUEST pixels.
pub fn copy_words(info:&ResolveInfo,kind:K,offset:[u32;2],scale:u32)->Option<[u32;24]> {
    let source=info.source_edram()?;
    let dest=info.copy_dest_info;
    let (mode,words_per_pixel,dest_bpp)=copy_encoding(kind);
    let flags=u32::from(source.is_depth)|(u32::from(source.format_is_64bpp)<<1)
        |(u32::from(dest.copy_dest_swap)<<2)|(u32::from(source.decode_pwl_gamma)<<3);
    Some([
        info.rect.width,info.rect.height,info.edram_offset_x_div_8<<3,info.edram_offset_y_div_8<<3,
        source.base_tiles&(EDRAM_TILE_COUNT-1),source.pitch_tiles,source.msaa_samples.raw(),flags,
        source.format,mode,info.copy_sample_select.raw(),info.dest_format.raw(),
        dest.copy_dest_number.raw(),(dest.copy_dest_exp_bias.clamp(-32,31) as f32).exp2().to_bits(),
        words_per_pixel,dest_bpp,info.copy_func,info.copy_ref,info.copy_mask,
        offset[0]*scale,offset[1]*scale,0,0,0,
    ])
}
