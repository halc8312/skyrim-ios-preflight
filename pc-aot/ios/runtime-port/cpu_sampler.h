/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in CPU sampling for performance analysis; off unless SKYRIM_IOS_CPU_SAMPLE=1
 * (the startup "CPU sampling" switch sets it). Starts one dedicated thread that,
 * about every 2 ms by default (SKYRIM_IOS_CPU_SAMPLE_HZ=50..1000), briefly stops
 * each *running* thread of this process, reads its program counter and frame
 * chain, and resumes it. It writes numeric IOS_CPU_IMAGE / IOS_CPU_SAMPLE lines:
 * an anonymous thread ordinal and offsets inside this app's own images, never
 * names or paths. Prints one IOS_CPU_SAMPLER line stating whether it started.
 * Call once on the main thread. Returns 1 when sampling started. */
int skyrim_cpu_sampler_start(void);

#ifdef __cplusplus
}
#endif
