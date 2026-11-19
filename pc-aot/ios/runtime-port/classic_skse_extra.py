"""Adapt Classic Papyrus control lookups and typed reads from Scaleform."""

from prepare_runtime import replace_once


def adapt(name, text):
    if name == "skse_compat.h":
        return replace_once(text, " S_METHOD=0x8000", " S_INPUT_KEY=1320,\n S_METHOD=0x8000", name)
    if name == "skse_compat_values.c":
        return replace_once(text, '    F("Input","GetMappedControl",2,2,3,3,0,0,S_INPUT_CONTROL),',
                            '    F("Input","GetMappedControl",2,1,3,0,0,0,S_INPUT_CONTROL),\n'
                            '    F("Input","GetMappedKey",3,2,2,3,0,0,S_INPUT_KEY),', name)
    if name != "skse_system.c":
        return text
    entry = "int skse_system_run("
    text = replace_once(text, entry, '#include "classic_skse_extra.h"\n'
                        '#include "classic_skse_ui_values.h"\n' + entry, name)
    start = text.index("    if(op>=S_UI_GBOOL && op<=S_UI_GSTRING) {")
    end = text.index("\n    }\n", start) + len("\n    }\n")
    text = replace_once(text, text[start:end],
                        "    if (op >= S_UI_GBOOL && op <= S_UI_GSTRING) {\n"
                        "        skse_classic_ui_read(c, op, args, result);\n"
                        "        return 1;\n"
                        "    }\n", name)
    start = text.index("    if(op==S_INPUT_CONTROL) {")
    end = text.index("\n    }\n", start) + len("\n    }\n")
    return replace_once(text, text[start:end],
                        "    if (op == S_INPUT_CONTROL || op == S_INPUT_KEY) {\n"
                        "        skse_classic_input_query(c, op, args, result);\n"
                        "        return 1;\n"
                        "    }\n", name)
