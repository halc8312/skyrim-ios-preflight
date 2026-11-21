/* SPDX-License-Identifier: GPL-3.0-only */
#include "rt.h"

int g_log_level;
char g_game_dir[1024], g_save_dir[1024];

/* The sanitized runtime retains its import table. Calls outside the hosted
 * file fixture must stop the test if any are unexpectedly reached. */
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
uint32_t gmalloc(uint32_t n) { (void)n; abort(); }
void gfree(uint32_t p) { (void)p; abort(); }
uint32_t grealloc(uint32_t p, uint32_t n) { (void)p; (void)n; abort(); }
uint32_t gsize(uint32_t p) { (void)p; abort(); }
uint32_t gstrdup(const char *p) { (void)p; abort(); }
uint32_t gvalloc(uint32_t address, uint32_t size) { (void)address; (void)size; abort(); }
void gvfree(uint32_t address) { (void)address; abort(); }
int rt_host_cpu_count(void) { abort(); }
int rt_host_match(const char *pattern, const char *name) { (void)pattern; (void)name; abort(); }
uint32_t rt_import_opt(const char *library, const char *name) { (void)library; (void)name; abort(); }
void rt_altstack(void) { abort(); }
void skyrim_ios_pump_events(void) { abort(); }
uint32_t rt_invoke(CPU *cpu, uint32_t fn, int nargs, const uint32_t *args) {
    (void)cpu;
    (void)fn;
    (void)nargs;
    (void)args;
    abort();
}
