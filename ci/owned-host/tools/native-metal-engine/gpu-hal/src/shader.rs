//! Shared Naga29 validation and explicit pipeline-resource/interface contracts.
use super::bindings::PipelineLayout;
use super::*;
use std::borrow::Cow;
mod color_outputs;
mod sample_mask;
use types::{
    BindingType as Ty, BufferBindingType as Bt, TextureSampleType as Sample,
    TextureViewDimension as V,
};

pub(crate) struct ShaderInner<B: Backend> {
    pub(crate) owner: Rc<Owner<B>>,
    raw: Option<B::ShaderModule>,
    id: u64,
    pub(crate) module: naga::Module,
    pub(crate) info: naga::valid::ModuleInfo,
}
impl<B: Backend> Drop for ShaderInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner
                .device
                .destroy_shader_module(self.raw.take().unwrap());
        }
    }
}
pub struct Shader<B: Backend>(pub(crate) Rc<ShaderInner<B>>);
impl<B: Backend> Clone for Shader<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> Shader<B> {
    /// Minimum physical binding spans for this entry. Zero/unspecified layout
    /// minima are deferred to each draw/dispatch, never treated as proof.
    pub(crate) fn buffer_requirements(&self, index: usize) -> Result<Vec<(u32, u32, u64)>> {
        let module = &self.0.module;
        let info = self.0.info.get_entry_point(index);
        let mut sizes = naga::proc::Layouter::default();
        sizes.update(module.to_ctx()).map_err(backend_error)?;
        Ok(module.global_variables.iter().filter_map(|(h, v)| {
            if info[h].is_empty() || !matches!(v.space, naga::AddressSpace::Uniform | naga::AddressSpace::Storage { .. }) { return None; }
            v.binding.map(|b| (b.group, b.binding, u64::from(sizes[v.ty].size)))
        }).collect())
    }
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub(crate) fn raw(&self) -> &B::ShaderModule {
        self.0.raw.as_ref().unwrap()
    }
    pub(crate) fn entry(&self, name: &str, stage: naga::ShaderStage) -> Result<usize> {
        self.0
            .module
            .entry_points
            .iter()
            .position(|p| p.name == name && p.stage == stage)
            .ok_or(Error::Rejected("shader entry point/stage"))
    }
    pub(crate) fn check_layout(&self, index: usize, layout: &PipelineLayout<B>) -> Result<()> {
        if !Rc::ptr_eq(&self.0.owner, &layout.0.owner) {
            return Err(Error::Rejected("shader/layout device"));
        }
        let module = &self.0.module;
        let info = self.0.info.get_entry_point(index);
        let stage = wgpu_naga_bridge::map_naga_stage(module.entry_points[index].stage);
        let mut sizes = naga::proc::Layouter::default();
        sizes.update(module.to_ctx()).map_err(backend_error)?;
        for (handle, var) in module.global_variables.iter() {
            if info[handle].is_empty() {
                continue;
            }
            let Some(binding) = var.binding else {
                continue;
            };
            let slot = layout
                .entry(binding.group, binding.binding)
                .ok_or(Error::Rejected("shader resource absent from layout"))?;
            if !slot.visibility.contains(stage) {
                return Err(Error::Rejected("shader resource visibility"));
            }
            let compatible = match (&module.types[var.ty].inner, var.space, slot.ty) {
                (
                    _,
                    naga::AddressSpace::Uniform,
                    Ty::Buffer {
                        ty: Bt::Uniform,
                        min_binding_size,
                        ..
                    },
                ) => min_binding_size.is_none_or(|n| n.get() >= u64::from(sizes[var.ty].size)),
                (
                    _,
                    naga::AddressSpace::Storage { access },
                    Ty::Buffer {
                        ty: Bt::Storage { read_only },
                        min_binding_size,
                        ..
                    },
                ) => {
                    (!access.contains(naga::StorageAccess::STORE) || !read_only)
                        && min_binding_size
                            .is_none_or(|n| n.get() >= u64::from(sizes[var.ty].size))
                }
                (naga::TypeInner::Sampler { comparison }, _, Ty::Sampler(kind)) => {
                    *comparison == (kind == types::SamplerBindingType::Comparison)
                }
                (
                    naga::TypeInner::Image {
                        dim,
                        arrayed,
                        class,
                    },
                    _,
                    Ty::Texture {
                        sample_type,
                        view_dimension,
                        multisampled,
                    },
                ) => {
                    let dimension = match (dim, arrayed) {
                        (naga::ImageDimension::D1, false) => Some(V::D1),
                        (naga::ImageDimension::D2, false) => Some(V::D2),
                        (naga::ImageDimension::D2, true) => Some(V::D2Array),
                        (naga::ImageDimension::D3, false) => Some(V::D3),
                        (naga::ImageDimension::Cube, false) => Some(V::Cube),
                        (naga::ImageDimension::Cube, true) => Some(V::CubeArray),
                        _ => None,
                    };
                    dimension == Some(view_dimension)
                        && match class {
                            naga::ImageClass::Sampled { kind, multi } => {
                                *multi == multisampled
                                    && matches!(
                                        (kind, sample_type),
                                        (naga::ScalarKind::Float, Sample::Float { .. })
                                            | (naga::ScalarKind::Sint, Sample::Sint)
                                            | (naga::ScalarKind::Uint, Sample::Uint)
                                    )
                            }
                            naga::ImageClass::Depth { multi } => {
                                *multi == multisampled && sample_type == Sample::Depth
                            }
                            _ => false,
                        }
                }
                _ => false,
            };
            if !compatible {
                return Err(Error::Rejected(
                    "shader resource/layout type or minimum size",
                ));
            }
        }
        for pair in &info.sampling_set {
            let image = module.global_variables[pair.image]
                .binding
                .ok_or(Error::Rejected("unbound sampled image"))?;
            let sampler = module.global_variables[pair.sampler]
                .binding
                .ok_or(Error::Rejected("unbound sampler"))?;
            let image = layout.entry(image.group, image.binding).unwrap();
            let sampler = layout.entry(sampler.group, sampler.binding).unwrap();
            if matches!(
                sampler.ty,
                Ty::Sampler(types::SamplerBindingType::Filtering)
            ) && matches!(
                image.ty,
                Ty::Texture {
                    sample_type: Sample::Float { filterable: false }
                        | Sample::Sint
                        | Sample::Uint
                        | Sample::Depth,
                    ..
                }
            ) {
                return Err(Error::Rejected(
                    "filtering sampler with unfilterable shader texture",
                ));
            }
        }
        Ok(())
    }
}
impl<B: Backend> Gpu<B> {
    /// Entry-point reflection matching the pinned core29 auto-layout rules.
    /// Layouts remain immutable; explicit layouts keep their original identity.
    pub fn inferred_layout(&self, stages: &[(&Shader<B>, &str, types::ShaderStages)]) -> Result<(PipelineLayout<B>, Vec<crate::bindings::GroupLayout<B>>)> {
        self.owner.healthy()?;
        let mut groups: BTreeMap<u32, BTreeMap<u32, types::BindGroupLayoutEntry>> = BTreeMap::new();
        for &(shader, name, visibility) in stages {
            if !Rc::ptr_eq(&self.owner, &shader.0.owner) { return Err(Error::Rejected("inferred layout shader device")); }
            let stage=match visibility {types::ShaderStages::VERTEX=>naga::ShaderStage::Vertex,types::ShaderStages::FRAGMENT=>naga::ShaderStage::Fragment,types::ShaderStages::COMPUTE=>naga::ShaderStage::Compute,_=>return Err(Error::Rejected("inferred entry stage"))};
            let index=shader.entry(name,stage)?;
            let module=&shader.0.module;
            let info=shader.0.info.get_entry_point(index);
            let mut sizes=naga::proc::Layouter::default();sizes.update(module.to_ctx()).map_err(backend_error)?;
            for (handle,v) in module.global_variables.iter() {
                if info[handle].is_empty(){continue;}
                let Some(binding)=v.binding else {continue;};
                if binding.group>=self.owner.limits.max_bind_groups {return Err(Error::Rejected("inferred group index"));}
                let ty=match v.space {
                    naga::AddressSpace::Uniform|naga::AddressSpace::Storage{..}=>Ty::Buffer{
                        ty:match v.space {naga::AddressSpace::Uniform=>Bt::Uniform,naga::AddressSpace::Storage{access}=>Bt::Storage{read_only:!access.contains(naga::StorageAccess::STORE)},_=>unreachable!()},
                        has_dynamic_offset:false,min_binding_size:NonZeroU64::new(u64::from(sizes[v.ty].size))},
                    _=>match module.types[v.ty].inner {
                        naga::TypeInner::Sampler{comparison}=>Ty::Sampler(if comparison{types::SamplerBindingType::Comparison}else{types::SamplerBindingType::Filtering}),
                        naga::TypeInner::Image{dim,arrayed,class}=>{
                            let view_dimension=match(dim,arrayed){(naga::ImageDimension::D1,false)=>V::D1,(naga::ImageDimension::D2,false)=>V::D2,(naga::ImageDimension::D2,true)=>V::D2Array,(naga::ImageDimension::D3,false)=>V::D3,(naga::ImageDimension::Cube,false)=>V::Cube,(naga::ImageDimension::Cube,true)=>V::CubeArray,_=>return Err(Error::Rejected("inferred image dimension"))};
                            let(sample_type,multisampled)=match class{
                                naga::ImageClass::Sampled{kind,multi}=>(match kind{naga::ScalarKind::Float=>Sample::Float{filterable:info.sampling_set.iter().any(|p|p.image==handle)},naga::ScalarKind::Sint=>Sample::Sint,naga::ScalarKind::Uint=>Sample::Uint,_=>return Err(Error::Rejected("inferred image scalar"))},multi),
                                naga::ImageClass::Depth{multi}=>(Sample::Depth,multi),_=>return Err(Error::Rejected("inferred storage image"))};
                            Ty::Texture{sample_type,view_dimension,multisampled}
                        },_=>return Err(Error::Rejected("inferred resource class")),
                    }
                };
                let group=groups.entry(binding.group).or_default();
                if let Some(old)=group.get_mut(&binding.binding){if old.ty!=ty{return Err(Error::Rejected("inconsistent inferred binding type"));}old.visibility|=visibility;}
                else{group.insert(binding.binding,types::BindGroupLayoutEntry{binding:binding.binding,visibility,ty,count:None});}
            }
        }
        let count=groups.keys().next_back().map_or(0,|n|n+1);
        let layouts=(0..count).map(|i|self.group_layout(&groups.remove(&i).unwrap_or_default().into_values().collect::<Vec<_>>())).collect::<Result<Vec<_>>>()?;
        Ok((self.pipeline_layout(&layouts)?,layouts))
    }
    pub fn shader(&self, wgsl: &str) -> Result<Shader<B>> {
        self.shader_impl(wgsl, None)
    }
    /// Resolve named/id overrides once, before validation, reflection and native
    /// creation. The resulting immutable shader identity includes the variant.
    pub fn shader_specialized(&self, wgsl: &str, constants: &[(&str, f64)]) -> Result<Shader<B>> {
        self.shader_impl(wgsl, Some(constants))
    }
    fn shader_impl(&self, wgsl: &str, constants: Option<&[(&str, f64)]>) -> Result<Shader<B>> {
        self.owner.healthy()?;
        let mut module = naga::front::wgsl::parse_str(wgsl).map_err(backend_error)?;
        if constants.is_none() && !module.overrides.is_empty() {
            return Err(Error::Rejected("explicit shader specialization required"));
        }
        if module
                .global_variables
                .iter()
                .any(|(_, v)| v.space == naga::AddressSpace::WorkGroup)
        {
            return Err(Error::Rejected(
                "workgroup storage not implemented",
            ));
        }
        let validate = |module: &naga::Module| {
            wgpu_naga_bridge::create_validator(
                self.owner.features,
                self.owner.downlevel,
                naga::valid::ValidationFlags::all(),
            )
            .validate(module)
            .map_err(backend_error)
        };
        let mut info = validate(&module)?;
        if let Some(constants) = constants {
            if constants.len() > 256 {
                return Err(Error::Rejected("shader specialization budget"));
            }
            let mut values = naga::back::PipelineConstants::default();
            for &(name, value) in constants {
                if !value.is_finite() || values.insert(name.to_owned(), value).is_some() {
                    return Err(Error::Rejected("duplicate/nonfinite shader specialization"));
                }
            }
            let specialized =
                naga::back::pipeline_constants::process_overrides(&module, &info, None, &values)
                    .map_err(backend_error)?;
            module = specialized.0.into_owned();
            // Naga29 keeps resolved override declarations despite the helper's
            // stale empty-arena documentation. Drop only unreachable declarations
            // through its IR compactor; never clear referenced handles by hand.
            naga::compact::compact(&mut module, naga::compact::KeepUnused::No);
            if !module.overrides.is_empty() {
                return Err(Error::Rejected("unresolved shader specialization"));
            }
            info = validate(&module)?;
        }
        self.shader_module(module, info)
    }
    fn shader_module(
        &self,
        module: naga::Module,
        info: naga::valid::ModuleInfo,
    ) -> Result<Shader<B>> {
        self.owner.healthy()?;
        let identity = id()?;
        let raw = unsafe {
            self.owner.device.create_shader_module(
                &hal::ShaderModuleDescriptor {
                    label: Some("native validated shader"),
                    runtime_checks: types::ShaderRuntimeChecks::checked(),
                },
                hal::ShaderInput::Naga(hal::NagaShader {
                    module: Cow::Owned(module.clone()),
                    info: info.clone(),
                    debug_source: None,
                }),
            )
        }
        .map_err(|e| self.owner.shader_error(e))?;
        Ok(Shader(Rc::new(ShaderInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            id: identity,
            module,
            info,
        })))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct InterfaceType {
    pub kind: naga::ScalarKind,
    pub width: u8,
    pub lanes: u8,
    interpolation: Option<naga::Interpolation>,
    sampling: Option<naga::Sampling>,
}
#[derive(Default)]
pub(crate) struct Interface {
    pub locations: BTreeMap<u32, InterfaceType>,
    pub builtins: Vec<naga::BuiltIn>,
}
pub(crate) fn io(
    module: &naga::Module,
    ty: naga::Handle<naga::Type>,
    binding: Option<&naga::Binding>,
    out: &mut Interface,
) -> Result<()> {
    if let Some(binding) = binding {
        match binding {
            naga::Binding::BuiltIn(builtin) => out.builtins.push(*builtin),
            naga::Binding::Location {
                location,
                interpolation,
                sampling,
                blend_src,
                per_primitive,
            } => {
                if blend_src.is_some() || *per_primitive {
                    return Err(Error::Rejected(
                        "dual-source/per-primitive IO not implemented",
                    ));
                }
                let (scalar, lanes) = match module.types[ty].inner {
                    naga::TypeInner::Scalar(s) => (s, 1),
                    naga::TypeInner::Vector { size, scalar } => (scalar, size as u8),
                    _ => return Err(Error::Rejected("shader IO type not implemented")),
                };
                let value = InterfaceType {
                    kind: scalar.kind,
                    width: scalar.width,
                    lanes,
                    interpolation: *interpolation,
                    sampling: *sampling,
                };
                if out.locations.insert(*location, value).is_some() {
                    return Err(Error::Rejected("duplicate shader IO location"));
                }
            }
        }
    } else if let naga::TypeInner::Struct { members, .. } = &module.types[ty].inner {
        for member in members {
            io(module, member.ty, member.binding.as_ref(), out)?;
        }
    } else {
        return Err(Error::Rejected("unbound shader IO"));
    }
    Ok(())
}
pub(crate) fn inputs(module: &naga::Module, ep: &naga::EntryPoint) -> Result<Interface> {
    let mut out = Interface::default();
    for a in &ep.function.arguments {
        io(module, a.ty, a.binding.as_ref(), &mut out)?;
    }
    Ok(out)
}
pub(crate) fn outputs(module: &naga::Module, ep: &naga::EntryPoint) -> Result<Interface> {
    let mut out = Interface::default();
    if let Some(r) = &ep.function.result {
        io(module, r.ty, r.binding.as_ref(), &mut out)?;
    }
    Ok(out)
}
