//! AOT-only bridge for a synthetic texture probe. No game files are loaded.

use std::ptr;

const WIDTH: u32 = 4;
const HEIGHT: u32 = 4;
const RGBA_BYTES: usize = WIDTH as usize * HEIGHT as usize * 4;

// Hand-authored BC1: red and blue RGB565 endpoints, then [0, 1, 2, 3] per row.
// This fixture contains no bytes extracted from a game.
const BC1_BLOCK: [u8; 8] = [0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4];

#[unsafe(no_mangle)]
pub extern "C" fn probe_abi_version() -> u32 {
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_width() -> u32 {
    WIDTH
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_height() -> u32 {
    HEIGHT
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_rgba_byte_len() -> usize {
    RGBA_BYTES
}

/// Decodes the fixed synthetic BC1 block to row-major, straight-alpha RGBA8.
///
/// Returns 64 on success, -1 for null, -2 for insufficient capacity, or -3 if
/// the decoder panics. A failure leaves the output buffer untouched.
///
/// # Safety
/// For a non-null pointer and capacity >= 64, `out` must point to at least 64
/// writable bytes for this call. No other thread may access those bytes during
/// the call. The pointer is never retained; bytes beyond the first 64 are unused.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_decode_rgba(out: *mut u8, capacity: usize) -> i32 {
    if out.is_null() {
        return -1;
    }
    if capacity < RGBA_BYTES {
        return -2;
    }

    // Decode before touching caller memory. An upstream panic cannot unwind
    // through Swift/C, and even that failure preserves the caller's buffer.
    let Ok(pixels) = std::panic::catch_unwind(|| {
        let mut pixels = [[0u8; 4]; 16];
        // Keep the probe exercising the decoder on device instead of letting
        // release/LTO replace this constant input with precomputed pixels.
        let block = std::hint::black_box(BC1_BLOCK);
        xenos_texture::bc::decode_bc1(&block, &mut pixels);
        pixels
    }) else {
        return -3;
    };

    // Arrays of [u8; 4] have no padding. The checked size is fixed, so a caller
    // reporting an excessive capacity never makes us construct a huge slice.
    unsafe {
        ptr::copy_nonoverlapping(pixels.as_ptr().cast::<u8>(), out, RGBA_BYTES);
    }
    RGBA_BYTES as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXPECTED_ROW: [u8; 16] = [
        255, 0, 0, 255, // red endpoint
        0, 0, 255, 255, // blue endpoint
        170, 0, 85, 255, // two thirds red, one third blue
        85, 0, 170, 255, // one third red, two thirds blue
    ];

    #[test]
    fn metadata_matches_the_c_buffer_contract() {
        assert_eq!(probe_abi_version(), 1);
        assert_eq!((probe_width(), probe_height()), (4, 4));
        assert_eq!(probe_rgba_byte_len(), 64);
        assert_eq!(std::mem::size_of::<[[u8; 4]; 16]>(), RGBA_BYTES);
    }

    #[test]
    fn decodes_every_expected_pixel_through_the_ffi() {
        let mut out = [0xA5u8; RGBA_BYTES];
        assert_eq!(
            unsafe { probe_decode_rgba(out.as_mut_ptr(), out.len()) },
            64
        );
        for row in out.chunks_exact(16) {
            assert_eq!(row, EXPECTED_ROW);
        }
    }

    #[test]
    fn rejects_every_short_capacity_without_writing() {
        for capacity in 0..RGBA_BYTES {
            let mut out = [0xA5u8; RGBA_BYTES];
            assert_eq!(unsafe { probe_decode_rgba(out.as_mut_ptr(), capacity) }, -2);
            assert_eq!(out, [0xA5; RGBA_BYTES]);
        }
    }

    #[test]
    fn rejects_null_without_dereferencing_it() {
        for capacity in [0, RGBA_BYTES - 1, RGBA_BYTES, usize::MAX] {
            assert_eq!(unsafe { probe_decode_rgba(ptr::null_mut(), capacity) }, -1);
        }
    }

    #[test]
    fn preserves_guard_bytes_and_unused_capacity() {
        let mut guarded = [0xA5u8; RGBA_BYTES + 16];
        let out = unsafe { guarded.as_mut_ptr().add(8) };
        assert_eq!(unsafe { probe_decode_rgba(out, RGBA_BYTES + 8) }, 64);
        assert_eq!(&guarded[..8], &[0xA5; 8]);
        assert_eq!(&guarded[8 + RGBA_BYTES..], &[0xA5; 8]);
        for row in guarded[8..8 + RGBA_BYTES].chunks_exact(16) {
            assert_eq!(row, EXPECTED_ROW);
        }
    }

    #[test]
    fn excessive_reported_capacity_still_writes_only_the_fixed_image() {
        let mut out = [0xA5u8; RGBA_BYTES];
        assert_eq!(
            unsafe { probe_decode_rgba(out.as_mut_ptr(), usize::MAX) },
            64
        );
        for row in out.chunks_exact(16) {
            assert_eq!(row, EXPECTED_ROW);
        }
    }
}
