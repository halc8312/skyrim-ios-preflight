//! One native encoder allocation for every transition command buffer in a
//! submission. It is reset/recycled only when ALL its command buffers return.
use super::*;
pub(crate) struct Arena<B: Backend> {
    raw: RefCell<Option<B::CommandEncoder>>,
    recording: Cell<bool>,
    pub(crate) returned: RefCell<Vec<B::CommandBuffer>>,
    owner: Rc<Owner<B>>,
}
impl<B: Backend> Drop for Arena<B> {
    fn drop(&mut self) {
        let mut raw = self.raw.get_mut().take().unwrap();
        if self.recording.get() {
            unsafe {
                raw.discard_encoding();
            }
        }
        unsafe {
            raw.reset_all(self.returned.get_mut().drain(..));
        }
        self.owner.recycle(raw);
    }
}
pub(crate) struct Fixups<B: Backend>(Rc<Arena<B>>);
impl<B: Backend> Fixups<B> {
    pub(crate) fn new(owner: Rc<Owner<B>>) -> Result<Self> {
        let mut encoder = Encoder::new(owner.clone())?;
        let raw = encoder.raw.take();
        Ok(Self(Rc::new(Arena {
            raw: RefCell::new(raw),
            recording: Cell::new(true),
            returned: RefCell::new(Vec::new()),
            owner,
        })))
    }
    pub(crate) fn encode(
        &self,
        buffers: BTreeMap<u64, Use<B>>,
        textures: BTreeMap<(u64, u32, u32), texture::TextureUse<B>>,
    ) -> Result<Commands<B>> {
        let mut lease = self.0.raw.borrow_mut();
        let raw = lease.as_mut().unwrap();
        if !self.0.recording.get() {
            unsafe { raw.begin_encoding(Some("native submission transitions")) }
                .map_err(|e| self.0.owner.device_error(e))?;
            self.0.recording.set(true);
        }
        // Batch all barriers in this phase, not one driver call per resource.
        if !buffers.is_empty() {
            unsafe {
                raw.transition_buffers(buffers.values().map(|u| hal::BufferBarrier {
                    buffer: u.buffer.raw(),
                    usage: hal::StateTransition {
                        from: u.first,
                        to: u.last,
                    },
                }));
            }
        }
        if !textures.is_empty() {
            unsafe {
                raw.transition_textures(textures.iter().map(|(&(_, mip, layer), u)| {
                    hal::TextureBarrier {
                        texture: u.texture.raw(),
                        range: texture::subresource(mip, layer),
                        usage: hal::StateTransition {
                            from: u.first,
                            to: u.last,
                        },
                    }
                }));
            }
        }
        let watch = B::watch(raw);
        self.0.recording.set(false);
        let commands = unsafe { raw.end_encoding() }.map_err(|e| self.0.owner.device_error(e))?;
        Ok(Commands {
            owner: self.0.owner.clone(),
            encoder: None,
            arena: Some(self.0.clone()),
            commands: Some(commands),
            watch,
            uses: buffers,
            texture_uses: textures,
            holds: BTreeMap::new(),
            ordered_staging: Vec::new(),
            submitted: false,
            terminal: false,
        })
    }
}
