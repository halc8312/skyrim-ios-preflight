use super::{setup,open,finish,DEADLINE,U};
use skyrim_native_gpu::{bindings::{Entry,Resource},render::{BoundGroup,ColorAttachment,PipelineDescriptor,Draw},sink::PassSink,texture::TextureDescriptor,types::{self,TextureUses as T}};

#[test]
fn unspecified_minimum_checks_each_compute_pipeline_before_any_dispatch() {
    let (_lock,before)=setup();let mut gpu=open();
    let gl=gpu.group_layout(&[types::BindGroupLayoutEntry{binding:0,visibility:types::ShaderStages::COMPUTE,ty:types::BindingType::Buffer{ty:types::BufferBindingType::Storage{read_only:false},has_dynamic_offset:false,min_binding_size:None},count:None}]).unwrap();
    let layout=gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let small=gpu.shader("@group(0) @binding(0) var<storage,read_write> data:vec4u; @compute @workgroup_size(1) fn main(){data=vec4u(1u,2u,3u,4u);}").unwrap();
    let large=gpu.shader("@group(0) @binding(0) var<storage,read_write> data:array<vec4u,2>; @compute @workgroup_size(1) fn main(){data[1]=vec4u(9u);}").unwrap();
    let p=gpu.shared_compute_pipeline(&layout,&small,"main").unwrap();let q=gpu.shared_compute_pipeline(&layout,&large,"main").unwrap();
    let buffer=gpu.create_buffer("deferred size output",16,U::STORAGE_READ_WRITE|U::COPY_SRC).unwrap();
    let group=gpu.bind_group(&gl,&[Entry{binding:0,resource:Resource::Buffer{buffer:&buffer,offset:0,size:16}}]).unwrap();
    let groups=[BoundGroup{group:&group,dynamic_offsets:&[]}];let mut encoder=gpu.encoder().unwrap();
    let bad=[skyrim_native_gpu::compute::ComputeDispatch{pipeline:&p,groups:&groups,workgroups:[1,1,1]},skyrim_native_gpu::compute::ComputeDispatch{pipeline:&q,groups:&groups,workgroups:[0,1,1]}];
    assert!(encoder.dispatch_shared_batch(&bad).is_err());
    let read=gpu.create_buffer("deferred size check",16,U::MAP_READ|U::COPY_DST).unwrap();encoder.copy_buffer(&buffer,0,&read,0,16).unwrap();let serial=gpu.submit(vec![encoder.finish().unwrap()]).unwrap();gpu.wait(serial,DEADLINE).unwrap();assert_eq!(gpu.read_buffer(&read,0..16,DEADLINE).unwrap(),[0;16]);
    let mut encoder=gpu.encoder().unwrap();encoder.dispatch_shared(&p,&groups,[1,1,1]).unwrap();encoder.copy_buffer(&buffer,0,&read,0,16).unwrap();gpu.submit(vec![encoder.finish().unwrap()]).unwrap();assert_eq!(gpu.read_buffer(&read,0..16,DEADLINE).unwrap(),[1u32,2,3,4].into_iter().flat_map(u32::to_le_bytes).collect::<Vec<_>>());
    drop(gpu);finish(before);
}

#[test]
fn same_layout_render_pipeline_switch_rechecks_deferred_minimum() {
    let (_lock,before)=setup();let mut gpu=open();
    let gl=gpu.group_layout(&[types::BindGroupLayoutEntry{binding:0,visibility:types::ShaderStages::FRAGMENT,ty:types::BindingType::Buffer{ty:types::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None},count:None}]).unwrap();let layout=gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let vertex=gpu.shader("@vertex fn vs(@builtin(vertex_index)i:u32)->@builtin(position)vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}").unwrap();
    let small=gpu.shader("@group(0) @binding(0) var<uniform> c:vec4f; @fragment fn fs()->@location(0)vec4f{return c;}").unwrap();
    let large=gpu.shader("struct P{a:vec4f,b:vec4f} @group(0) @binding(0) var<uniform> c:P; @fragment fn fs()->@location(0)vec4f{return c.b;}").unwrap();
    let colors=[Some(types::ColorTargetState{format:types::TextureFormat::Rgba8Unorm,blend:None,write_mask:types::ColorWrites::ALL})];
    let make=|shader|gpu.render_pipeline(&PipelineDescriptor{layout:&layout,vertex:&vertex,vertex_entry:"vs",fragment:Some((shader,"fs")),colors:&colors,primitive:Default::default(),depth_stencil:None,multisample:Default::default()}).unwrap();let p=make(&small);let q=make(&large);
    let u=gpu.create_buffer("small uniform",16,U::UNIFORM).unwrap();let group=gpu.bind_group(&gl,&[Entry{binding:0,resource:Resource::Buffer{buffer:&u,offset:0,size:16}}]).unwrap();
    let target=gpu.create_texture("minimum test target",TextureDescriptor{size:types::Extent3d{width:1,height:1,depth_or_array_layers:1},mip_levels:1,sample_count:1,dimension:types::TextureDimension::D2,format:types::TextureFormat::Rgba8Unorm,usage:T::COLOR_TARGET}).unwrap();let view=gpu.texture_view(&target,types::TextureViewDimension::D2,0..1,0..1).unwrap();let attachments=[Some(ColorAttachment{view:&view,clear:Some(types::Color::BLACK)})];
    let mut recorder=gpu.render_recorder(&attachments,None,&[]).unwrap();recorder.set_pipeline(&p).unwrap();recorder.set_group(0,&group,&[]).unwrap();recorder.draw(0..3,0..1).unwrap();recorder.set_pipeline(&q).unwrap();assert!(recorder.draw(0..3,0..1).is_err());
    let groups=[BoundGroup{group:&group,dynamic_offsets:&[]}];let bad=Draw{pipeline:&q,groups:&groups,vertices:0..3,instances:0..1,indices:None,viewport:None,scissor:None,blend:types::Color::TRANSPARENT,stencil_reference:0};let mut encoder=gpu.encoder().unwrap();assert!(encoder.render(&attachments,&[bad]).is_err());assert!(encoder.render_recorded(&recorder).is_err());drop(encoder);
    drop(gpu);finish(before);
}

#[test]
fn inferred_layout_uses_only_entry_resources_and_actual_sampling_filterability(){
    let (_lock,before)=setup();let gpu=open();
    let shader=gpu.shader("@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var s:sampler; @group(0) @binding(2) var<storage,read_write> out:vec4f; @compute @workgroup_size(1) fn load(){out=textureLoad(t,vec2i(0),0);} @compute @workgroup_size(1) fn sample(){out=textureSampleLevel(t,s,vec2f(0.5),0.);}").unwrap();
    let(_,load)=gpu.inferred_layout(&[(&shader,"load",types::ShaderStages::COMPUTE)]).unwrap();assert_eq!(load[0].entries().len(),2);assert!(matches!(load[0].entries()[0].ty,types::BindingType::Texture{sample_type:types::TextureSampleType::Float{filterable:false},..}));
    let(_,sample)=gpu.inferred_layout(&[(&shader,"sample",types::ShaderStages::COMPUTE)]).unwrap();assert_eq!(sample[0].entries().len(),3);assert!(matches!(sample[0].entries()[0].ty,types::BindingType::Texture{sample_type:types::TextureSampleType::Float{filterable:true},..}));
    assert!(gpu.inferred_layout(&[(&shader,"load",types::ShaderStages::COMPUTE),(&shader,"sample",types::ShaderStages::COMPUTE)]).is_err());drop(gpu);finish(before);
}
