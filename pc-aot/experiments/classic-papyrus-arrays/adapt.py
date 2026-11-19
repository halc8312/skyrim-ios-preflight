"""Opt-in, host-tested adapters applied after the normal Classic runtime overlay."""
from prepare_runtime import replace_once


def adapt(name, text):
    if name == "skse_compat.h":
        return replace_once(text, " S_METHOD=0x8000",
                            " S_ARRAY_CREATE=1340,S_ARRAY_RESIZE,S_STRING_SPLIT,\n S_METHOD=0x8000", name)
    if name == "skse_compat_values.c":
        rows = []
        for label, element in (("Int", 3), ("Float", 4), ("String", 2)):
            array = element + 10
            rows.append(f'    F("Utility","Create{label}Array",{array},2,3,{element},0,0,S_ARRAY_CREATE),')
            rows.append(f'    F("Utility","Resize{label}Array",{array},3,{array},3,{element},0,S_ARRAY_RESIZE),')
        rows.append('    F("StringUtil","Split",12,2,2,2,0,0,S_STRING_SPLIT),')
        return replace_once(text, "\n};\n#undef F", "\n" + "\n".join(rows) + "\n};\n#undef F", name)
    if name == "skse_system.c":
        start = "int skse_system_run("
        text = replace_once(text, start, '#include "classic_skse_arrays.h"\n' + start, name)
        operation = "    unsigned op=fn->operation&0x7fff;\n"
        return replace_once(text, operation,
                            operation + "    if (op >= S_ARRAY_CREATE && op <= S_STRING_SPLIT) {\n"
                            "        skse_classic_array_run(c, fn, args, registry, result);\n"
                            "        return 1;\n"
                            "    }\n", name)
    return text
