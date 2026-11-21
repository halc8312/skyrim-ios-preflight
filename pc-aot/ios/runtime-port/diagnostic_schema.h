/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Private diagnostic wire schema v1. Positive allowlist shared by the manual
 * iOS sender and the PC receiver. Each regex must match the complete ASCII
 * line without LF/CR. Unknown records are excluded, never sent as raw text.
 * No paths, filenames, opaque strings, guest values, shader bytes or secrets.
 * IOS_LIFECYCLE event: inactive request, active request, drain begin/end,
 * background, deadline missed, failure, audio pause/resume (0..8).
 * Lifecycle phase: active, closing, draining, paused, failed (0..4).
 * Capture=closed means not the current log, not proof of graceful shutdown.
 */
#define SKYRIM_DIAGNOSTIC_SCHEMA_VERSION 1
#define SKYRIM_DIAGNOSTIC_MAX_LINE 8191
#define SKYRIM_DIAGNOSTIC_MAX_SESSION_LINES 60000
#define SKYRIM_DIAGNOSTIC_MAX_SESSION_BYTES (6u * 1024u * 1024u)
#define SKYRIM_DIAGNOSTIC_MAX_BODY_BYTES (16u * 1024u * 1024u)
typedef enum SkyrimDiagnosticSettingKey {
    SKYRIM_DIAG_SETTING_FPS = 0,
    SKYRIM_DIAG_SETTING_THERMAL = 1,
    SKYRIM_DIAG_SETTING_LANGUAGE = 2,
    SKYRIM_DIAG_SETTING_VERBOSE = 3,
    SKYRIM_DIAG_SETTING_HUD = 4,
    SKYRIM_DIAG_SETTING_RESOLUTION = 5,
    SKYRIM_DIAG_SETTING_TEXTURE = 6,
    SKYRIM_DIAG_SETTING_ANISOTROPY = 7,
    SKYRIM_DIAG_SETTING_FXAA = 8,
    SKYRIM_DIAG_SETTING_MSAA = 9,
    SKYRIM_DIAG_SETTING_SHADOW = 10,
    SKYRIM_DIAG_SETTING_SHADOW_DISTANCE = 11,
    SKYRIM_DIAG_SETTING_GRASS = 12,
    SKYRIM_DIAG_SETTING_OBJECTS = 13,
    SKYRIM_DIAG_SETTING_WATER = 14,
    SKYRIM_DIAG_SETTING_DECALS = 15,
    SKYRIM_DIAG_SETTING_AUTOSAVE = 16,
    SKYRIM_DIAG_SETTING_GAME_TEXT = 17,
    SKYRIM_DIAG_SETTING_GAME_VOICES = 18,
} SkyrimDiagnosticSettingKey;

/* E(message, code): exact FATAL iOS startup suffix only, never a substring.
 * Dynamic error descriptions (which can contain paths) are intentionally absent.
 * The sender emits IOS_DIAG_ERROR code=N instead of the original message. */
#define SKYRIM_DIAG_STATIC_ERRORS(E) \
    E("Cannot safely apply the game language to the app profile; the guest was not started", 1) \
    E("Private guest framework binding failed; see preceding diagnostics", 2) \
    E("Bundled SkyrimGuest.framework is missing or unreadable", 3) \
    E("Cannot resolve app container directory", 4) \
    E("Cannot redirect runtime log", 5) \
    E("PcGuestExpectedInputSHA256 is missing from the locally assembled app", 6) \
    E("PcGuestExpectedInputSHA256 must contain exactly 64 hexadecimal characters", 7) \
    E("PcGuestExpectedInputSHA256 contains a non-hexadecimal character", 8) \
    E("Cannot initialize guest-input SHA256 verification", 9) \
    E("Guest-input SHA256 verification failed while reading", 10) \
    E("Cannot close the verified guest input", 11) \
    E("Cannot finish guest-input SHA256 verification", 12) \
    E("TESV_unpacked.exe SHA256 does not match this locally assembled app; guest was not loaded", 13) \
    E("SDL UIKit entry did not run on the main thread", 14) \
    E("Cannot initialize shader preparation option", 15)

/* X(prefix, regex): expand into the caller's own string table. */
#define SKYRIM_DIAG_RULES(X) \
    X("IOS_SHADER_LEAD", "^IOS_SHADER_LEAD schema=1 seq=[1-9][0-9]{0,19} result=[12] ready_ns=[0-9]{1,20} queued_ns=[0-9]{1,20} worker_ns=[0-9]{1,20} compile_ns=[0-9]{1,20} published_ns=[0-9]{1,20} wait_start_ns=[0-9]{1,20} wait_end_ns=[0-9]{1,20}$") \
    X("IOS_SHADER_LEAD_DROPS", "^IOS_SHADER_LEAD_DROPS schema=1 seq=[1-9][0-9]{0,19} busy=[0-9]{1,20} full=[0-9]{1,20}$") \
    X("IOS_LOCK", "^IOS_LOCK schema=1 threshold_ns=16666667 count=[0-9]{1,20} dropped_busy=[0-9]{1,20} dropped_full=[0-9]{1,20} window_start_ns=[0-9]{1,20} window_end_ns=[0-9]{1,20} totals=[0-9]{1,20}(?:,[0-9]{1,20}){5} fields=lock,phase,operation,instance,thread,owner_thread,owner_operation,start_ns,end_ns,cpu_ns,cpu_valid spans=[0-9,;]*$") \
    X("IOS_TIMING", "^IOS_TIMING schema=1 approximate_window=1 end_ns=[0-9]{1,20} window_ns=[0-9]{1,20} kind=(?:frame_gap|present_host|pacer|hlsl|graphics_pipeline|compute_pipeline|shader_module|graphics_compile_wait|compute_compile_wait|read_file|read_file_ex|d3d_shader_create|texture_load|xwma_decode|resource_wait|d3d_shader_wait|image_acquire|queue_present|queue_submit|fence_wait|submission_wait|mvk_msl_wait|mvk_specialize_wait|mvk_pipeline_wait|mvk_other_wait|mvk_dispatch_wait|mvk_dispatch_block|write_file|write_file_ex|save_candidate_write|save_candidate_write_ex|save_candidate_window|mvk_cold_lock_wait|mvk_convert|pacer_runloop|pacer_sleep|ui_pump|pacer_overrun|graphics_state_wait|save_first_to_last_write_start|save_last_write_start_to_close|save_close_call) samples=[0-9]{1,20} total_ns=[0-9]{1,20} max_ns=[0-9]{1,20} failures_known=[01] failures=[0-9]{1,20} sleep_calls=[0-9]{1,20} bins=[0-9]{1,20}(?:,[0-9]{1,20}){19}$") \
    X("IOS_TIMING_CONTEXT", "^IOS_TIMING_CONTEXT schema=1 approximate_window=1 end_ns=[0-9]{1,20} window_ns=[0-9]{1,20} kind=(?:frame_gap|present_host|pacer|hlsl|graphics_pipeline|compute_pipeline|shader_module|graphics_compile_wait|compute_compile_wait|read_file|read_file_ex|d3d_shader_create|texture_load|xwma_decode|resource_wait|d3d_shader_wait|image_acquire|queue_present|queue_submit|fence_wait|submission_wait|mvk_msl_wait|mvk_specialize_wait|mvk_pipeline_wait|mvk_other_wait|mvk_dispatch_wait|mvk_dispatch_block|write_file|write_file_ex|save_candidate_write|save_candidate_write_ex|save_candidate_window|mvk_cold_lock_wait|mvk_convert|pacer_runloop|pacer_sleep|ui_pump|pacer_overrun|graphics_state_wait) samples=[0-9]{1,20} total_ns=[0-9]{1,20} max_ns=[0-9]{1,20} failures_known=[01] failures=[0-9]{1,20} sleep_calls=[0-9]{1,20} bins=[0-9]{1,20}(?:,[0-9]{1,20}){19} role=[0-2] source=[0-3] result=[0-2]$") \
    X("IOS_STALL", "^IOS_STALL schema=1 threshold_ns=50000000 count=[0-9]{1,20} dropped_busy=[0-9]{1,20} dropped_full=[0-9]{1,20} fields=kind,thread,start_ns,end_ns,failed,estimated_end spans=[0-9,;]*$") \
    X("IOS_STALL", "^IOS_STALL schema=2 threshold_ns=50000000 count=[0-9]{1,20} dropped_busy=[0-9]{1,20} dropped_full=[0-9]{1,20} fields=kind,thread,start_ns,end_ns,failed,estimated_end,role,source,result spans=[0-9,;]*$") \
    X("IOS_STALL", "^IOS_STALL schema=3 threshold_ns=50000000 count=[0-9]{1,20} dropped_busy=[0-9]{1,20} dropped_full=[0-9]{1,20} window_start_ns=[0-9]{1,20} window_end_ns=[0-9]{1,20} fields=kind,thread,start_ns,end_ns,failed,estimated_end,role,source,result spans=[0-9,;]*$") \
    X("IOS_PERF", "^IOS_PERF frames=[0-9]{1,20} fps=-?[0-9]{1,10}(?:[.][0-9]{1,6})? cap=[0-9]{1,20} thermal=[0-9]{1,20} present_host_avg_ms=-?[0-9]{1,10}(?:[.][0-9]{1,6})? present_host_max_ms=-?[0-9]{1,10}(?:[.][0-9]{1,6})? frame_gap_max_ms=-?[0-9]{1,10}(?:[.][0-9]{1,6})?$") \
    X("IOS_DEVICE", "^IOS_DEVICE schema=1 cpus=[0-9]{1,20} active=[0-9]{1,20} screen_max_hz=[0-9]{1,20} physical_bytes=[0-9]{1,20} gpu=[0-9]{1,20} apple_family=[0-9]{1,20} unified=[01] bc=[01] max_buffer_bytes=[0-9]{1,20} guest_cpu_policy_unchanged=1$") \
    X("IOS_DEVICE_STATE", "^IOS_DEVICE_STATE active=[0-9]{1,20} thermal=[0-3] low_power=[01] foreground=[01] headroom_known=[01] headroom_bytes=[0-9]{1,20} memory_warning=[01] reclaimed_pool_bytes=[0-9]{1,20} smaller_pools=[0-9]{1,20}$") \
    X("IOS_WORK_QOS", "^IOS_WORK_QOS role=[0-9]{1,20} target=[0-9]{1,20} error=-?[0-9]{1,20}$") \
    X("IOS_DRAWABLE", "^IOS_DRAWABLE pixels=[0-9]{1,20}x[0-9]{1,20} main_thread=1$") \
    X("IOS_GRAPHICS", "^IOS_GRAPHICS override_count=[0-9]{1,20} resolution=[0-9]{1,20}x[0-9]{1,20}$") \
    X("IOS_SHADER_OPTION", "^IOS_SHADER_OPTION option=[0-5] enabled=[01]$") \
    X("IOS_SHADER_POLICY", "^IOS_SHADER_POLICY schema=1 module_cold=[01] cold_scope=[01]$") \
    X("IOS_GRAPHICS_POLICY", "^IOS_GRAPHICS_POLICY schema=1 (?:exact_state=0 wait_scope=0|exact_state=1 wait_scope=1)$") \
    X("IOS_STATE_CACHE", "^IOS_STATE_CACHE entries=[0-9]{1,20} shader_tuples=[0-9]{1,20} wakeup_links=[0-9]{1,20}$") \
    X("IOS_GAME_LANGUAGE", "^IOS_GAME_LANGUAGE requested_text=[01] text=[01] text_reason=[0-3] requested_voices=[01] voices=[01] voices_reason=[0-3]$") \
    X("IOS_OVERLAY_LAYOUT", "^IOS_OVERLAY_LAYOUT schema=1 view_pt=[0-9]{1,5}x[0-9]{1,5} safe=[0-9]{1,5},[0-9]{1,5},[0-9]{1,5},[0-9]{1,5} top_controls_max_y=[0-9]{1,5} scale=[0-9]{1,4}$") \
    X("IOS_CLASSIC_SKYUI", "^IOS_CLASSIC_SKYUI (?:requested=0 enabled=0|requested=1 enabled=[01])$") \
    X("IOS_MOD_FEATURE", "^IOS_MOD_FEATURE id=[0-9]{1,2} requested=1 available=[01] enabled=[01] reason=[0-5]$") \
    X("IOS_PAPYRUSUTIL", "^IOS_PAPYRUSUTIL schema=1 published=[0-9]{1,2} calls=[0-9]{1,20} refused_stale=[0-9]{1,20} commits=[0-9]{1,20} discards=[0-9]{1,20} errors=[0-9]{1,20}$") \
    X("IOS_PAPYRUSUTIL_OPEN", "^IOS_PAPYRUSUTIL_OPEN (acquisition=-?[0-9]{1,5}|status=[0-9]{1,2})$") \
    X("IOS_FUZ", "^IOS_FUZ schema=1 enabled=[01] substituted=[0-9]{1,20} kept=[0-9]{1,20} missing=[0-9]{1,20} errors=[0-9]{1,20}$") \
    X("IOS_CAMERA_STATE", "^IOS_CAMERA_STATE slot=(?:[0-9]|1[0-2])$") \
    X("IOS_CAMERA_RENDER", "^IOS_CAMERA_RENDER schema=1 first=(?:[0-9]|1[0-5]) second=(?:[0-9]|1[0-5])$") \
    X("IOS_CAMERA_CULL", "^IOS_CAMERA_CULL schema=1 stages=[1357] producer=(?:[0-9]|[1-9][0-9]|1[0-2][0-9])(?:,(?:[0-9]|[1-9][0-9]|1[01][0-9]|12[0-8])){2} first=(?:[0-9]|[1-9][0-9]|1[0-2][0-9])(?:,(?:[0-9]|[1-9][0-9]|1[01][0-9]|12[0-8])){2} second=(?:[0-9]|[1-9][0-9]|1[0-2][0-9])(?:,(?:[0-9]|[1-9][0-9]|1[01][0-9]|12[0-8])){2}$") \
    X("IOS_ECE_TRI", "^IOS_ECE_TRI schema=1 (?:source=0 parsed=0|source=[0-2] parsed=1)$") \
    X("IOS_ECE_NOSE", "^IOS_ECE_NOSE schema=1 female_nord=[01] head=[01] geometry=[01] path=[01] fresh=[01] record=[01]$") \
    X("IOS_CAMERA_BODY", "^IOS_CAMERA_BODY schema=1 slot=[089] arms=[0-5] body=[0-5]$") \
    X("IOS_LOADING_MENU", "^IOS_LOADING_MENU opening=[01] ts_ns=[0-9]{1,20}$") \
    X("IOS_CHARGEN_MENU", "^IOS_CHARGEN_MENU opening=[01] ts_ns=[0-9]{1,20}$") \
    X("IOS_INVENTORY_FIT", "^IOS_INVENTORY_FIT armed=[01]$") \
    X("IOS_SAFETY_LOAD", "^IOS_SAFETY_LOAD accepted=[0-9]{1,20}$") \
    X("IOS_CLASSIC_FRESH_SAVE", "^IOS_CLASSIC_FRESH_SAVE published=1$") \
    X("IOS_CLASSIC_SAVE_RECORDS", "^IOS_CLASSIC_SAVE_RECORDS captured=[01] mods=(?:[1-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]) records=(?:[0-9]|1[0-6])$") \
    X("IOS_CLASSIC_SAVE_STORE", "^IOS_CLASSIC_SAVE_STORE phase=[12] result=(?:[0-9]|1[01]) detail=(?:[0-9]|1[0-7])$") \
    X("IOS_CLASSIC_LOAD_STORE", "^IOS_CLASSIC_LOAD_STORE result=(?:[0-9]|1[01]) detail=(?:[0-9]|1[0-7])$") \
    X("IOS_CLASSIC_LOAD_RECORDS", "^IOS_CLASSIC_LOAD_RECORDS matched=[01] applied=[01]$") \
    X("IOS_CLASSIC_SKYUI_INPUTS", "^IOS_CLASSIC_SKYUI_INPUTS skyui=[01] skse_pex=[01] support=[0-4]$") \
    X("IOS_CLASSIC_VM", "^IOS_CLASSIC_VM registered=[0-9]{1,10} primitive_passed=[0-9]{1,10}$") \
    X("IOS_CLASSIC_RELEASE", "^IOS_CLASSIC_RELEASE value=[0-9]{1,10}$") \
    X("IOS_CLASSIC_HOOK", "^IOS_CLASSIC_HOOK point=[12] order=[12] startup_thread=[01]$") \
    X("IOS_PROGRESS", "^IOS_PROGRESS present_phase=[0-9]{1,20} present_main=[01] phase_age_ms=[0-9]{1,20} ui_pump_age_ms=[0-9]{1,20}$") \
    X("IOS_PRESENT", "^IOS_PRESENT failure=0x[0-9a-fA-F]{8}$") \
    X("IOS_STAGE", "^IOS_STAGE (?:sdl-application-entry|guest-input-sha256-verified|private-guest-bound|first-successful-present-title-not-verified|runtime-start|guest-memory-bound|guest-entry|vulkan-window-ready) monotonic=[0-9]{1,20}[.][0-9]{9}$") \
    X("IOS_PREFETCH_GATE", "^IOS_PREFETCH_GATE event=[0-9]{1,20} enabled=1 yielded=[0-9]{1,20} sleeps=[0-9]{1,20} waited_ns=[0-9]{1,20} expired=[0-9]{1,20} thermal=[0-9]{1,20} clock_failures=[0-9]{1,20} required=[0-9]{1,20} disabled=[01] stopping=[01]$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=storage persistent_requested=1 selected=[01] backup_excluded=[01] error=-?[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=worker ticket=[0-9]{1,20} kind=[0-9]{1,20} phase=[0-9]{1,20} queue_wait_ns=[0-9]{1,20} elapsed_ns=[0-9]{1,20} monotonic_ns=[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=finish ticket=[0-9]{1,20} source=[0-9]{1,20} current=[0-9]{1,20} task_valid=[01]$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=prepare ticket=[0-9]{1,20} task_valid=[01] reused=[01]$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=queue ticket=[0-9]{1,20} reason=[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=identity reuse_requested=[01] reuse_reviewed=[01]$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=checkpoint_boundary ticket=[0-9]{1,20} boundary=[0-9]{1,20} elapsed_ns=[0-9]{1,20} monotonic_ns=[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=checkpoint_stage ticket=[0-9]{1,20} stage=[0-9]{1,20} attempt=[0-9]{1,20} elapsed_ns=[0-9]{1,20} monotonic_ns=[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=checkpoint ticket=[0-9]{1,20} outcome=[0-9]{1,20} stage=[0-9]{1,20} attempts=[0-9]{1,20} vk=-?[0-9]{1,20} size_query=[0-9]{1,20} bytes=[0-9]{1,20} gen=[0-9]{1,20} saved_gen=[0-9]{1,20} errno=-?[0-9]{1,20} budget_expired=[01] stored=[01] elapsed_ns=[0-9]{1,20} import_publish=[0-9]{1,20} cleanup_errno=-?[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=create offered_bytes=[0-9]{1,20} empty_retry=[01] result=-?[0-9]{1,20} hit_count_unknown=1 imported=[01]$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=foreground_admit ticket=[0-9]{1,20} quiet_ns=[0-9]{1,20} cooldown_ns=[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=checkpoint_all ticket=[0-9]{1,20} cancelled=[01] caches=[0-9]{1,20}$") \
    X("IOS_PIPE_CACHE", "^IOS_PIPE_CACHE event=checkpoint_all ticket=[0-9]{1,20} busy=1 caches=0$") \
    X("IOS_DIAG_SETTING", "^IOS_DIAG_SETTING key=(?:[0-9]|1[0-8]) value=-?[0-9]{1,10}(?:[.][0-9]{1,6})?$") \
    X("IOS_DIAG_ERROR", "^IOS_DIAG_ERROR code=(?:[1-9]|1[0-5])$") \
    X("IOS_LIFECYCLE", "^IOS_LIFECYCLE schema=1 event=[0-8] generation=[0-9]{1,20} phase=[0-4] active_calls=[0-9]{1,20} requested=[01] monotonic_ns=[0-9]{1,20} elapsed_ns=[0-9]{1,20} result=-?[0-9]{1,20}$")
