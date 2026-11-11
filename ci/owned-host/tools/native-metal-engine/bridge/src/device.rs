use crate::*;
pub(crate) struct Core {
    pub gpu: NativeGpu,
    pub scopes: Vec<(ErrorFilter, Option<GpuError>)>,
    pub errors: Vec<GpuError>,
    pub error_count: u64,
    pub fatal: Option<Error>,
    pub prefix: Vec<native::Commands<Platform>>,
    pub texture_uploads: Option<native::Encoder<Platform>>,
    pub prefix_bytes: u64,
    pub serial: u64,
}
pub(crate) type Owner = Rc<RefCell<Core>>;
impl Core {
    pub fn write(
        &mut self,
        buffer: &native::Buffer<Platform>,
        offset: u64,
        data: &[u8],
    ) -> Result<()> {
        self.check()?;
        if offset % 4 != 0
            || data.len() % 4 != 0
            || offset
                .checked_add(data.len() as u64)
                .is_none_or(|end| end > buffer.size())
        {
            return Err(Error::Invalid("queue upload range/alignment".into()));
        }
        if data.is_empty() {
            return self
                .gpu
                .write_buffer(buffer, offset, data)
                .map_err(convert::err);
        }
        let mut cursor = 0;
        while cursor < data.len() {
            if self.gpu.pending_upload_remaining() < 4 {
                self.submit(Vec::new())?;
            }
            let room = self.gpu.pending_upload_remaining() & !3;
            if room == 0 {
                return Err(Error::Invalid("native upload budget below one word".into()));
            }
            let length = (data.len() - cursor).min(room as usize);
            self.gpu
                .write_buffer(
                    buffer,
                    offset + cursor as u64,
                    &data[cursor..cursor + length],
                )
                .map_err(convert::err)?;
            cursor += length;
        }
        Ok(())
    }
    pub fn check(&self) -> Result<()> {
        self.fatal.clone().map_or(Ok(()), Err)
    }
    pub fn report(&mut self, e: Error, fatal: bool) {
        if self.fatal.is_some() {
            return;
        }
        if fatal && self.fatal.is_none() {
            self.fatal = Some(e.clone());
        }
        let e = match e {
            Error::Gpu(g) => g,
            e => GpuError {
                ty: ErrorType::Validation,
                message: e.to_string(),
            },
        };
        let filter = if e.ty == ErrorType::OutOfMemory {
            ErrorFilter::OutOfMemory
        } else if e.ty == ErrorType::Internal {
            ErrorFilter::Internal
        } else {
            ErrorFilter::Validation
        };
        if let Some((_, slot)) = self.scopes.iter_mut().rev().find(|(f, _)| *f == filter) {
            if slot.is_none() {
                *slot = Some(e);
            }
            return;
        }
        self.error_count += 1;
        log::error!("native bridge: {e}");
        self.errors.push(e);
    }
    pub fn poll(&mut self) -> Result<u64> {
        self.check()?;
        match self.gpu.poll() {
            Ok(s) => Ok(s),
            Err(e) => {
                let e = convert::err(e);
                self.report(e.clone(), true);
                Err(e)
            }
        }
    }
    pub fn submit(&mut self, commands: Vec<native::Commands<Platform>>) -> Result<()> {
        self.check()?;
        if let Some(e) = self.texture_uploads.take() {
            self.prefix.push(e.finish().map_err(convert::err)?);
        }
        let mut all = std::mem::take(&mut self.prefix);
        self.prefix_bytes = 0;
        all.extend(commands);
        match self.gpu.submit(all) {
            Ok(s) => {
                self.serial = s;
                Ok(())
            }
            Err(e) => {
                let e = convert::err(e);
                self.report(e.clone(), true);
                Err(e)
            }
        }
    }
}
#[derive(Clone)]
pub struct Device {
    pub(crate) owner: Owner,
    pub(crate) queue: Queue,
}
#[derive(Clone)]
pub struct Queue {
    pub(crate) owner: Owner,
}
impl std::fmt::Debug for Device {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeDevice")
    }
}
impl std::fmt::Debug for Queue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeQueue")
    }
}
impl Device {
    pub(crate) fn new(gpu: NativeGpu) -> Self {
        let owner = Rc::new(RefCell::new(Core {
            gpu,
            scopes: Vec::new(),
            errors: Vec::new(),
            error_count: 0,
            fatal: None,
            prefix: Vec::new(),
            texture_uploads: None,
            prefix_bytes: 0,
            serial: 0,
        }));
        Self {
            queue: Queue {
                owner: owner.clone(),
            },
            owner,
        }
    }
    pub(crate) fn resource<T>(&self, f: impl FnOnce(&mut NativeGpu) -> Result<T>) -> Result<T> {
        let mut c = self.owner.borrow_mut();
        let result = c.check().and_then(|_| f(&mut c.gpu));
        if let Err(e) = &result {
            c.report(e.clone(), false);
        }
        result
    }
    pub fn queue(&self) -> &Queue {
        &self.queue
    }
    pub fn limits(&self) -> Limits {
        convert::limits(self.owner.borrow().gpu.limits())
    }
    pub fn features(&self) -> Vec<FeatureName> {
        feature_names(self.owner.borrow().gpu.features())
    }
    pub fn has_feature(&self, f: FeatureName) -> bool {
        self.features().contains(&f)
    }
    pub fn push_error_scope(&self, f: ErrorFilter) {
        self.owner.borrow_mut().scopes.push((f, None));
    }
    pub fn pop_error_scope(&self) -> Pending<Result<Option<GpuError>>> {
        Pending::ready(
            self.owner
                .borrow_mut()
                .scopes
                .pop()
                .map(|(_, e)| e)
                .ok_or_else(|| Error::ErrorScope("empty scope stack".into())),
        )
    }
    pub fn take_errors(&self) -> Vec<GpuError> {
        std::mem::take(&mut self.owner.borrow_mut().errors)
    }
    pub fn error_count(&self) -> u64 {
        self.owner.borrow().error_count
    }
    pub fn lost_reason(&self) -> Option<String> {
        self.owner.borrow().fatal.as_ref().map(ToString::to_string)
    }
    pub fn destroy(&self) {
        self.owner
            .borrow_mut()
            .report(Error::Invalid("device destroyed".into()), true);
    }
    pub fn poll(&self) {
        let _ = self.owner.borrow_mut().poll();
    }
    pub fn block_on<T>(&self, p: Pending<Result<T>>) -> Result<T> {
        pending::block(p)
    }
}
fn feature_names(f: types::Features) -> Vec<FeatureName> {
    let pairs = [
        (
            types::Features::TEXTURE_COMPRESSION_BC,
            FeatureName::TextureCompressionBC,
        ),
        (
            types::Features::TEXTURE_COMPRESSION_BC_SLICED_3D,
            FeatureName::TextureCompressionBCSliced3D,
        ),
        (
            types::Features::FLOAT32_FILTERABLE,
            FeatureName::Float32Filterable,
        ),
        (
            types::Features::DEPTH32FLOAT_STENCIL8,
            FeatureName::Depth32FloatStencil8,
        ),
        (
            types::Features::TEXTURE_FORMAT_16BIT_NORM,
            FeatureName(0x0003_000B),
        ),
    ];
    pairs
        .iter()
        .filter_map(|(bit, n)| f.contains(*bit).then_some(*n))
        .collect()
}
#[derive(Clone, Debug, Default)]
pub struct Instance;
#[derive(Clone)]
pub struct Adapter {
    info: types::AdapterInfo,
    limits: types::Limits,
    features: types::Features,
}
impl std::fmt::Debug for Adapter {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeAdapter")
    }
}
#[derive(Clone, Copy, Debug, Default)]
pub struct RequestAdapterOptions<'a> {
    pub power_preference: PowerPreference,
    pub force_fallback_adapter: bool,
    pub compatible_surface: Option<&'a Surface>,
}
#[derive(Clone, Debug)]
pub struct AdapterInfo {
    pub vendor: String,
    pub architecture: String,
    pub device: String,
    pub description: String,
    pub backend: BackendType,
    pub adapter_type: AdapterType,
    pub vendor_id: u32,
    pub device_id: u32,
}
/// Resident-memory ceilings of the live renderer's device. The HAL defaults (512 MiB each)
/// are development bounds sized for conformance fixtures; exceeding one rejects the allocation,
/// which the checked game renderer treats as fatal. The game's own budgets sit above them: the
/// texture cache alone may hold 512 MiB plus a 256 MiB reuse pool before it evicts, next to
/// supersampled 4x MSAA EDRAM targets (133 MiB per colour target at 3840x2160), and the guest
/// memory mirror may grow to the console's whole 512 MiB next to the index and constant rings.
/// These are refusal thresholds, not reservations: nothing is allocated up front.
fn live_config() -> native::Config {
    native::Config {
        max_resident_texture_bytes: 3 << 30,
        max_resident_buffer_bytes: 1 << 30,
        ..Default::default()
    }
}
impl Instance {
    pub fn new() -> Result<Self> {
        Ok(Self)
    }
    pub fn process_events(&self) {}
    pub fn block_on<T>(&self, p: Pending<Result<T>>) -> Result<T> {
        pending::block(p)
    }
    pub fn request_adapter(&self, options: &RequestAdapterOptions<'_>) -> Pending<Result<Adapter>> {
        let config = native::Config {
            adapter_limits: true,
            validation: std::env::var_os("SKYRIM_NATIVE_VALIDATION").is_some(),
            optional_texture_features: types::Features::TEXTURE_COMPRESSION_BC
                | types::Features::TEXTURE_COMPRESSION_BC_SLICED_3D
                | types::Features::FLOAT32_FILTERABLE
                | types::Features::TEXTURE_FORMAT_16BIT_NORM
                | types::Features::DEPTH32FLOAT_STENCIL8,
            ..live_config()
        };
        let result = NativeGpu::open_with_config(config)
            .map_err(convert::err)
            .and_then(|gpu| {
                if options.force_fallback_adapter
                    && gpu.adapter_info.device_type != types::DeviceType::Cpu
                {
                    return Err(Error::RequestAdapter(
                        "requested fallback adapter unavailable".into(),
                    ));
                }
                Ok(Adapter {
                    info: gpu.adapter_info.clone(),
                    limits: gpu.limits().clone(),
                    features: gpu.features(),
                })
            });
        Pending::ready(result)
    }
}
impl Adapter {
    pub fn info(&self) -> AdapterInfo {
        let i = &self.info;
        AdapterInfo {
            vendor: format!("{}", i.vendor),
            architecture: String::new(),
            device: i.name.clone(),
            description: i.driver_info.clone(),
            backend: match i.backend {
                types::Backend::Metal => BackendType::Metal,
                types::Backend::Vulkan => BackendType::Vulkan,
                _ => BackendType::Undefined,
            },
            adapter_type: match i.device_type {
                types::DeviceType::Cpu => AdapterType::CPU,
                types::DeviceType::IntegratedGpu => AdapterType::IntegratedGPU,
                types::DeviceType::DiscreteGpu => AdapterType::DiscreteGPU,
                _ => AdapterType::Unknown,
            },
            vendor_id: i.vendor,
            device_id: i.device,
        }
    }
    pub fn limits(&self) -> Limits {
        convert::limits(&self.limits)
    }
    pub fn features(&self) -> Vec<FeatureName> {
        feature_names(self.features)
    }
    pub fn has_feature(&self, f: FeatureName) -> bool {
        self.features().contains(&f)
    }
    pub fn request_device(&self, d: &DeviceDescriptor<'_>) -> Pending<Result<Device>> {
        let result = (|| {
            for &f in d.required_features {
                if !self.has_feature(f) {
                    return Err(Error::RequestDevice(format!(
                        "required feature {} unavailable",
                        f.0
                    )));
                }
            }
            if let Some(l) = &d.required_limits {
                convert::check_limits(l, &self.limits)?;
            }
            let mut requested = types::Features::empty();
            for &f in d.required_features {
                requested |= match f {
                    FeatureName::TextureCompressionBC => types::Features::TEXTURE_COMPRESSION_BC,
                    FeatureName::TextureCompressionBCSliced3D => {
                        types::Features::TEXTURE_COMPRESSION_BC_SLICED_3D
                    }
                    FeatureName::Float32Filterable => types::Features::FLOAT32_FILTERABLE,
                    FeatureName::Depth32FloatStencil8 => types::Features::DEPTH32FLOAT_STENCIL8,
                    FeatureName(0x0003_000B) => types::Features::TEXTURE_FORMAT_16BIT_NORM,
                    _ => return Err(Error::RequestDevice("feature mapping unavailable".into())),
                };
            }
            let gpu = NativeGpu::open_with_config(native::Config {
                // No explicit requirements must not invent desktop default
                // limits that the selected iOS/Simulator adapter cannot meet.
                // Explicit requirements were checked above; expose actual
                // adapter limits in both paths, with only requested features.
                adapter_limits: true,
                validation: std::env::var_os("SKYRIM_NATIVE_VALIDATION").is_some(),
                optional_texture_features: requested,
                ..live_config()
            })
            .map_err(convert::err)?;
            if gpu.adapter_info != self.info || !gpu.features().contains(requested) {
                return Err(Error::RequestDevice(
                    "adapter or feature availability changed".into(),
                ));
            }
            Ok(Device::new(gpu))
        })();
        Pending::ready(result)
    }
}
