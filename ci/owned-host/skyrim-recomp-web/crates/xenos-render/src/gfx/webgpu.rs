use super::sink::{PassSink, PassTypes};
use gpu_webgpu as w;
use std::{convert::Infallible, ops::Range};

pub(crate) struct Types;
impl PassTypes for Types {
    type Pipeline = w::RenderPipeline;
    type Group = w::BindGroup;
    type Buffer = w::Buffer;
    type IndexFormat = w::IndexFormat;
    // These are the real legacy handle identities, used only for deduplication.
    // A native session supplies its own IDs; no synthetic FFI pointers exist.
    fn pipeline_key(p: &Self::Pipeline) -> u64 { p.identity() as u64 }
    fn group_key(g: &Self::Group) -> u64 { g.identity() as u64 }
    fn index_bytes(format: Self::IndexFormat) -> u64 { if format == w::IndexFormat::Uint32 { 4 } else { 2 } }
}
pub(crate) struct Pass<'a, 'e>(pub &'a mut w::RenderPass<'e>);
impl PassSink<Types> for Pass<'_, '_> {
    type Error = Infallible;
    #[inline] fn set_pipeline(&mut self, p: &w::RenderPipeline) -> Result<(), Infallible> { self.0.set_pipeline(p); Ok(()) }
    #[inline] fn set_group(&mut self, slot: u32, g: &w::BindGroup, offsets: &[u32]) -> Result<(), Infallible> { self.0.set_bind_group(slot, g, offsets); Ok(()) }
    #[inline] fn set_index(&mut self, b: &w::Buffer, f: w::IndexFormat, offset: u64, size: Option<u64>) -> Result<(), Infallible> { self.0.set_index_buffer(b, f, offset, size); Ok(()) }
    #[inline] fn set_viewport(&mut self, [x,y,w,h,z0,z1]: [f32;6]) -> Result<(), Infallible> { self.0.set_viewport(x,y,w,h,z0,z1); Ok(()) }
    #[inline] fn set_scissor(&mut self, [x,y,w,h]: [u32;4]) -> Result<(), Infallible> { self.0.set_scissor_rect(x,y,w,h); Ok(()) }
    #[inline] fn set_blend(&mut self, [r,g,b,a]: [f64;4]) -> Result<(), Infallible> { self.0.set_blend_constant(w::Color {r,g,b,a}); Ok(()) }
    #[inline] fn set_stencil(&mut self, r: u32) -> Result<(), Infallible> { self.0.set_stencil_reference(r); Ok(()) }
    #[inline] fn draw(&mut self, v: Range<u32>, i: Range<u32>) -> Result<(), Infallible> { self.0.draw(v.end-v.start,i.end-i.start,v.start,i.start); Ok(()) }
    #[inline] fn draw_indexed(&mut self, v: Range<u32>, base: i32, i: Range<u32>) -> Result<(), Infallible> { self.0.draw_indexed(v.end-v.start,i.end-i.start,v.start,base,i.start); Ok(()) }
}
