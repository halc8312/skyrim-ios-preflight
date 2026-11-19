"""Adapt the classic translation reader to the selected iOS profile."""
import hashlib


def adapt(source_text: str) -> str:
    expected = "8dacae49458c91db29b15790d449295ed8c3b69e1efc16072ac6b1761b43a2d0"
    if hashlib.sha256(source_text.encode("utf-8")).hexdigest() != expected:
        raise ValueError("skse_translation.c differs from the pinned public source")
    return source_text.replace(
        "#ifdef SKYDREAM_SKSE\n",
        '#ifdef SKYDREAM_SKSE\n#include "classic_skse_translation_list.h"\n', 1
    ).replace(
        '    char listpath[1400];snprintf(listpath,sizeof listpath,"%s/Documents/Skyrim/Plugins.txt",g_save_dir);\n'
        '    FILE *f=fopen(listpath,"rb");',
        '    FILE *f = skyrim_skse_open_plugin_list(g_save_dir);', 1
    )
