//! Child of runtime, compiled only with fixture-tools on the Apple host.
//! One fixed authored fixture, after exit; no caller-chosen addresses or pointers.
use super::*;
use crate::public_fixture as fixture;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct OHPublicV2ProofV1 {
    pub version: u32,
    pub bytes: u32,
    pub abi_version: u32,
    pub layout_words: u32,
    pub host_bytes: u32,
    pub requirements_bytes: u32,
    pub provider_refusals: u32,
    pub provider_preserved: u32,
    pub frequency: u32,
    pub cleanup_count: u32,
    pub jump_result: u32,
    pub normal_result: u32,
    pub mmio_result: u32,
    pub mmio_lock_word: u32,
    pub imports_once: u32,
    pub republish_ok: u32,
}
const _: () = assert!(size_of::<OHPublicV2ProofV1>() == 64);

// Compare initialized fields, never Rust padding bytes. Pointer values stay
// internal; they are neither reported nor treated as independent provenance.
fn caller_fields(h: &abi_v2::PGHostCallbacksV2) -> Vec<u64> {
    let mut v = vec![h.abi_version.into(), h.struct_size.into(), h.contracts.into(),
        h.unwind_reserved.into(), h.user as usize as u64,
        h.memory.ea_offsets as usize as u64, h.memory.ea_offset_count,
        h.memory.written as usize as u64, h.memory.written_count,
        h.memory.guest_base as usize as u64, h.memory.guest_bytes,
        h.timebase.map_or(0, |p| p as usize as u64),
        h.kernel_call.map_or(0, |p| p as usize as u64),
        h.fatal.map_or(0, |p| p as usize as u64),
        h.xma_read_u32.map_or(0, |p| p as usize as u64),
        h.xma_write_u32.map_or(0, |p| p as usize as u64)];
    v.extend(h.context_layout);
    v.extend(h.reserved);
    v
}
fn all_fields(h: &abi_v2::PGHostCallbacksV2) -> Vec<u64> {
    let mut v = caller_fields(h);
    v.extend([h.jump_abi.into(), h.jump_buffer_size, h.jump_buffer_alignment,
        h.jump_pair_cookie, h.unwind_abi.into(), h.unwind_pair_cookie,
        h.copy_memory.map_or(0, |p| p as usize as u64),
        h.zero_memory.map_or(0, |p| p as usize as u64),
        h.exp2_float.map_or(0, |p| p as usize as u64),
        h.log2_float.map_or(0, |p| p as usize as u64),
        h.native_setjmp.map_or(0, |p| p as usize as u64),
        h.native_longjmp.map_or(0, |p| p as usize as u64),
        h.native_personality.map_or(0, |p| p as usize as u64),
        h.native_unwind_resume.map_or(0, |p| p as usize as u64),
        h.stack_guard_seed as u64]);
    v
}
fn prove(t: u64) -> Check<OHPublicV2ProofV1> {
    token(t)?;
    if STATE.state() != lifecycle::EXITED || EXIT_CODE.load(Ordering::Acquire) != 0 {
        return Err("public V2 proof requires successful exited fixture");
    }
    let guest = RESIDENT.get().ok_or("resident missing")?;
    fixture::verify_v2_fixture(&guest.binding)?;
    if guest.functions.as_ref() != fixture::FUNCTIONS || !matches!(guest.api.bind, HostBind::V2(_)) {
        return Err("public V2 exact function table required");
    }
    let k = prepared_kernel(t)?;
    let mut layout = [0xaaaa_5555_aaaa_5555; 51];
    let before = layout;
    if unsafe { darwin::OHNativeHostLayoutV2(layout.as_mut_ptr(), 50) } != -1 || layout != before {
        return Err("native V2 layout refusal changed output");
    }
    let count = unsafe { darwin::OHNativeHostLayoutV2(layout.as_mut_ptr(), 51) };
    GuestAbi::V2.verify_native_layout(count, &layout)?;

    let mut h = abi_v2::PGHostCallbacksV2 {
        abi_version: abi_v2::VERSION, struct_size: 416, contracts: abi_v2::CONTRACTS,
        context_layout: context_layout(), user: xma_callbacks::retained_kernel_user(k),
        memory: memory(k), timebase: Some(timebase), kernel_call: Some(kernel_call),
        fatal: Some(fatal), xma_read_u32: Some(xma_callbacks::xma_read_u32),
        xma_write_u32: Some(xma_callbacks::xma_write_u32), ..Default::default()
    };
    let mutations: [fn(&mut abi_v2::PGHostCallbacksV2); 8] = [
        |h| h.abi_version = VERSION, |h| h.struct_size = 400,
        |h| h.contracts = CONTRACTS, |h| h.unwind_reserved = 1,
        |h| h.xma_read_u32 = None, |h| h.xma_write_u32 = None,
        |h| h.reserved[0] = 1, |h| h.reserved[3] = 1,
    ];
    let mut refused = 0;
    for mutation in mutations {
        let mut bad = h;
        mutation(&mut bad);
        let expected = all_fields(&bad);
        if unsafe { darwin::OHNativePrepareHostV2(&mut bad, 416, guest.owner.header()) } != -1
            || all_fields(&bad) != expected {
            return Err("native V2 provider refusal changed fields");
        }
        refused += 1;
    }
    for (bytes, header) in [(415, guest.owner.header()), (416, std::ptr::null())] {
        let mut bad = h;
        let expected = all_fields(&bad);
        if unsafe { darwin::OHNativePrepareHostV2(&mut bad, bytes, header) } != -1
            || all_fields(&bad) != expected {
            return Err("native V2 provider size/header refusal changed fields");
        }
        refused += 1;
    }
    let supplied = caller_fields(&h);
    if unsafe { darwin::OHNativePrepareHostV2(&mut h, 416, guest.owner.header()) } != 0
        || caller_fields(&h) != supplied || h.jump_abi != APPLE_JUMP
        || h.unwind_abi != APPLE_UNWIND || h.jump_pair_cookie == 0 || h.unwind_pair_cookie == 0
        || h.stack_guard_seed == 0 || h.jump_buffer_size != guest.requirements.jump_buffer_size
        || h.jump_buffer_alignment != guest.requirements.jump_buffer_alignment
        || h.copy_memory.is_none() || h.zero_memory.is_none() || h.exp2_float.is_none()
        || h.log2_float.is_none() || h.native_setjmp.is_none() || h.native_longjmp.is_none()
        || h.native_personality.is_none() || h.native_unwind_resume.is_none() {
        return Err("native V2 provider success/preservation mismatch");
    }
    // The real provider enforces foreign image identity; this independent call
    // preserves the actual retained user/memory/callback fields without rebinding.
    let frequency = k.mem().read_u32(fixture::FREQUENCY);
    let cleanup_count = k.mem().read_u32(fixture::CLEANUP);
    let jump_result = k.mem().read_u32(fixture::JUMP_RESULT);
    let mmio_result = k.mem().read_u32(fixture::MMIO_RESULT);
    let mmio_lock_word = k.xma_mmio_read(kernel_hle::xma::CONTEXT_LOCK);
    let stats = k.import_stats();
    if frequency != 50_000_000 || cleanup_count != 1 || jump_result != 0x1111 || mmio_result != 1
        || mmio_lock_word != Some(0x8000_0001) || stats.len() != 2
        || ![0x83, 0x19].into_iter().all(|ordinal| stats.iter().any(|(m, o, _, s)|
            m.0 == 0 && *o == ordinal && s.calls == 1 && s.implemented)) {
        return Err("public V2 jump/unwind/MMIO/import outcomes differ");
    }
    // Only this authored fixture has exited. Successful republish proves the
    // adapter's active-call RAII guard unwound, independently of our guest guard.
    if unsafe { (guest.api.publish)(&memory(k), 48) } != OK {
        return Err("public V2 adapter active call did not unwind");
    }
    let mut context = PPCContext::new_zeroed();
    unsafe { init_context(&mut *context) };
    k.rt.call(&mut context, fixture::FUNCTIONS[2]).map_err(|_| "public normal return refused")?;
    if context.gpr(3) != 0x2222 { return Err("public normal return differs"); }
    Ok(OHPublicV2ProofV1 {
        version: 0x10000, bytes: 64, abi_version: abi_v2::VERSION, layout_words: 51,
        host_bytes: 416, requirements_bytes: 216, provider_refusals: refused, provider_preserved: 1,
        frequency, cleanup_count, jump_result, normal_result: 0x2222, mmio_result,
        mmio_lock_word: mmio_lock_word.unwrap(), imports_once: 2, republish_ok: 1,
    })
}

/// # Safety
/// `out` is exclusive aligned writable storage for exactly 64 bytes. Output is
/// untouched on refusal. The SDK test supplies its independent process ceiling.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHReadPublicV2ProofV1(t: u64, out: *mut OHPublicV2ProofV1, bytes: usize) -> i32 {
    if out.is_null() || bytes != 64 || !(out as usize).is_multiple_of(align_of::<OHPublicV2ProofV1>()) {
        return -1;
    }
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| prove(t))) {
        Ok(Ok(value)) => { unsafe { out.write(value); } 0 }
        Ok(Err(_)) => -2,
        Err(payload) => { std::mem::forget(payload); -3 }
    }
}
