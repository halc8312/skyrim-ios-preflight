//! Checked, reflected storage-buffer compute path. This supports the renderer's
//! constant-patch kernel without native/legacy resource interop. Other binding
//! classes, dynamic offsets and overrides return explicit unsupported errors.
use super::*;
use std::borrow::Cow;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StorageSlot {
    pub binding: u32,
    pub read_only: bool,
    pub min_bytes: u64,
}

struct PipelineInner<B: Backend> {
    owner: Rc<Owner<B>>,
    id: u64,
    slots: Vec<StorageSlot>,
    pipeline: Option<B::ComputePipeline>,
    layout: Option<B::PipelineLayout>,
    group_layout: Option<B::BindGroupLayout>,
    shader: Option<B::ShaderModule>,
}
impl<B: Backend> Drop for PipelineInner<B> {
    fn drop(&mut self) {
        unsafe {
            if let Some(p) = self.pipeline.take() {
                self.owner.device.destroy_compute_pipeline(p);
            }
            if let Some(p) = self.layout.take() {
                self.owner.device.destroy_pipeline_layout(p);
            }
            if let Some(p) = self.group_layout.take() {
                self.owner.device.destroy_bind_group_layout(p);
            }
            if let Some(p) = self.shader.take() {
                self.owner.device.destroy_shader_module(p);
            }
        }
    }
}
pub struct ComputePipeline<B: Backend>(Rc<PipelineInner<B>>);
impl<B: Backend> Clone for ComputePipeline<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> ComputePipeline<B> {
    pub fn slots(&self) -> &[StorageSlot] {
        &self.0.slots
    }
}

pub struct StorageBinding<'a, B: Backend> {
    pub binding: u32,
    pub buffer: &'a Buffer<B>,
    pub offset: u64,
    pub size: u64,
}
struct GroupInner<B: Backend> {
    id: u64,
    pipeline: ComputePipeline<B>,
    raw: Option<B::BindGroup>,
    buffers: Vec<(Buffer<B>, U)>,
}
impl<B: Backend> Drop for GroupInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.pipeline
                .0
                .owner
                .device
                .destroy_bind_group(self.raw.take().unwrap())
        };
    }
}
pub struct ComputeBindings<B: Backend>(Rc<GroupInner<B>>);
impl<B: Backend> Clone for ComputeBindings<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<B: Backend> Gpu<B> {
    pub fn create_compute(&self, wgsl: &str, entry: &str) -> Result<ComputePipeline<B>> {
        self.owner.healthy()?;
        let module = naga::front::wgsl::parse_str(wgsl).map_err(backend_error)?;
        // An explicit supported subset, not unchecked acceptance of arbitrary
        // shader features. Production reflection can extend these contracts.
        if !module.overrides.is_empty() {
            return Err(Error::Rejected("pipeline overrides not implemented"));
        }
        let ep = module
            .entry_points
            .iter()
            .find(|e| e.name == entry && e.stage == naga::ShaderStage::Compute)
            .ok_or(Error::Rejected("compute entry point"))?;
        let limits = &self.owner.limits;
        let [x, y, z] = ep.workgroup_size;
        if x == 0
            || y == 0
            || z == 0
            || x > limits.max_compute_workgroup_size_x
            || y > limits.max_compute_workgroup_size_y
            || z > limits.max_compute_workgroup_size_z
            || u64::from(x) * u64::from(y) * u64::from(z)
                > u64::from(limits.max_compute_invocations_per_workgroup)
        {
            return Err(Error::Rejected("workgroup size"));
        }
        let info = wgpu_naga_bridge::create_validator(
            self.owner.features,
            self.owner.downlevel,
            naga::valid::ValidationFlags::all(),
        )
        .validate(&module)
        .map_err(backend_error)?;
        let mut layouter = naga::proc::Layouter::default();
        layouter.update(module.to_ctx()).map_err(backend_error)?;
        let mut slots = Vec::new();
        for (_, var) in module.global_variables.iter() {
            let Some(binding) = var.binding else {
                if var.space == naga::AddressSpace::WorkGroup {
                    return Err(Error::Rejected("workgroup storage not implemented"));
                }
                continue;
            };
            if binding.group != 0 || binding.binding >= limits.max_bindings_per_bind_group {
                return Err(Error::Rejected("compute binding group/range"));
            }
            let naga::AddressSpace::Storage { access } = var.space else {
                return Err(Error::Rejected(
                    "only storage-buffer compute bindings are implemented",
                ));
            };
            let min_bytes = u64::from(layouter[var.ty].size);
            if min_bytes == 0 || min_bytes > u64::from(limits.max_storage_buffer_binding_size) {
                return Err(Error::Rejected("compute storage binding size"));
            }
            slots.push(StorageSlot {
                binding: binding.binding,
                read_only: !access.contains(naga::StorageAccess::STORE),
                min_bytes,
            });
        }
        slots.sort_by_key(|s| s.binding);
        if slots.len() > limits.max_storage_buffers_per_shader_stage as usize
            || slots.windows(2).any(|w| w[0].binding == w[1].binding)
        {
            return Err(Error::Rejected("compute storage binding count"));
        }
        let entries: Vec<_> = slots
            .iter()
            .map(|s| types::BindGroupLayoutEntry {
                binding: s.binding,
                visibility: types::ShaderStages::COMPUTE,
                ty: types::BindingType::Buffer {
                    ty: types::BufferBindingType::Storage {
                        read_only: s.read_only,
                    },
                    has_dynamic_offset: false,
                    min_binding_size: NonZeroU64::new(s.min_bytes),
                },
                count: None,
            })
            .collect();
        let mut inner = PipelineInner {
            owner: self.owner.clone(),
            id: id()?,
            slots,
            pipeline: None,
            layout: None,
            group_layout: None,
            shader: None,
        };
        inner.group_layout = Some(
            unsafe {
                self.owner
                    .device
                    .create_bind_group_layout(&hal::BindGroupLayoutDescriptor {
                        label: Some("native storage compute"),
                        flags: hal::BindGroupLayoutFlags::empty(),
                        entries: &entries,
                    })
            }
            .map_err(|e| self.owner.device_error(e))?,
        );
        inner.layout = Some(
            unsafe {
                self.owner
                    .device
                    .create_pipeline_layout(&hal::PipelineLayoutDescriptor {
                        label: Some("native storage compute"),
                        flags: hal::PipelineLayoutFlags::empty(),
                        bind_group_layouts: &[inner.group_layout.as_ref()],
                        immediate_size: 0,
                    })
            }
            .map_err(|e| self.owner.device_error(e))?,
        );
        inner.shader = Some(
            unsafe {
                self.owner.device.create_shader_module(
                    &hal::ShaderModuleDescriptor {
                        label: Some("native storage compute"),
                        runtime_checks: types::ShaderRuntimeChecks::checked(),
                    },
                    hal::ShaderInput::Naga(hal::NagaShader {
                        module: Cow::Owned(module),
                        info,
                        debug_source: None,
                    }),
                )
            }
            .map_err(|e| self.owner.shader_error(e))?,
        );
        inner.pipeline = Some(
            unsafe {
                self.owner
                    .device
                    .create_compute_pipeline(&hal::ComputePipelineDescriptor {
                        label: Some("native storage compute"),
                        layout: inner.layout.as_ref().unwrap(),
                        stage: hal::ProgrammableStage {
                            module: inner.shader.as_ref().unwrap(),
                            entry_point: entry,
                            constants: &Default::default(),
                            zero_initialize_workgroup_memory: true,
                        },
                        cache: None,
                    })
            }
            .map_err(|e| self.owner.pipeline_error(e))?,
        );
        Ok(ComputePipeline(Rc::new(inner)))
    }
    pub fn bind_compute(
        &self,
        pipeline: &ComputePipeline<B>,
        bindings: &[StorageBinding<'_, B>],
    ) -> Result<ComputeBindings<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &pipeline.0.owner) {
            return Err(Error::Rejected("compute pipeline device"));
        }
        let mut sorted: Vec<_> = bindings.iter().collect();
        sorted.sort_by_key(|b| b.binding);
        if sorted.len() != pipeline.0.slots.len() {
            return Err(Error::Rejected("compute binding count"));
        }
        let mut buffers = Vec::new();
        let mut raw_buffers = Vec::new();
        let mut entries = Vec::new();
        let mut scope = BTreeMap::new();
        for (index, (binding, slot)) in sorted.into_iter().zip(&pipeline.0.slots).enumerate() {
            if binding.binding != slot.binding || !Rc::ptr_eq(&self.owner, &binding.buffer.0.owner)
            {
                return Err(Error::Rejected("compute binding index/device"));
            }
            let usage = if slot.read_only {
                U::STORAGE_READ_ONLY
            } else {
                U::STORAGE_READ_WRITE
            };
            if !binding.buffer.0.usage.contains(usage)
                || binding.size < slot.min_bytes
                || binding.size > u64::from(self.owner.limits.max_storage_buffer_binding_size)
                || !binding.offset.is_multiple_of(u64::from(
                    self.owner.limits.min_storage_buffer_offset_alignment,
                ))
            {
                return Err(Error::Rejected("compute binding usage/size/alignment"));
            }
            binding.buffer.range(binding.offset, binding.size)?;
            if let Some(previous) = scope.insert(binding.buffer.id(), usage) {
                if previous != U::STORAGE_READ_ONLY || usage != U::STORAGE_READ_ONLY {
                    return Err(Error::Rejected(
                        "writable compute binding aliases another binding",
                    ));
                }
            }
            buffers.push((binding.buffer.clone(), usage));
            raw_buffers.push(hal::BufferBinding::new_unchecked(
                binding.buffer.raw(),
                binding.offset,
                NonZeroU64::new(binding.size),
            ));
            entries.push(hal::BindGroupEntry {
                binding: binding.binding,
                resource_index: index as u32,
                count: 1,
            });
        }
        let group_id = id()?;
        let raw = unsafe {
            self.owner
                .device
                .create_bind_group(&hal::BindGroupDescriptor {
                    label: Some("native storage bindings"),
                    layout: pipeline.0.group_layout.as_ref().unwrap(),
                    buffers: &raw_buffers,
                    samplers: &[],
                    textures: &[],
                    entries: &entries,
                    acceleration_structures: &[],
                    external_textures: &[],
                })
        }
        .map_err(|e| self.owner.device_error(e))?;
        Ok(ComputeBindings(Rc::new(GroupInner {
            id: group_id,
            pipeline: pipeline.clone(),
            raw: Some(raw),
            buffers,
        })))
    }
}

impl<B: Backend + 'static> Encoder<B> {
    pub fn dispatch(
        &mut self,
        pipeline: &ComputePipeline<B>,
        bindings: &ComputeBindings<B>,
        groups: [u32; 3],
    ) -> Result<()> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &pipeline.0.owner)
            || !Rc::ptr_eq(&self.owner, &bindings.0.pipeline.0.owner)
            || pipeline.0.slots != bindings.0.pipeline.0.slots
        {
            return Err(Error::Rejected("compute pipeline/bindings mismatch"));
        }
        if groups
            .iter()
            .any(|&g| g > self.owner.limits.max_compute_workgroups_per_dimension)
        {
            return Err(Error::Rejected("compute dispatch dimensions"));
        }
        if groups.contains(&0) {
            return Ok(());
        }
        // The immutable group has already proven the whole dispatch usage scope
        // and ranges. Transition before entering the compute encoder.
        for (buffer, usage) in &bindings.0.buffers {
            self.transition(buffer, *usage);
        }
        self.holds
            .entry(pipeline.0.id)
            .or_insert_with(|| pipeline.0.clone());
        self.holds
            .entry(bindings.0.id)
            .or_insert_with(|| bindings.0.clone());
        unsafe {
            let raw = self.raw.as_mut().unwrap();
            raw.begin_compute_pass(&hal::ComputePassDescriptor {
                label: Some("native compute"),
                timestamp_writes: None,
            });
            raw.set_compute_pipeline(pipeline.0.pipeline.as_ref().unwrap());
            raw.set_bind_group(
                pipeline.0.layout.as_ref().unwrap(),
                0,
                bindings.0.raw.as_ref().unwrap(),
                &[],
            );
            raw.dispatch(groups);
            raw.end_compute_pass();
        }
        Ok(())
    }
}

mod general;
pub use general::{ComputeDispatch, ComputeStats, SharedComputePipeline};
