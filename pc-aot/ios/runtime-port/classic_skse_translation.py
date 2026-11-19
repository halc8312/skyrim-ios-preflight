"""Adapt the classic translation reader to the selected iOS profile."""
import hashlib


def adapt_profile_list(source_text: str) -> str:
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


def adapt(source_text: str) -> str:
    source = adapt_profile_list(source_text)
    # Keep the pinned resource constructor, validity check and destructor in
    # their original ABI. A prepared SkyUI stream stays open until its turn in
    # the plugin list. No dictionary entries are inserted on a deferred pass.
    start = source.index("static unsigned import_file(")
    ctor = source.index("    uint32_t filename=gstrdup(path)", start)
    finish_ctor = source.index("    unsigned imported=0;", ctor)
    valid = source.index("    if(skse_member(", finish_ctor)
    body = source.index("        uint32_t read[]", valid)
    end_close = source.index("gfree(buffer);", body)
    close = source.rindex("\n", body, end_close) + 1
    close_stream = source[close:end_close].strip()
    condition = source[valid:body].strip().removesuffix(" {")
    source = (source[:start] + "static void close_translation_stream(CPU *c,uint32_t stream) {\n"
              + "    " + close_stream + "\n}\n"
              + "static uint32_t open_translation_stream(CPU *c,const char *path) {\n"
              + "    uint32_t stream=gmalloc(32);memset(GPTR(stream),0,32);\n"
              + source[ctor:finish_ctor] + "    " + condition + " return stream;\n"
              + "    close_translation_stream(c,stream);return 0;\n}\n"
              + "static unsigned import_file(CPU *c,uint32_t translator,const char *path,uint32_t stream) {\n"
              + "    if(!stream) stream=open_translation_stream(c,path);\n"
              + "    if(!stream) return 0;\n"
              + "    uint32_t buffer=gmalloc(1024*1024+2),scratch=gmalloc(12);\n"
              + "    memset(GPTR(scratch),0,12);\n    unsigned imported=0;\n    {\n"
              + source[body:close] + "    close_translation_stream(c,stream);" + source[end_close:])
    begin = source.index("void skse_import_translations(CPU *c) {")
    before_setting = source.index("    uint32_t setting=skse_ini_setting(", begin)
    released = source.index("    if(!__atomic_sub_fetch(", before_setting)
    fast_path = "    if(loader==translated_loader) {pthread_mutex_unlock(&translation_lock);return;}\n"
    entry = source[begin:before_setting].replace(
        fast_path, fast_path +
        "    if(!translation_attempt_due(loader)) {pthread_mutex_unlock(&translation_lock);return;}\n"
    )
    return (source[:begin] + '#include "classic_skse_translation_import.h"\n' + entry +
            "    if(import_plugin_translations(c,translator) || !translation_defer()) translated_loader=loader;\n"
            + source[released:])
