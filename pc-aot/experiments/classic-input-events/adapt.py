"""Opt-in Classic live input experiment. Apply after the standard Classic adapter."""
import hashlib

from prepare_runtime import replace_once

PINNED_INPUT = {
    "skse_input.c": "e7545db20bf742f2981b3c73299ee8567cd3c2ddfe4e9f4c11ed55916094591d",
    "skse_runtime.h": "205548582c1b1cdb1d7775decffeff3bad6625ab65284575e8e0a59cef262327",
}

MENU_POLL = """static void pump_events(CPU *c) {
    typedef struct MenuDelivery {
        struct MenuDelivery *next;
        Registration recipient;
        int opened;
    } MenuDelivery;
    MenuDelivery *pending = NULL;
    pthread_mutex_lock(&event_lock);
    for (Registration *r = registrations; r; r = r->next) {
        if (r->kind != 0) continue;
        int opened = !!skse_menu_view(r->name);
        if ((unsigned)opened == r->active) continue;
        MenuDelivery *delivery = calloc(1, sizeof *delivery);
        if (!delivery) rt_fatal("SKSE menu delivery allocation failed");
        delivery->recipient = *r;
        delivery->recipient.name = strdup(r->name);
        if (!delivery->recipient.name) rt_fatal("SKSE menu name allocation failed");
        handle_ref(c, r->handle, 1);
        delivery->opened = opened;
        delivery->next = pending;
        pending = delivery;
        r->active = opened;
    }
    pthread_mutex_unlock(&event_lock);
    while (pending) {
        MenuDelivery *d = pending;
        pending = d->next;
        uint32_t value = gmalloc(8);
        memset(GPTR(value), 0, 8);
        vm_string(c, value, d->recipient.name);
        send_event(c, &d->recipient, d->opened ? "OnMenuOpen" : "OnMenuClose", 1, value);
        vm_clear(c, value);
        gfree(value);
        handle_ref(c, d->recipient.handle, 0);
        free(d->recipient.name);
        free(d);
    }
}
"""


def adapt(name, text):
    if name in PINNED_INPUT and hashlib.sha256(text.encode()).hexdigest() != PINNED_INPUT[name]:
        raise ValueError(f"{name}: different from the reviewed Classic input source")
    if name == "skse_runtime.h":
        line = "void skse_input_install(CPU *);"
        return replace_once(text, line, line + "\nvoid skse_classic_button_event(CPU *,uint32_t,uint32_t,int);", name)
    if name == "skse_input.c":
        text = replace_once(text, "if(device==0) return key;", "if(device!=1 && device!=2) return key;", name)
        text = replace_once(text, "if(device>2 || key>=282) continue;", "if(key>=282) continue;", name)
        line = '        skse_gfx_input_event(c,key,control?control:"",down);'
        return replace_once(text, line, "        if (device <= 2) " + line.strip()
                            + "\n        skse_classic_button_event(c, event, key, down);", name)
    if name == "skse_compat.h":
        return replace_once(text, " S_METHOD=0x8000",
                            " S_REG_CONTROL=1350,S_UNREG_CONTROL,S_UNREG_CONTROLS,\n S_METHOD=0x8000", name)
    if name == "skse_compat_values.c":
        rows = []
        for owner in ("Form", "Alias", "ActiveMagicEffect"):
            for method, args, operation in (("RegisterForControl", "1,2", "S_REG_CONTROL"),
                                            ("UnregisterForControl", "1,2", "S_UNREG_CONTROL"),
                                            ("UnregisterForAllControls", "0,0", "S_UNREG_CONTROLS")):
                rows.append(f'    F("{owner}","{method}",0,{args},0,0,0,S_METHOD|{operation}),')
        return replace_once(text, "\n};\n#undef F", "\n" + "\n".join(rows) + "\n};\n#undef F", name)
    if name != "skse_system.c":
        return text
    # IFunctionArguments::Output::Resize returns C++ bool in AL. Keep the VM
    # buffer unmodified on a false, short or absent result.
    text = replace_once(text, """    if(!skse_member(c,0x8C2A70,out,1,&count)) RET(0,1);
    for(unsigned i=0;i<count;i++) vm_copy(c,M32(out+4)+8*i,self+8+8*i);""",
                        """    if (!out || out > UINT32_MAX - 15 ||
        !(uint8_t)skse_member(c, 0x8C2A70, out, 1, &count)) RET(0,1);
    uint32_t data = M32(out + 4);
    if (count > M32(out + 0x0c) || (count && (!data ||
        (uint64_t)data + (uint64_t)count * 8 > UINT64_C(0x100000000)))) RET(0,1);
    for (uint32_t i = 0; i < count; ++i) vm_copy(c, data + 8*i, self + 8 + 8*i);""", name)
    # The VM input observer can run concurrently with a Papyrus mod callback.
    # Publish the arguments vtable only after both callbacks are ready.
    event = text.index("static void send_event(")
    init_start = text.index("    if(!event_vtable) {", event)
    init_end = text.index("    uint32_t object=", init_start)
    init = ("static pthread_once_t classic_event_once = PTHREAD_ONCE_INIT;\n"
            "static void classic_init_event_vtable(void) {\n"
            "    event_vtable = gmalloc(8);\n"
            '    M32(event_vtable) = rt_thunk(event_destroy, "SKSE.Event.Destroy");\n'
            '    M32(event_vtable + 4) = rt_thunk(event_copy, "SKSE.Event.Copy");\n'
            "}\n")
    text = (text[:event] + init + text[event:init_start]
            + "    pthread_once(&classic_event_once, classic_init_event_vtable);\n" + text[init_end:])
    start = text.index("static void pump_events(CPU *c) {")
    end = text.index("void skse_ui_pump(void) {", start)
    text = text[:start] + MENU_POLL + text[end:]
    text = replace_once(text, "    uint32_t down_tick;\n", "", name)
    # All key registrations now share the event source with named controls.
    text = replace_once(text, "unsigned kind=op>=S_REG_MOD ? 2 : op>=S_REG_KEY ? 1 : 0;",
                        "unsigned kind=op>=S_REG_MOD ? 2 : 0;", name)
    text = replace_once(text, "op==S_REG_MENU || op==S_REG_KEY || op==S_REG_MOD",
                        "op==S_REG_MENU || op==S_REG_MOD", name)
    text = replace_once(text, "op==S_UNREG_MENUS || op==S_UNREG_KEYS || op==S_UNREG_MODS",
                        "op==S_UNREG_MENUS || op==S_UNREG_MODS", name)
    text = replace_once(text, 'kind==1 ? "" : str(args[0])', "str(args[0])", name)
    text = replace_once(text, "(kind==1 ? r->key==args[0] : !strcmp(r->name,name))",
                        "!strcmp(r->name,name)", name)
    enter = "int skse_system_run("
    text = replace_once(text, enter, '#include "classic_input_events.h"\n' + enter, name)
    op = "    unsigned op=fn->operation&0x7fff;\n"
    return replace_once(text, op, op + """    if ((op >= S_REG_KEY && op <= S_UNREG_KEYS) ||
        (op >= S_REG_CONTROL && op <= S_UNREG_CONTROLS)) {
        if (!skse_classic_input_subscribe(c, fn->class_name, op, registry, base, args[0])) return 0;
        vm_clear(c, result);
        M32(result) = 0;
        M32(result + 4) = 0;
        return 1;
    }
""", name)
