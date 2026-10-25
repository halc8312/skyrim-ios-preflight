//! WebGPU executor of the separate sealed target plan. Authored probes only.
use super::*;
use crate::{sampled_frame as sf, target_frame as tf};

pub(super) fn render(host: &WebGpuFrames, plan: &tf::TargetFrame, reuse: bool) -> Result<Vec<u8>, String> {
    if host.sampled_types.is_empty() {
        return Err("target plan requires the sampled shader mode".into());
    }
    for bindings in plan.bindings() {
        if bindings.len() != host.sampled_types.len()
            || bindings
                .iter()
                .any(|b| host.sampled_types.get(&b.binding) != Some(&matches!(b.resource, sf::Resource::Texture(_))))
        {
            return Err("target binding type or coverage does not match shader reflection".into());
        }
    }
    let device = &host.device;
    let frame = plan.geometry();
    let mut input_textures = Vec::new();
    let mut input_views = Vec::new();
    for t in plan.textures() {
        let texture = device.create_texture(&TextureDescriptor {
            label: "target-plan CPU image version",
            size: extent(t.width, t.height, 1),
            mip_level_count: t.mips.len() as u32,
            format: TextureFormat::RGBA8Unorm,
            usage: TextureUsage::TextureBinding | TextureUsage::CopySrc | TextureUsage::CopyDst,
            ..Default::default()
        });
        for (level, mip) in t.mips.iter().enumerate() {
            device.queue().write_texture(
                &TexelCopyTextureInfo {
                    texture: &texture,
                    mip_level: level as u32,
                    origin: origin(0, 0, 0),
                    aspect: TextureAspect::All,
                },
                &mip.bytes,
                mip.bytes_per_row,
                Some((t.height >> level).max(1)),
                extent((t.width >> level).max(1), (t.height >> level).max(1), 1),
            );
        }
        input_views.push(texture.create_view(None));
        input_textures.push(texture);
    }
    let address = |a| match a {
        sf::AddressMode::ClampToEdge => AddressMode::ClampToEdge,
        sf::AddressMode::Repeat => AddressMode::Repeat,
        sf::AddressMode::MirrorRepeat => AddressMode::MirrorRepeat,
    };
    let filter = |f| if f == sf::FilterMode::Nearest { FilterMode::Nearest } else { FilterMode::Linear };
    let samplers: Vec<_> = plan
        .samplers()
        .iter()
        .map(|s| {
            let s = s.state;
            device.create_sampler(&SamplerDescriptor {
                label: "target-plan immutable sampler",
                address_mode_u: address(s.address[0]),
                address_mode_v: address(s.address[1]),
                address_mode_w: address(s.address[2]),
                min_filter: filter(s.filters[0]),
                mag_filter: filter(s.filters[1]),
                mipmap_filter: if s.filters[2] == sf::FilterMode::Nearest {
                    MipmapFilterMode::Nearest
                } else {
                    MipmapFilterMode::Linear
                },
                lod_min_clamp: s.lod[0],
                lod_max_clamp: s.lod[1],
                max_anisotropy: s.anisotropy,
                compare: None,
            })
        })
        .collect();
    let mut target_textures = Vec::new();
    let mut target_views = Vec::new();
    let allocation = tf::AllocationPlan::new(plan, reuse);
    for t in allocation.slots() {
        let mut usage = TextureUsage::RenderAttachment | TextureUsage::CopySrc | TextureUsage::CopyDst;
        if t.kind == tf::Kind::Color {
            usage |= TextureUsage::TextureBinding;
        }
        let texture = device.create_texture(&TextureDescriptor {
            label: "single-producer target version",
            size: extent(t.width, t.height, 1),
            format: if t.kind == tf::Kind::Color { TextureFormat::RGBA8Unorm } else { TextureFormat::Depth32Float },
            usage,
            ..Default::default()
        });
        target_views.push(texture.create_view(None));
        target_textures.push(texture);
    }
    let target_index =
        |v| allocation.mapping()[plan.targets().iter().position(|t| t.version == v).expect("sealed target")];
    let buffer_index = |v| frame.resources().iter().position(|r| r.version == v).expect("sealed buffer");
    let resources: Vec<_> = frame
        .resources()
        .iter()
        .map(|r| {
            let buffer = device.create_buffer(&BufferDescriptor {
                label: "target-plan immutable buffer",
                size: r.bytes.len() as u64,
                usage: BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Index,
                mapped_at_creation: true,
            });
            let status = buffer.write_mapped(0, &r.bytes);
            buffer.unmap();
            status.map_err(|e| format!("map {e:?}"))?;
            Ok(buffer)
        })
        .collect::<Result<_, String>>()?;
    let mut held = Vec::new();
    for (draw_index, d) in frame.passes().iter().flat_map(|p| &p.draws).enumerate() {
        let pipeline = checked_gpu(device, &format!("target pipeline draw={draw_index}"), || {
            planned_pipeline(
                device,
                &host.pipeline_layout,
                d,
                &host.vertex,
                host.vertex_entry,
                &host.pixel,
                host.pixel_entry,
            )
        })?;
        let groups = checked_gpu(device, &format!("target bindings draw={draw_index}"), || {
            let mut groups = Vec::new();
            for group in [0, 1] {
                let entries: Vec<_> = d
                    .bindings
                    .iter()
                    .filter(|b| b.group == group && host.used.contains(&(b.group, b.binding)))
                    .map(|b| BindGroupEntry {
                        binding: b.binding,
                        resource: BindingResource::Buffer {
                            buffer: &resources[buffer_index(b.slice.version)],
                            offset: u64::from(b.slice.offset),
                            size: Some(u64::from(b.slice.bytes)),
                        },
                    })
                    .collect();
                groups.push(device.create_bind_group(
                    "target-plan buffer bindings",
                    &host.layouts[group as usize],
                    &entries,
                ));
            }
            let entries: Vec<_> = plan.bindings()[draw_index]
                .iter()
                .map(|b| BindGroupEntry {
                    binding: b.binding,
                    resource: match b.resource {
                        sf::Resource::Texture(v) => BindingResource::TextureView(
                            if let Some(i) = plan.textures().iter().position(|t| t.version == v) {
                                &input_views[i]
                            } else {
                                &target_views[target_index(v)]
                            },
                        ),
                        sf::Resource::Sampler(v) => BindingResource::Sampler(
                            &samplers[plan.samplers().iter().position(|s| s.version == v).expect("sealed sampler")],
                        ),
                    },
                })
                .collect();
            groups.push(device.create_bind_group("target-plan sampled bindings", &host.layouts[2], &entries));
            groups
        })?;
        held.push((pipeline, groups));
    }
    let mut encoder = device.create_command_encoder("ordered versioned target operations");
    let mut draw_index = 0;
    for (operation_index, op) in plan.operations().iter().enumerate() {
        match *op {
            tf::Operation::Copy(c) => {
                if allocation.copy_elided()[operation_index] {
                    continue;
                }
                let source = if let Some(i) = plan.textures().iter().position(|t| t.version == c.source.version) {
                    &input_textures[i]
                } else {
                    &target_textures[target_index(c.source.version)]
                };
                let destination = plan.target(c.destination).expect("sealed copy destination");
                encoder.copy_texture_to_texture(
                    &TexelCopyTextureInfo {
                        texture: source,
                        mip_level: c.source.mip_level,
                        origin: origin(c.origin[0], c.origin[1], 0),
                        aspect: TextureAspect::All,
                    },
                    &TexelCopyTextureInfo::whole(&target_textures[target_index(c.destination)]),
                    extent(destination.width, destination.height, 1),
                );
            }
            tf::Operation::Render(r) => {
                for (kind, (source, destination)) in
                    [(r.color_load, r.color), (r.depth_load, r.depth)].into_iter().enumerate()
                {
                    if allocation.load_elided()[operation_index][kind] {
                        continue;
                    }
                    if let Some(source) = source {
                        encoder.copy_texture_to_texture(
                            &TexelCopyTextureInfo::whole(&target_textures[target_index(source)]),
                            &TexelCopyTextureInfo::whole(&target_textures[target_index(destination)]),
                            extent(frame.width(), frame.height(), 1),
                        );
                    }
                }
                let pass = &frame.passes()[r.pass as usize];
                let view = &target_views[target_index(r.color)];
                let mut color = match pass.clear_color {
                    Some([r, g, b, a]) => RenderPassColorAttachment::clear(view, Color { r, g, b, a }),
                    None => RenderPassColorAttachment::load(view),
                };
                color.store_op = if r.store_color { StoreOp::Store } else { StoreOp::Discard };
                let colors = [Some(color)];
                let depth = RenderPassDepthStencilAttachment {
                    view: &target_views[target_index(r.depth)],
                    depth_load_op: if pass.clear_depth.is_some() { LoadOp::Clear } else { LoadOp::Load },
                    depth_store_op: if r.store_depth { StoreOp::Store } else { StoreOp::Discard },
                    depth_clear_value: pass.clear_depth.unwrap_or(0.) as f32,
                    depth_read_only: false,
                    stencil_load_op: LoadOp::Undefined,
                    stencil_store_op: StoreOp::Undefined,
                    stencil_clear_value: 0,
                    stencil_read_only: false,
                };
                let mut rp = encoder.begin_render_pass("ordered target render pass", &colors, Some(&depth));
                for d in &pass.draws {
                    let (pipeline, groups) = &held[draw_index];
                    draw_index += 1;
                    rp.set_pipeline(pipeline);
                    for (group, bg) in groups.iter().enumerate() {
                        rp.set_bind_group(group as u32, bg, &[]);
                    }
                    let [x, y, w, h, z0, z1] = d.state.viewport();
                    rp.set_viewport(x, y, w, h, z0, z1);
                    let [x, y, w, h] = d.state.scissor;
                    rp.set_scissor_rect(x, y, w, h);
                    let [r, g, b, a] = d.state.blend();
                    rp.set_blend_constant(Color { r, g, b, a });
                    rp.set_stencil_reference(d.state.stencil_reference);
                    if let Some(index) = d.indices {
                        rp.set_index_buffer(
                            &resources[buffer_index(index.slice.version)],
                            if index.stride == 2 { IndexFormat::Uint16 } else { IndexFormat::Uint32 },
                            u64::from(index.slice.offset),
                            Some(u64::from(index.slice.bytes)),
                        );
                        rp.draw_indexed(d.count, 1, 0, 0, 0);
                    } else {
                        rp.draw(d.count, 1, 0, 0);
                    }
                }
                rp.end();
            }
        }
    }
    let out = plan.output();
    let row = (out.width * 4).next_multiple_of(256);
    let read = device.create_buffer(&BufferDescriptor {
        label: "target-plan result",
        size: u64::from(row * out.height),
        usage: BufferUsage::MapRead | BufferUsage::CopyDst,
        mapped_at_creation: false,
    });
    encoder.copy_texture_to_buffer(
        &TexelCopyTextureInfo::whole(&target_textures[target_index(out.version)]),
        &TexelCopyBufferInfo { buffer: &read, offset: 0, bytes_per_row: row, rows_per_image: None },
        extent(out.width, out.height, 1),
    );
    device.queue().submit([encoder.finish("ordered target-plan result")]);
    if let Err(error) = device.block_on(read.map_async(MapMode::Read, 0, None)) {
        read.unmap();
        return Err(format!("readback {error:?}"));
    }
    let mut padded = vec![0; row as usize * out.height as usize];
    let status = read.read_mapped(0, &mut padded);
    read.unmap();
    status.map_err(|e| format!("mapped {e:?}"))?;
    Ok(padded.chunks(row as usize).flat_map(|r| r[..out.width as usize * 4].iter().copied()).collect())
}
