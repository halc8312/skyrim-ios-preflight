//! Native constant-segment encoding using the exact shared production kernel.
//! This is an executor building block, not yet the live game's submit policy.
use super::*;
use crate::compute::{ComputeBindings, ComputePipeline, StorageBinding};
use xenos_gfx::constant_segments::{ConstantBlock, MAX_RECORDS, MAX_VALUES, SealedSegment};

pub struct ConstantStream<B: Backend> {
    output: Buffer<B>,
    records: Buffer<B>,
    values: Buffer<B>,
    pipeline: ComputePipeline<B>,
    bindings: ComputeBindings<B>,
    last: Option<(u64, u64, u32)>,
    alignment: u64,
}
pub struct PreparedConstants<'a, 's, B: Backend> {
    stream: &'a ConstantStream<B>,
    segment: &'s SealedSegment<'s>,
}
impl<B: Backend + 'static> Gpu<B> {
    pub fn constant_stream(&mut self, capacity: u64) -> Result<ConstantStream<B>> {
        // All dispatch preconditions must be fixed before ordered snapshots can
        // mutate the stream. The kernel uses one workgroup for each of5stages.
        if self.owner.limits.max_compute_workgroups_per_dimension < 5 {
            return Err(Error::Rejected("constant kernel needs five workgroups"));
        }
        if capacity == 0
            || capacity > 16 << 20
            || !capacity.is_multiple_of(16)
            || capacity > self.owner.limits.max_storage_buffer_binding_size
        {
            return Err(Error::Rejected("constant stream capacity"));
        }
        let alignment = u64::from(
            self.owner
                .limits
                .min_uniform_buffer_offset_alignment
                .max(self.owner.limits.min_storage_buffer_offset_alignment),
        )
        .max(16);
        if capacity < alignment {
            return Err(Error::Rejected("constant capacity below device alignment"));
        }
        let pipeline = self.create_compute(xenos_gfx::constants::BINNED_SHADER, "main")?;
        let records = self.create_buffer(
            "constant stream records",
            ((16 + MAX_RECORDS * 13) * 4) as u64,
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )?;
        let values = self.create_buffer(
            "constant stream values",
            (MAX_VALUES * 16) as u64,
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )?;
        let output = self.create_buffer(
            "constant stream blocks",
            capacity,
            U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::UNIFORM | U::COPY_SRC,
        )?;
        let bindings = self.bind_compute(
            &pipeline,
            &[
                StorageBinding {
                    binding: 0,
                    buffer: &records,
                    offset: 0,
                    size: records.size(),
                },
                StorageBinding {
                    binding: 1,
                    buffer: &values,
                    offset: 0,
                    size: values.size(),
                },
                StorageBinding {
                    binding: 2,
                    buffer: &output,
                    offset: 0,
                    size: output.size(),
                },
            ],
        )?;
        Ok(ConstantStream {
            output,
            records,
            values,
            pipeline,
            bindings,
            last: None,
            alignment,
        })
    }
}
impl<B: Backend + 'static> ConstantStream<B> {
    pub(crate) fn output(&self) -> &Buffer<B> {
        &self.output
    }
    pub fn alignment(&self) -> u64 {
        self.alignment
    }
    /// Before this call, seal all prior consumers into this same encoder. Encode
    /// every consumer of this returned scope before starting the next segment.
    /// Old raw buffer/group clones MUST NOT be reused in a later scope; the
    /// future whole-executor boundary owns that cache reset. No implicit submit,
    /// guest fence/event completion, mapping invalidation, or CPU rewrite occurs.
    pub fn encode<'a, 's>(
        &'a mut self,
        gpu: &mut Gpu<B>,
        encoder: &mut Encoder<B>,
        segment: &'s SealedSegment<'s>,
    ) -> Result<PreparedConstants<'a, 's, B>> {
        self.encode_impl(gpu, encoder, segment, None)
    }
    /// Commit a snapshot whose actual staging allocation was secured before
    /// accepting draws. No further staging allocation occurs in this path.
    pub fn encode_reserved<'a, 's>(
        &'a mut self,
        gpu: &mut Gpu<B>,
        encoder: &mut Encoder<B>,
        segment: &'s SealedSegment<'s>,
        reservation: &mut Option<UploadReservation<B>>,
    ) -> Result<PreparedConstants<'a, 's, B>> {
        self.encode_impl(gpu, encoder, segment, Some(reservation))
    }
    fn encode_impl<'a, 's>(
        &'a mut self,
        gpu: &mut Gpu<B>,
        encoder: &mut Encoder<B>,
        segment: &'s SealedSegment<'s>,
        reservation: Option<&mut Option<UploadReservation<B>>>,
    ) -> Result<PreparedConstants<'a, 's, B>> {
        gpu.owner.healthy()?;
        if !Rc::ptr_eq(&gpu.owner, &encoder.owner) {
            return Err(Error::Rejected("constant encoder device"));
        }
        encoder.check(&self.output, U::UNIFORM)?;
        if segment.capacity() != self.output.size() {
            return Err(Error::Rejected("constant segment capacity mismatch"));
        }
        if segment.alignment() < self.alignment
            || !segment.alignment().is_multiple_of(self.alignment)
        {
            return Err(Error::Rejected("constant segment device alignment"));
        }
        let sequential = match self.last {
            Some((e, epoch, dispatch)) if epoch == segment.id() => {
                e == encoder.id && dispatch.checked_add(1) == Some(segment.dispatch())
            }
            _ => segment.dispatch() == 0,
        };
        if !sequential {
            return Err(Error::Rejected("constant epoch/dispatch sequence mismatch"));
        }
        if segment.records()[0] != 0 {
            // Sealed records remain immutable until the ordered upload snapshots
            // them synchronously. On our little-endian iPhone/Windows/Linux
            // targets their existing bytes already have the GPU wire encoding;
            // avoid allocating and copying another record vector per dispatch.
            #[cfg(target_endian = "little")]
            let bytes: &[u8] = bytemuck::cast_slice(segment.records());
            #[cfg(target_endian = "big")]
            let encoded: Vec<u8> = segment
                .records()
                .iter()
                .flat_map(|v| v.to_le_bytes())
                .collect();
            #[cfg(target_endian = "big")]
            let bytes: &[u8] = &encoded;
            let uploads = [
                OrderedUpload {
                    destination: &self.records,
                    offset: 0,
                    data: bytes,
                },
                OrderedUpload {
                    destination: &self.values,
                    offset: 0,
                    data: segment.values(),
                },
            ];
            if let Some(slot) = reservation {
                let reserved = slot
                    .as_mut()
                    .ok_or(Error::Rejected("constant snapshot was not reserved"))?;
                gpu.write_buffers_reserved(encoder, reserved, &uploads)?;
                *slot = None;
            } else {
                gpu.write_buffers_ordered(encoder, &uploads)?;
            }
            encoder.dispatch(&self.pipeline, &self.bindings, [5, 1, 1])?;
        } else if reservation.is_some_and(|slot| slot.is_some()) {
            return Err(Error::Rejected(
                "unexpected reservation for empty constants",
            ));
        }
        self.last = Some((encoder.id, segment.id(), segment.dispatch()));
        Ok(PreparedConstants {
            stream: self,
            segment,
        })
    }
}
impl<'a, B: Backend> PreparedConstants<'a, '_, B> {
    /// A checked window from THIS immutable segment. Foreign/stale tokens are
    /// rejected even when their numeric offset happens to be identical.
    pub fn window(&self, block: ConstantBlock) -> Result<(&Buffer<B>, u64, u64)> {
        let (offset, window) = self
            .segment
            .block(block)
            .map_err(|_| Error::Rejected("stale/foreign constant window"))?;
        Ok((&self.stream.output, u64::from(offset), u64::from(window)))
    }
}
