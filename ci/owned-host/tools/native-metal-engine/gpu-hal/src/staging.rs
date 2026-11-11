//! Private upload leases. No mapped memory is exposed and no slot is reused
//! merely because a frame counter advanced or a later submission completed.
use super::*;

#[derive(Debug, Clone, Copy)]
pub struct StagingStats {
    pub created: u64,
    pub reused: u64,
    pub waits: u64,
    pub retained_bytes: u64,
    pub retained_buffers: usize,
    pub resident_bytes: u64,
    pub pending_leases: usize,
}
impl<B: Backend> Gpu<B> {
    pub(crate) fn take_upload(&mut self, size: u64) -> Result<Buffer<B>> {
        self.take_upload_limited(size, u64::MAX)
    }
    pub(crate) fn take_upload_limited(
        &mut self,
        size: u64,
        lease_ceiling: u64,
    ) -> Result<Buffer<B>> {
        if size > lease_ceiling {
            return Err(Error::Rejected("ordered staging lease budget"));
        }
        let deadline = Instant::now()
            .checked_add(self.owner.config.backpressure_timeout)
            .ok_or(Error::Rejected("staging deadline overflow"))?;
        let mut backoff = Backoff::default();
        loop {
            match self.try_take_upload(size, lease_ceiling) {
                Err(e @ Error::Rejected("staging buffer budget" | "buffer object budget"))
                    if !self.flights.is_empty()
                        && size <= self.owner.config.max_staging_buffer_bytes =>
                {
                    if matches!(e, Error::Rejected("staging buffer budget"))
                        && !self.flights.iter().any(|f| !f.staging.is_empty())
                    {
                        return Err(e);
                    }
                    self.staging_waits += 1;
                    // Only submitted flights can make progress. Never wait for
                    // the caller's unsubmitted encoder or recycle its leases.
                    self.poll()?;
                    if self.flights.is_empty() {
                        return self.try_take_upload(size, lease_ceiling);
                    }
                    if Instant::now() >= deadline {
                        return Err(e);
                    }
                    backoff.pause();
                }
                result => return result,
            }
        }
    }
    fn try_take_upload(&mut self, size: u64, lease_ceiling: u64) -> Result<Buffer<B>> {
        let pooled = self.owner.config.retained_staging_bytes > 0
            && self.owner.config.retained_staging_buffers > 0;
        let desired = if pooled {
            size.checked_next_power_of_two()
                .filter(|&n| {
                    n <= lease_ceiling
                        && n <= self.owner.limits.max_buffer_size
                        && n <= self.owner.config.max_staging_buffer_bytes
                })
                .unwrap_or(size)
        } else {
            size
        };
        // A tiny write must not occupy a very large idle lease and force the
        // next large write to allocate again. The upper bound is conservative.
        if let Some((index, _)) = self
            .staging_free
            .iter()
            .enumerate()
            .filter(|(_, b)| {
                b.size() >= size && b.size() <= desired.saturating_mul(2).min(lease_ceiling)
            })
            .min_by_key(|(_, b)| b.size())
        {
            let buffer = self.staging_free.swap_remove(index);
            self.staging_cached_bytes -= buffer.size();
            // Only Gpu owns cached leases. Referenced buffers are never cached.
            if Rc::strong_count(&buffer.0) != 1 {
                return Err(self
                    .owner
                    .fail_next("shared staging lease in idle pool".into()));
            }
            self.staging_reused += 1;
            return Ok(buffer);
        }
        // Idle objects must not cause a false global budget rejection when a
        // differently sized upload needs their space. Live/pending leases stay.
        while !self.staging_free.is_empty()
            && (self
                .owner
                .staging_bytes
                .get()
                .checked_add(size)
                .is_none_or(|n| n > self.owner.config.max_staging_buffer_bytes)
                || self.owner.live_buffers.get() >= self.owner.config.max_buffer_objects)
        {
            let buffer = self.staging_free.pop().unwrap();
            self.staging_cached_bytes -= buffer.size();
            drop(buffer);
        }
        let bucket = if self
            .owner
            .staging_bytes
            .get()
            .checked_add(desired)
            .is_some_and(|n| n <= self.owner.config.max_staging_buffer_bytes)
        {
            desired
        } else {
            size
        };
        let buffer = self.raw_buffer(
            "native upload lease",
            bucket,
            U::MAP_WRITE | U::COPY_SRC,
            true,
        )?;
        self.staging_created += 1;
        Ok(buffer)
    }
    pub(crate) fn recycle_upload(&mut self, buffer: Buffer<B>) {
        if self.owner.healthy().is_err()
            || Rc::strong_count(&buffer.0) != 1
            || self.staging_free.len() >= self.owner.config.retained_staging_buffers
            || self
                .staging_cached_bytes
                .checked_add(buffer.size())
                .is_none_or(|n| n > self.owner.config.retained_staging_bytes)
        {
            return;
        }
        self.staging_cached_bytes += buffer.size();
        self.staging_free.push(buffer);
    }
    /// Releases only idle staging. Pending and unknown/in-flight work are intact.
    pub fn trim_staging_pool(&mut self) {
        self.staging_free.clear();
        self.staging_cached_bytes = 0;
    }
    pub fn staging_stats(&self) -> StagingStats {
        StagingStats {
            created: self.staging_created,
            reused: self.staging_reused,
            waits: self.staging_waits,
            retained_bytes: self.staging_cached_bytes,
            retained_buffers: self.staging_free.len(),
            resident_bytes: self.owner.staging_bytes.get(),
            pending_leases: self.pending_staging.len(),
        }
    }
}
