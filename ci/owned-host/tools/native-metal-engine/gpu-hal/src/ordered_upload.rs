//! Engine-aware uploads at an exact command-stream position. The established
//! queue-prefix write_buffer semantics remain unchanged. Callers seal prior
//! recorded draws into the SAME encoder before refreshing their consumed bytes.
use super::*;

pub struct OrderedUpload<'a, B: Backend> {
    pub destination: &'a Buffer<B>,
    pub offset: u64,
    pub data: &'a [u8],
}
impl<B: Backend + 'static> Gpu<B> {
    /// Snapshot CPU bytes and encode the copy here without submitting. The host
    /// still owns higher-level cache invalidation and guest coherence.
    pub fn write_buffer_ordered(
        &mut self,
        encoder: &mut Encoder<B>,
        destination: &Buffer<B>,
        offset: u64,
        data: &[u8],
    ) -> Result<()> {
        self.write_buffers_ordered(
            encoder,
            &[OrderedUpload {
                destination,
                offset,
                data,
            }],
        )
    }
    /// Validate the ENTIRE batch before any allocation or command, then snapshot
    /// all sources into one private mapped lease. Overlapping destinations are
    /// applied in list order. No arbitrary next submission can take this lease:
    /// only this encoder retains it, through all commands and fence retirement.
    /// Abandonment frees it; unknown submit quarantines it. It can join the
    /// shared idle pool ONLY after its own flight and native commands retire.
    pub fn write_buffers_ordered(
        &mut self,
        encoder: &mut Encoder<B>,
        uploads: &[OrderedUpload<'_, B>],
    ) -> Result<()> {
        let bytes = self.validate_ordered_uploads(encoder, uploads)?;
        let remaining = self
            .owner
            .config
            .max_pending_upload_bytes
            .checked_sub(encoder.ordered_staging_bytes)
            .and_then(|n| n.checked_sub(encoder.ordered_reserved_bytes()))
            .ok_or(Error::Rejected("ordered staging accounting"))?;
        if bytes > remaining {
            return Err(Error::Rejected("ordered upload encoder byte budget"));
        }
        if bytes == 0 {
            return Ok(());
        }
        // Charge actual retained capacity, never just payload bytes. Ordered
        // traffic may borrow at most its own power-of-two bucket, not a4x lease.
        let ceiling = bytes
            .checked_next_power_of_two()
            .unwrap_or(bytes)
            .min(remaining);
        let staging = self.take_upload_limited(bytes, ceiling)?;
        self.snapshot_ordered_uploads(encoder, uploads, bytes, staging)
    }
    pub(crate) fn validate_ordered_uploads(
        &self,
        encoder: &Encoder<B>,
        uploads: &[OrderedUpload<'_, B>],
    ) -> Result<u64> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &encoder.owner) {
            return Err(Error::Rejected("ordered upload encoder device"));
        }
        if uploads.len() > 8192 {
            return Err(Error::Rejected("ordered upload batch count"));
        }
        let mut bytes = 0u64;
        for u in uploads {
            encoder.check(u.destination, U::COPY_DST)?;
            u.destination.range(u.offset, u.data.len() as u64)?;
            bytes = bytes
                .checked_add(u.data.len() as u64)
                .ok_or(Error::Rejected("ordered upload size overflow"))?;
        }
        Ok(bytes)
    }
    // Both ordinary and reserved commits share the same prevalidated snapshot
    // path. The caller has charged a real lease before entering this function.
    pub(crate) fn snapshot_ordered_uploads(
        &mut self,
        encoder: &mut Encoder<B>,
        uploads: &[OrderedUpload<'_, B>],
        bytes: u64,
        staging: Buffer<B>,
    ) -> Result<()> {
        let reserved = encoder.ordered_staging_bytes + staging.size();
        let total = encoder.ordered_upload_bytes + bytes;
        let range = 0..staging.size();
        let mapped = unsafe { self.owner.device.map_buffer(staging.raw(), range.clone()) }
            .map_err(|e| self.owner.device_error(e))?;
        unsafe {
            let mut at = 0;
            for u in uploads {
                std::ptr::copy_nonoverlapping(
                    u.data.as_ptr(),
                    mapped.ptr.as_ptr().add(at),
                    u.data.len(),
                );
                at += u.data.len();
            }
            if !mapped.is_coherent {
                self.owner
                    .device
                    .flush_mapped_ranges(staging.raw(), std::iter::once(range));
            }
            self.owner.device.unmap_buffer(staging.raw());
        }
        staging.0.state.set(U::MAP_WRITE);
        let mut at = 0;
        for u in uploads {
            if !u.data.is_empty() {
                encoder.copy_buffer(&staging, at, u.destination, u.offset, u.data.len() as u64)?;
            }
            at += u.data.len() as u64;
        }
        encoder.ordered_staging.push(staging);
        encoder.ordered_upload_bytes = total;
        encoder.ordered_staging_bytes = reserved;
        Ok(())
    }
}
impl<B: Backend> Encoder<B> {
    /// Snapshot bytes retained by this encoder's explicitly ordered uploads.
    pub fn ordered_staging_bytes(&self) -> u64 {
        self.ordered_staging_bytes
    }
    pub fn ordered_upload_bytes(&self) -> u64 {
        self.ordered_upload_bytes
    }
}
