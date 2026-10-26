//! Backed, encoder-specific upload reservations. Ordinary uploads cannot spend
//! these bytes. A reservation is consumed exactly once or released on abandon.
use super::*;

pub(super) struct Book<B: Backend> {
    open: Cell<bool>,
    next: Cell<u64>,
    bytes: Cell<u64>,
    leases: RefCell<BTreeMap<u64, Buffer<B>>>,
}
impl<B: Backend> Book<B> {
    fn new() -> Self {
        Self {
            open: Cell::new(true),
            next: Cell::new(1),
            bytes: Cell::new(0),
            leases: RefCell::new(BTreeMap::new()),
        }
    }
    pub(super) fn bytes(&self) -> u64 {
        self.bytes.get()
    }
    pub(super) fn is_empty(&self) -> bool {
        self.leases.borrow().is_empty()
    }
    pub(super) fn close(&self) {
        self.open.set(false);
        self.leases.borrow_mut().clear();
        self.bytes.set(0);
    }
    fn remove(&self, id: u64) -> Option<Buffer<B>> {
        let lease = self.leases.borrow_mut().remove(&id)?;
        self.bytes.set(self.bytes.get() - lease.size());
        Some(lease)
    }
}

/// This token owns a real, private staging allocation, not an optimistic byte
/// estimate. It cannot be cloned or moved to another encoder. Dropping the
/// token or its encoder abandons the reservation without submitting anything.
#[must_use]
pub struct UploadReservation<B: Backend> {
    book: Rc<Book<B>>,
    id: Option<u64>,
}
impl<B: Backend> UploadReservation<B> {
    pub fn capacity(&self) -> u64 {
        self.id
            .and_then(|id| self.book.leases.borrow().get(&id).map(Buffer::size))
            .unwrap_or(0)
    }
    fn validate(&self, encoder: &Encoder<B>) -> Result<u64> {
        if !self.book.open.get()
            || !encoder
                .reservations
                .as_ref()
                .is_some_and(|b| Rc::ptr_eq(b, &self.book))
        {
            return Err(Error::Rejected("upload reservation encoder"));
        }
        let id = self
            .id
            .ok_or(Error::Rejected("upload reservation already consumed"))?;
        if !self.book.leases.borrow().contains_key(&id) {
            return Err(Error::Rejected("upload reservation abandoned"));
        }
        Ok(id)
    }
    fn take(&mut self, encoder: &Encoder<B>) -> Result<Buffer<B>> {
        let id = self.validate(encoder)?;
        let lease = self
            .book
            .remove(id)
            .ok_or(Error::Rejected("upload reservation absent"))?;
        self.id = None;
        Ok(lease)
    }
}
impl<B: Backend> Drop for UploadReservation<B> {
    fn drop(&mut self) {
        if let Some(id) = self.id.take() {
            self.book.remove(id);
        }
    }
}

impl<B: Backend> Encoder<B> {
    pub fn ordered_reserved_bytes(&self) -> u64 {
        self.reservations.as_ref().map_or(0, |b| b.bytes())
    }
}

impl<B: Backend + 'static> Gpu<B> {
    /// Acquire backing before accepting CPU draws. Failure does not change an
    /// existing reservation or recorded commands. Global resident/object bounds
    /// include this buffer immediately, so other uploads cannot steal its space.
    pub fn reserve_ordered_upload(
        &mut self,
        encoder: &mut Encoder<B>,
        bytes: u64,
    ) -> Result<UploadReservation<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &encoder.owner) {
            return Err(Error::Rejected("upload reservation device"));
        }
        if bytes == 0 || !bytes.is_multiple_of(4) {
            return Err(Error::Rejected("upload reservation size/alignment"));
        }
        let remaining = self
            .owner
            .config
            .max_pending_upload_bytes
            .checked_sub(encoder.ordered_staging_bytes)
            .and_then(|n| n.checked_sub(encoder.ordered_reserved_bytes()))
            .ok_or(Error::Rejected("ordered reservation accounting"))?;
        if bytes > remaining {
            return Err(Error::Rejected("ordered upload encoder byte budget"));
        }
        let id = encoder.reservations.as_ref().map_or(1, |b| b.next.get());
        let next = id
            .checked_add(1)
            .ok_or(Error::Rejected("upload reservation identity exhaustion"))?;
        let ceiling = bytes
            .checked_next_power_of_two()
            .unwrap_or(bytes)
            .min(remaining)
            .min(self.owner.config.max_staging_buffer_bytes)
            .min(self.owner.limits.max_buffer_size);
        if ceiling < bytes {
            return Err(Error::Rejected("staging buffer budget"));
        }
        // Reserve geometrically even with the retired pool disabled. Actual
        // capacity is charged; GPU-referenced allocations are never relocated.
        let lease = self.take_upload_limited(ceiling, ceiling)?;
        let book = encoder
            .reservations
            .get_or_insert_with(|| Rc::new(Book::new()))
            .clone();
        book.bytes.set(book.bytes.get() + lease.size());
        book.next.set(next);
        book.leases.borrow_mut().insert(id, lease);
        Ok(UploadReservation { book, id: Some(id) })
    }

    /// Growth retains the old backing until a replacement has been acquired.
    /// A failed growth therefore still permits flushing every previously
    /// admitted draw. It never moves a buffer referenced by GPU commands.
    pub fn grow_ordered_upload(
        &mut self,
        encoder: &mut Encoder<B>,
        reservation: &mut UploadReservation<B>,
        bytes: u64,
    ) -> Result<()> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &encoder.owner) {
            return Err(Error::Rejected("upload reservation device"));
        }
        let id = reservation.validate(encoder)?;
        if !bytes.is_multiple_of(4) {
            return Err(Error::Rejected("upload reservation size/alignment"));
        }
        let old = reservation.capacity();
        if bytes <= old {
            return Ok(());
        }
        let others = encoder.ordered_reserved_bytes() - old;
        let remaining = self
            .owner
            .config
            .max_pending_upload_bytes
            .checked_sub(encoder.ordered_staging_bytes)
            .and_then(|n| n.checked_sub(others))
            .ok_or(Error::Rejected("ordered reservation accounting"))?;
        if bytes > remaining {
            return Err(Error::Rejected("ordered upload encoder byte budget"));
        }
        let ceiling = bytes
            .checked_next_power_of_two()
            .unwrap_or(bytes)
            .min(remaining)
            .min(self.owner.config.max_staging_buffer_bytes)
            .min(self.owner.limits.max_buffer_size);
        if ceiling < bytes {
            return Err(Error::Rejected("staging buffer budget"));
        }
        // Reserve geometrically even with the retired pool disabled. Actual
        // capacity is charged; GPU-referenced allocations are never relocated.
        let lease = self.take_upload_limited(ceiling, ceiling)?;
        let size = lease.size();
        let previous = reservation
            .book
            .leases
            .borrow_mut()
            .insert(id, lease)
            .expect("validated reservation");
        reservation.book.bytes.set(others + size);
        self.recycle_upload(previous);
        Ok(())
    }

    /// Validates all destinations before consuming the reservation. A size or
    /// owner error leaves it usable. There is no budget allocation at commit.
    pub fn write_buffers_reserved(
        &mut self,
        encoder: &mut Encoder<B>,
        reservation: &mut UploadReservation<B>,
        uploads: &[OrderedUpload<'_, B>],
    ) -> Result<()> {
        let bytes = self.validate_ordered_uploads(encoder, uploads)?;
        reservation.validate(encoder)?;
        if bytes > reservation.capacity() {
            return Err(Error::Rejected("upload exceeds reserved capacity"));
        }
        let lease = reservation.take(encoder)?;
        if bytes == 0 {
            self.recycle_upload(lease);
            return Ok(());
        }
        self.snapshot_ordered_uploads(encoder, uploads, bytes, lease)
    }
}
