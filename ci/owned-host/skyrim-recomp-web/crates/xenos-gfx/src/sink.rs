//! Xenos draw recording contract with no GPU API imports or raw handles.
//! Resource lookups happen before recording. The same immutable packets and
//! deduplication rules may target a legacy pass or a validated native recorder.
use std::ops::Range;

pub trait PassTypes {
    type Pipeline;
    type Group;
    type Buffer;
    type IndexFormat: Copy + Eq;
    fn pipeline_key(p: &Self::Pipeline) -> u64;
    fn group_key(g: &Self::Group) -> u64;
    fn index_bytes(format: Self::IndexFormat) -> u64;
}

pub trait PassSink<T: PassTypes> {
    type Error;
    fn set_pipeline(&mut self, p: &T::Pipeline) -> Result<(), Self::Error>;
    fn set_group(&mut self, slot: u32, g: &T::Group, offsets: &[u32]) -> Result<(), Self::Error>;
    fn set_index(&mut self, b: &T::Buffer, format: T::IndexFormat, offset: u64, size: Option<u64>) -> Result<(), Self::Error>;
    fn set_viewport(&mut self, values: [f32; 6]) -> Result<(), Self::Error>;
    fn set_scissor(&mut self, rect: [u32; 4]) -> Result<(), Self::Error>;
    // Preserve the legacy f64 interface; a backend performs its own conversion.
    fn set_blend(&mut self, color: [f64; 4]) -> Result<(), Self::Error>;
    fn set_stencil(&mut self, reference: u32) -> Result<(), Self::Error>;
    fn draw(&mut self, vertices: Range<u32>, instances: Range<u32>) -> Result<(), Self::Error>;
    fn draw_indexed(&mut self, indices: Range<u32>, base_vertex: i32, instances: Range<u32>) -> Result<(), Self::Error>;
}

/// Captured at the guest draw position. Equality preserves float bits, including
/// signed zero. Backend validators retain responsibility for legal value ranges.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct DrawState {
    pub viewport_bits: [u32; 6],
    pub scissor: [u32; 4],
    pub blend_bits: [u64; 4],
    pub stencil_reference: u32,
    pub reserved: u32,
}
impl DrawState {
    pub fn new(viewport: [f32; 6], scissor: [u32; 4], blend: [f64; 4], stencil_reference: u32) -> Self {
        Self { viewport_bits: viewport.map(f32::to_bits), scissor, blend_bits: blend.map(f64::to_bits), stencil_reference, reserved: 0 }
    }
    pub fn viewport(&self) -> [f32; 6] { self.viewport_bits.map(f32::from_bits) }
    pub fn blend(&self) -> [f64; 4] { self.blend_bits.map(f64::from_bits) }
}
const _: () = assert!(std::mem::size_of::<DrawState>() == 80);

pub enum DrawIndices<T: PassTypes> {
    Ring { format: T::IndexFormat, offset: u64 },
    Gathered { format: T::IndexFormat, offset: u64 },
    Shared { buffer: usize, format: T::IndexFormat, offset: u64 },
}
impl<T: PassTypes> Copy for DrawIndices<T> {}
impl<T: PassTypes> Clone for DrawIndices<T> { fn clone(&self) -> Self { *self } }
pub struct DrawPacket<T: PassTypes> {
    pub pipeline: T::Pipeline,
    pub textures: T::Group,
    pub constant_offsets: [u32; 5],
    pub state: DrawState,
    pub indices: Option<DrawIndices<T>>,
    pub count: u32,
}
pub struct Resources<'a, T: PassTypes> {
    pub constants: &'a T::Group,
    pub shared: &'a T::Group,
    pub indices: &'a T::Buffer,
    pub gathered: &'a T::Buffer,
    pub shared_buffers: &'a [T::Buffer],
}
#[derive(Clone, Copy, Default)]
pub struct DebugOptions { pub one_pixel: bool, pub skip_draws: bool }

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum PacketError { IndexWidth, IndexAlignment, IndexRange, MissingSharedBuffer }
#[derive(Debug,PartialEq,Eq)]
pub enum EmitError<E> { Packet(PacketError), Sink(E) }

/// Check producer invariants without touching a sink. This also lets callers
/// reject corrupt input before opening a pass with attachment clears.
pub struct ValidatedPackets<'a,T:PassTypes> {draws:&'a [DrawPacket<T>],shared_buffers:usize,options:DebugOptions}
impl<T:PassTypes> ValidatedPackets<'_,T> {
    pub fn emit<S:PassSink<T>>(&self,resources:&Resources<'_,T>,sink:&mut S)->Result<(),EmitError<S::Error>> {
        if resources.shared_buffers.len()<self.shared_buffers {return Err(EmitError::Packet(PacketError::MissingSharedBuffer));}
        emit_validated(self.draws,resources,self.options,sink).map_err(EmitError::Sink)
    }
}
pub fn validate_packets<T:PassTypes>(draws:&[DrawPacket<T>],shared_buffers:usize,options:DebugOptions)->Result<ValidatedPackets<'_,T>,PacketError> {
    if options.skip_draws {return Ok(ValidatedPackets{draws,shared_buffers:0,options});}
    for d in draws {
        let (format,offset)=match d.indices {
            None=>continue,
            Some(DrawIndices::Ring{format,offset}|DrawIndices::Gathered{format,offset})=>(format,offset),
            Some(DrawIndices::Shared{buffer,format,offset})=>{
                if buffer>=shared_buffers {return Err(PacketError::MissingSharedBuffer);}(format,offset)
            }
        };
        let unit=T::index_bytes(format);
        if unit!=2 && unit!=4 {return Err(PacketError::IndexWidth);}
        if offset%unit!=0 {return Err(PacketError::IndexAlignment);}
        if u32::try_from(offset/unit).ok().and_then(|first|first.checked_add(d.count)).is_none() {
            return Err(PacketError::IndexRange);
        }
    }Ok(ValidatedPackets{draws,shared_buffers,options})
}

/// Matches the original Executor's state-change order, including the initial
/// shared group, skipped-draw diagnostics, whole-buffer index binding and first
/// index calculation. No command vector or allocation is introduced here.
pub fn emit<T: PassTypes, S: PassSink<T>>(
    draws: &[DrawPacket<T>], resources: &Resources<'_, T>, options: DebugOptions, sink: &mut S,
) -> Result<(), EmitError<S::Error>> {
    validate_packets(draws,resources.shared_buffers.len(),options).map_err(EmitError::Packet)?.emit(resources,sink)
}
fn emit_validated<T:PassTypes,S:PassSink<T>>(
    draws:&[DrawPacket<T>],resources:&Resources<'_,T>,options:DebugOptions,sink:&mut S,
)->Result<(),S::Error> {
    sink.set_group(1, resources.shared, &[])?;
    let mut pipeline = None;
    let mut constants = None;
    let mut textures = None;
    let mut viewport = None;
    let mut scissor = None;
    let mut blend = None;
    let mut stencil = None;
    let mut index = None;
    for d in draws {
        let key = T::pipeline_key(&d.pipeline);
        if pipeline != Some(key) { sink.set_pipeline(&d.pipeline)?; pipeline = Some(key); }
        if constants != Some(d.constant_offsets) {
            sink.set_group(0, resources.constants, &d.constant_offsets)?; constants = Some(d.constant_offsets);
        }
        let key = T::group_key(&d.textures);
        if textures != Some(key) { sink.set_group(2, &d.textures, &[])?; textures = Some(key); }
        if viewport != Some(d.state.viewport_bits) { sink.set_viewport(d.state.viewport())?; viewport = Some(d.state.viewport_bits); }
        let rect = if options.one_pixel { [d.state.scissor[0], d.state.scissor[1], 1, 1] } else { d.state.scissor };
        if scissor != Some(rect) { sink.set_scissor(rect)?; scissor = Some(rect); }
        if blend != Some(d.state.blend_bits) { sink.set_blend(d.state.blend())?; blend = Some(d.state.blend_bits); }
        if stencil != Some(d.state.stencil_reference) { sink.set_stencil(d.state.stencil_reference)?; stencil = Some(d.state.stencil_reference); }
        if options.skip_draws { continue; }
        let (source, format, offset) = match d.indices {
            None => { sink.draw(0..d.count, 0..1)?; continue; }
            Some(DrawIndices::Ring { format, offset }) => (usize::MAX, format, offset),
            Some(DrawIndices::Gathered { format, offset }) => (usize::MAX - 1, format, offset),
            Some(DrawIndices::Shared { buffer, format, offset }) => (buffer, format, offset),
        };
        if index != Some((source, format)) {
            let buffer = match source {
                usize::MAX => resources.indices,
                value if value == usize::MAX - 1 => resources.gathered,
                value => &resources.shared_buffers[value],
            };
            sink.set_index(buffer, format, 0, None)?; index = Some((source, format));
        }
        // The producer reserves a ring/shared window before recording. Keep its
        // established first-index meaning instead of baking a new byte offset.
        let first = (offset / T::index_bytes(format)) as u32;
        sink.draw_indexed(first..first + d.count, 0, 0..1)?;
    }
    Ok(())
}

#[cfg(test)]
#[path = "sink_tests.rs"]
mod tests;
