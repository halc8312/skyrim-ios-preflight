#include <string.h>
#include "funcs_serial.h"
#include "rt_table.h"
#include "serial_layout.h"
#include "serialization_session.h"

enum { SKSE = 0x3000, INFO = 0x3040, SERIAL = 0x3100, STACK = 0x8000 };
enum { QIF = 0x1100, HANDLE, UID, REG_REVERT, REG_SAVE, REG_LOAD, WRITE, NEXT, READ };
extern const FnEntry g_fn_table_serial[];
extern const unsigned g_fn_count_serial;
static unsigned char memory[SERIAL_IMAGE_END];
uint8_t *MEM = memory;
static GuestFn events[EVENT_COUNT];

static void valid_word(uint32_t address) {
    if (!address || address > sizeof memory - 4) fixture_fail("guest word outside fixture memory");
}
static unsigned word(uint32_t address) { valid_word(address); return M32(address); }
static void put(uint32_t address, unsigned value) { valid_word(address); M32(address) = value; }

static GuestFn translated(uint32_t address) {
    for (unsigned i = 0; i < g_fn_count_serial; ++i)
        if (g_fn_table_serial[i].addr == address) return g_fn_table_serial[i].fn;
    fixture_fail("callback outside translated fixture functions");
    return NULL;
}

void rt_call(CPU *c, uint32_t target) {
    uint32_t sp = c->r[4];
    if (sp > STACK || sp < STACK - 1024) fixture_fail("unexpected guest stack");
    unsigned result = 0;
    switch (target) {
        case QIF: host_query_interface(word(sp + 4)); result = SERIAL; break;
        case HANDLE: result = host_handle(); break;
        case UID: host_unique_id(word(sp + 4), word(sp + 8)); break;
        case REG_REVERT: case REG_SAVE: case REG_LOAD: {
            enum Event event = (enum Event)(target - REG_REVERT);
            host_event(word(sp + 4), event);
            events[event] = translated(word(sp + 8));
            break;
        }
        case WRITE:
            result = host_write(word(sp + 4), word(sp + 8), word(sp + 16), word(word(sp + 12)));
            break;
        case NEXT: {
            unsigned type, version, length;
            result = host_next(&type, &version, &length);
            if (result) { put(word(sp + 4), type); put(word(sp + 8), version); put(word(sp + 12), length); }
            break;
        }
        case READ: {
            unsigned value;
            result = host_read(&value, word(sp + 8));
            put(word(sp + 4), value);
            break;
        }
        default: fixture_fail("unknown serialization fixture callback");
    }
    c->r[0] = result;
    c->r[4] += 4;
}
void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) { (void)cache; rt_call(c, target); }

static int invoke(GuestFn entry, uint32_t first, uint32_t second) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    put(STACK, 0xfeeda11u);
    put(STACK + 4, first);
    put(STACK + 8, second);
    entry(&cpu);
    if (cpu.r[4] != STACK + 4 || word(STACK) != 0xfeeda11u) fixture_fail("callback cdecl stack mismatch");
    return (int)(cpu.r[0] & 0xffu);
}
int fixture_register_plugin(void) {
    memcpy(MEM + SERIAL_RODATA, serial_rodata, sizeof serial_rodata);
    put(SKSE, 0x01070030u); put(SKSE + 4, 0x09200000u);
    put(SKSE + 16, QIF); put(SKSE + 20, HANDLE);
    put(SERIAL, 4); put(SERIAL + 4, UID);
    put(SERIAL + 8, REG_REVERT); put(SERIAL + 12, REG_SAVE); put(SERIAL + 16, REG_LOAD);
    put(SERIAL + 24, WRITE); put(SERIAL + 36, NEXT); put(SERIAL + 40, READ);
    if (!invoke(SERIAL_QUERY, SKSE, INFO) || word(INFO) != 1 || word(INFO + 8) != 2) return 0;
    unsigned name = word(INFO + 4);
    if (name < SERIAL_RODATA || name - SERIAL_RODATA > sizeof serial_rodata - sizeof "SerialProbe" ||
        memcmp(MEM + name, "SerialProbe", sizeof "SerialProbe")) return 0;
    return invoke(SERIAL_LOAD, SKSE, 0);
}
void fixture_invoke_event(enum Event event) { (void)invoke(events[event], SERIAL, 0); }
