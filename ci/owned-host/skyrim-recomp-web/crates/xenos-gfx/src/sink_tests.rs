use super::*;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Format { U16, U32 }
struct Types;
impl PassTypes for Types {
    type Pipeline=u64;type Group=u64;type Buffer=u64;type IndexFormat=Format;
    fn pipeline_key(p:&u64)->u64 {*p}fn group_key(g:&u64)->u64 {*g}
    fn index_bytes(f:Format)->u64 {match f {Format::U16=>2,Format::U32=>4}}
}
#[derive(Debug,PartialEq,Eq)]
enum Op {
    Pipeline(u64),Group(u32,u64,Vec<u32>),Index(u64,Format,u64,Option<u64>),
    Viewport([u32;6]),Scissor([u32;4]),Blend([u64;4]),Stencil(u32),
    Draw(Range<u32>,Range<u32>),Indexed(Range<u32>,i32,Range<u32>),
}
#[derive(Default)]
struct Trace {ops:Vec<Op>,fail_at:Option<usize>}
impl Trace {fn push(&mut self,op:Op)->Result<(),&'static str> {
    if self.fail_at==Some(self.ops.len()) {return Err("injected sink failure");}self.ops.push(op);Ok(())
}}
impl PassSink<Types> for Trace {
    type Error=&'static str;
    fn set_pipeline(&mut self,p:&u64)->Result<(),Self::Error>{self.push(Op::Pipeline(*p))}
    fn set_group(&mut self,s:u32,g:&u64,o:&[u32])->Result<(),Self::Error>{self.push(Op::Group(s,*g,o.to_vec()))}
    fn set_index(&mut self,b:&u64,f:Format,o:u64,s:Option<u64>)->Result<(),Self::Error>{self.push(Op::Index(*b,f,o,s))}
    fn set_viewport(&mut self,v:[f32;6])->Result<(),Self::Error>{self.push(Op::Viewport(v.map(f32::to_bits)))}
    fn set_scissor(&mut self,r:[u32;4])->Result<(),Self::Error>{self.push(Op::Scissor(r))}
    fn set_blend(&mut self,c:[f64;4])->Result<(),Self::Error>{self.push(Op::Blend(c.map(f64::to_bits)))}
    fn set_stencil(&mut self,r:u32)->Result<(),Self::Error>{self.push(Op::Stencil(r))}
    fn draw(&mut self,v:Range<u32>,i:Range<u32>)->Result<(),Self::Error>{self.push(Op::Draw(v,i))}
    fn draw_indexed(&mut self,v:Range<u32>,b:i32,i:Range<u32>)->Result<(),Self::Error>{self.push(Op::Indexed(v,b,i))}
}
fn state(changed:bool)->DrawState {
    if changed {DrawState::new([1.,0.,31.,32.,0.,1.],[1,2,3,4],[0.5,0.25,0.,1.],7)}
    else {DrawState::new([0.,0.,32.,32.,0.,1.],[0,0,32,32],[0.,0.,0.,0.],0)}
}
fn packet(changed:bool,indices:Option<DrawIndices<Types>>)->DrawPacket<Types> {
    DrawPacket {pipeline:if changed{11}else{10},textures:if changed{41}else{40},
        constant_offsets:if changed{[256,0,0,0,0]}else{[0;5]},state:state(changed),indices,count:3}
}
fn resources()->Resources<'static,Types> {
    Resources{constants:&30,shared:&20,indices:&50,gathered:&60,shared_buffers:&[70,71]}
}
#[test]
fn original_command_order_cache_and_all_index_sources_are_preserved() {
    let packets=[packet(false,None),packet(false,None),
        packet(true,Some(DrawIndices::Ring{format:Format::U16,offset:4})),
        packet(true,Some(DrawIndices::Ring{format:Format::U16,offset:10})),
        packet(true,Some(DrawIndices::Gathered{format:Format::U32,offset:8})),
        packet(true,Some(DrawIndices::Shared{buffer:1,format:Format::U32,offset:12}))];
    let mut trace=Trace::default();emit(&packets,&resources(),DebugOptions::default(),&mut trace).unwrap();
    // Independently specified from the pre-extraction Executor behavior.
    let a=state(false);let b=state(true);
    assert_eq!(trace.ops,vec![
        Op::Group(1,20,vec![]),Op::Pipeline(10),Op::Group(0,30,vec![0;5]),Op::Group(2,40,vec![]),
        Op::Viewport(a.viewport_bits),Op::Scissor(a.scissor),Op::Blend(a.blend_bits),Op::Stencil(0),
        Op::Draw(0..3,0..1),Op::Draw(0..3,0..1),
        Op::Pipeline(11),Op::Group(0,30,vec![256,0,0,0,0]),Op::Group(2,41,vec![]),
        Op::Viewport(b.viewport_bits),Op::Scissor(b.scissor),Op::Blend(b.blend_bits),Op::Stencil(7),
        Op::Index(50,Format::U16,0,None),Op::Indexed(2..5,0,0..1),Op::Indexed(5..8,0,0..1),
        Op::Index(60,Format::U32,0,None),Op::Indexed(2..5,0,0..1),
        Op::Index(71,Format::U32,0,None),Op::Indexed(3..6,0,0..1),
    ]);
}
#[test]
fn debug_skip_keeps_state_but_emits_no_indices_or_draws() {
    let packets=[packet(false,None),packet(true,Some(DrawIndices::Shared{buffer:1,format:Format::U32,offset:12}))];
    let mut trace=Trace::default();emit(&packets,&resources(),DebugOptions{one_pixel:true,skip_draws:true},&mut trace).unwrap();
    assert!(!trace.ops.iter().any(|o|matches!(o,Op::Draw(..)|Op::Indexed(..)|Op::Index(..))));
    assert_eq!(trace.ops.iter().filter_map(|o|if let Op::Scissor(r)=o{Some(*r)}else{None}).collect::<Vec<_>>(),vec![[0,0,1,1],[1,2,1,1]]);
}
#[test]
fn signed_zero_state_and_sink_failure_are_not_silently_discarded() {
    let mut second=packet(false,None);second.state.viewport_bits[0]=(-0.0f32).to_bits();
    let packets=[packet(false,None),second];let mut trace=Trace::default();
    emit(&packets,&resources(),DebugOptions::default(),&mut trace).unwrap();
    assert_eq!(trace.ops.iter().filter(|o|matches!(o,Op::Viewport(_))).count(),2);
    let mut fail=Trace{fail_at:Some(1),..Default::default()};
    assert_eq!(emit(&packets,&resources(),DebugOptions::default(),&mut fail),Err(EmitError::Sink("injected sink failure")));
    assert_eq!(fail.ops,vec![Op::Group(1,20,vec![])]);
}

#[test]
fn invalid_index_packets_are_rejected_before_any_sink_call() {
    for (indices,count,error) in [
        (DrawIndices::Ring{format:Format::U16,offset:1u64<<33},3,PacketError::IndexRange),
        (DrawIndices::Ring{format:Format::U32,offset:u64::from(u32::MAX)*4},3,PacketError::IndexRange),
        (DrawIndices::Gathered{format:Format::U16,offset:3},3,PacketError::IndexAlignment),
        (DrawIndices::Shared{buffer:usize::MAX,format:Format::U16,offset:0},3,PacketError::MissingSharedBuffer),
    ] {
        let mut invalid=packet(false,Some(indices));invalid.count=count;let packets=[packet(false,None),invalid];let mut trace=Trace::default();
        assert_eq!(emit(&packets,&resources(),DebugOptions::default(),&mut trace),Err(EmitError::Packet(error)));
        assert!(trace.ops.is_empty());
    }
}
