/* SPDX-License-Identifier: GPL-3.0-only */
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu_sampler.h"
#include "cpu_sample_core.h"
#include "performance_metrics.h"

/* Compile under ARC. Only the sampler thread touches the aggregation state.
 *
 * Stopping a thread is only safe if the sampler does nothing while it is stopped
 * that could need a lock the stopped thread holds (the allocator, stdio, dyld, the
 * pthread list). Between thread_suspend and thread_resume the sampler therefore
 * reads registers and the stopped thread's own stack, and nothing else. Stack
 * bounds, thread identity, image tables and all logging are prepared or done
 * while no thread is stopped. One thread is stopped at a time, for microseconds. */
enum { KNOWN_THREADS = 128, REPORT_SECONDS = 5, DEFAULT_HZ = 500 };

typedef struct KnownThread {
    uint64_t id;                    /* kernel thread id */
    uint64_t stack_lo, stack_hi;    /* 0, 0 when unknown: the walk then stops at the pc */
    unsigned ordinal;               /* sampler-local, in first-seen order */
    int main_thread;
} KnownThread;

static KnownThread known[KNOWN_THREADS];
static unsigned known_count;
static uint64_t main_thread_id;
static unsigned hz;
static unsigned images_printed;
static uint64_t now_ns(void) { return skyrim_perf_now_ns(); }

static void refresh_images(void) {
    CpuSampleImage list[CPU_SAMPLE_MAX_IMAGES];
    unsigned count = 0;
    const uint32_t total = _dyld_image_count();
    for (uint32_t i = 0; i < total && count < CPU_SAMPLE_MAX_IMAGES; ++i) {
        const char *name = _dyld_get_image_name(i);
        /* System libraries come from the shared cache and cannot be named from
         * this app's symbol tables; their time stays under the app caller. The
         * path is only used for this filter and is never logged. */
        if (!name || !strncmp(name, "/System/", 8) || !strncmp(name, "/usr/", 5) ||
            !strncmp(name, "/Developer/", 11)) continue;
        const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!header || header->magic != MH_MAGIC_64) continue;
        CpuSampleImage image;
        memset(&image, 0, sizeof image);
        image.base = (uint64_t)(uintptr_t)header;
        int have_uuid = 0;
        const struct load_command *command = (const struct load_command *)(header + 1);
        for (uint32_t c = 0; c < header->ncmds; ++c) {
            if (command->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
                if (!strncmp(segment->segname, "__TEXT", 16)) image.text_size = segment->vmsize;
            } else if (command->cmd == LC_UUID) {
                memcpy(image.uuid, ((const struct uuid_command *)command)->uuid, 16);
                have_uuid = 1;
            }
            command = (const struct load_command *)((const char *)command + command->cmdsize);
        }
        if (have_uuid && image.text_size) list[count++] = image;
    }
    cpu_sample_set_images(list, count);
    char line[160];
    for (; images_printed < cpu_sample_image_count(); ++images_printed)
        if (cpu_sample_format_image(line, sizeof line, images_printed)) fputs(line, stderr);
}

static KnownThread *lookup(thread_act_t port) {
    thread_identifier_info_data_t identity;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(port, THREAD_IDENTIFIER_INFO, (thread_info_t)&identity, &count) != KERN_SUCCESS) return NULL;
    for (unsigned i = 0; i < known_count; ++i)
        if (known[i].id == identity.thread_id) return &known[i];
    if (known_count == KNOWN_THREADS) return NULL;
    KnownThread *entry = &known[known_count];
    memset(entry, 0, sizeof *entry);
    entry->id = identity.thread_id;
    entry->ordinal = ++known_count;
    entry->main_thread = identity.thread_id == main_thread_id;
    pthread_t thread = pthread_from_mach_thread_np(port);   /* no thread is stopped here */
    if (thread) {
        const uint64_t top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(thread);
        const uint64_t size = pthread_get_stacksize_np(thread);
        if (top && size && size <= top) { entry->stack_hi = top; entry->stack_lo = top - size; }
    }
    return entry;
}

static void sample_thread(thread_act_t port) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &info_count) != KERN_SUCCESS) {
        cpu_sample_note_suspend_error();
        return;
    }
    /* Waiting and idle threads use no CPU; sampling them would only add noise. */
    if ((info.flags & TH_FLAGS_IDLE) || info.run_state != TH_STATE_RUNNING) return;
    KnownThread *entry = lookup(port);
    if (!entry) return;
    uint64_t frames[CPU_SAMPLE_MAX_DEPTH];
    unsigned depth = 0;
    arm_thread_state64_t state;
    mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
    if (thread_suspend(port) != KERN_SUCCESS) {
        cpu_sample_note_suspend_error();
        return;
    }
    kern_return_t result = thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &state_count);
    if (result == KERN_SUCCESS)
        depth = cpu_sample_unwind(arm_thread_state64_get_pc(state), arm_thread_state64_get_lr(state),
                                  arm_thread_state64_get_fp(state), entry->stack_lo, entry->stack_hi, frames);
    thread_resume(port);
    if (result != KERN_SUCCESS || !depth) {
        cpu_sample_note_suspend_error();
        return;
    }
    cpu_sample_add(entry->ordinal, entry->main_thread, frames, depth);
}

static void *sampler_main(void *argument) {
    (void)argument;
    pthread_setname_np("SkyrimIOS.cpu-sampler");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    const mach_port_t self_port = mach_thread_self();
    const uint64_t period_ns = UINT64_C(1000000000) / hz;
    uint64_t next = now_ns(), report_start = next, images_checked = 0;
    for (;;) {
        const uint64_t started = now_ns();
        if (started - images_checked >= UINT64_C(1000000000)) { refresh_images(); images_checked = started; }
        thread_act_array_t threads = NULL;
        mach_msg_type_number_t thread_count = 0;
        if (task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
                if (threads[i] != self_port) sample_thread(threads[i]);
                mach_port_deallocate(mach_task_self(), threads[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)threads, thread_count * sizeof(*threads));
        } else {
            cpu_sample_note_suspend_error();
        }
        cpu_sample_note_tick();
        uint64_t now = now_ns();
        if (now - report_start >= (uint64_t)REPORT_SECONDS * UINT64_C(1000000000)) {
            char line[8192];
            if (cpu_sample_format_window(line, sizeof line, now, now - report_start, hz)) fputs(line, stderr);
            report_start = now;
        }
        next += period_ns;
        if (now > next + period_ns) { cpu_sample_note_overrun(); next = now; }   /* fell behind: do not burst */
        if (next > now) {
            const uint64_t wait = next - now;
            struct timespec pause = {(time_t)(wait / UINT64_C(1000000000)), (long)(wait % UINT64_C(1000000000))};
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

int skyrim_cpu_sampler_start(void) {
    static int started;
    const char *setting = getenv("SKYRIM_IOS_CPU_SAMPLE");
    if (!setting || strcmp(setting, "1") || started) {
        fprintf(stderr, "IOS_CPU_SAMPLER schema=1 enabled=0 hz=0\n");
        return 0;
    }
    hz = DEFAULT_HZ;
    const char *rate = getenv("SKYRIM_IOS_CPU_SAMPLE_HZ");
    if (rate) {
        char *end = NULL;
        long value = strtol(rate, &end, 10);
        if (end && !*end && value >= 50 && value <= 1000) hz = (unsigned)value;
    }
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);     /* the caller is the main thread */
    main_thread_id = id;
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    const int error = pthread_create(&thread, &attributes, sampler_main, NULL);
    pthread_attr_destroy(&attributes);
    if (error) {
        fprintf(stderr, "IOS_CPU_SAMPLER schema=1 enabled=0 hz=0\n");
        return 0;
    }
    started = 1;
    fprintf(stderr, "IOS_CPU_SAMPLER schema=1 enabled=1 hz=%u\n", hz);
    return 1;
}
