"""Stage private observation of the original runtime's file shims."""

import hashlib


KERNEL_SHA256 = "c6360da8a234421feac620c5db529d7a488ff6fc16747a0ad275f6dc987a0dfc"
GFILE = "typedef struct { int fd; char path[1024]; int shared; off_t pos;"

OPEN_RETURN = "    RET(h_new(H_FILE, f), 7);\n"
OPEN_OBSERVED = """    const int original_errno = errno;
    const uint64_t observed = classic_runtime_saved_opened(c, host, fd, bsa != 0);
    f->classic_save_token = observed;
    errno = original_errno;
    const uint32_t handle = h_new(H_FILE, f);
    if (observed) classic_runtime_saved_bound(c, observed, handle);
    RET(handle, 7);
"""

WRITE_CALL = "        r = write(f->fd, MEM + buf, n);\n"
WRITE_OBSERVED = """        const int observed_fd = f->fd;
        const uint64_t observed = f->classic_save_token;
        const bool tracking = observed && classic_runtime_saved_io(c, observed);
        r = write(observed_fd, MEM + buf, n);
        if (tracking) classic_runtime_saved_written(c, observed, observed_fd, n, r);
"""

WRITE_EX_CALL = "    ssize_t r = write ? pwrite(f->fd, MEM + buf, n, off) : pread(f->fd, MEM + buf, n, off);\n"
WRITE_EX_OBSERVED = """    const int observed_fd = f->fd;
    const uint64_t observed = write ? f->classic_save_token : 0;
    const bool tracking = observed && classic_runtime_saved_io(c, observed);
    ssize_t r = write ? pwrite(observed_fd, MEM + buf, n, off) : pread(observed_fd, MEM + buf, n, off);
    if (tracking) classic_runtime_saved_written(c, observed, observed_fd, n, r);
"""

CLOSE_CALL = "        if (!f->shared) close(f->fd);  /* shared BSA fds stay open */\n"
CLOSE_OBSERVED = """        if (!f->shared) {
            const int observed_fd = f->fd;
            const uint64_t observed = f->classic_save_token;
            if (observed && classic_runtime_saved_io(c, observed)) {
                struct stat before;
                const int before_errno = errno;
                const bool have_before = !fstat(observed_fd, &before);
                errno = before_errno;
                const int result = close(observed_fd);
                const int after_errno = errno;
                classic_runtime_saved_closed(c, observed, h, observed_fd, have_before ? &before : NULL,
                                             result, result ? after_errno : 0);
                errno = after_errno;
            } else close(observed_fd);
        }
"""

MOVE_CALL = "    int ok = rename(a, b) == 0;\n"
MOVE_OBSERVED = """    const int before_errno = errno;
    const bool tracking = classic_runtime_saved_move_begin(c, a, b);
    errno = before_errno;
    int ok = rename(a, b) == 0;
    if (tracking) classic_runtime_saved_move_end(c, ok, ok ? 0 : errno);
"""

SHIMS = (
    ("ClassicCreateFile", "CreateFileA"), ("ClassicWriteFile", "WriteFile"),
    ("ClassicWriteFileEx", "WriteFileEx"), ("ClassicFlushFileBuffers", "FlushFileBuffers"),
    ("ClassicCloseHandle", "CloseHandle"), ("ClassicMoveFile", "MoveFileA"), ("ClassicDeleteFile", "DeleteFileA"),
)


LOAD_CLONE_OPEN = """    f->classic_load_source_fd = -1;
    if (load) {
        const int snapshot = classic_runtime_loaded_prepare_clone(c, load, f->fd, &f->classic_load_source);
        if (snapshot >= 0) {
            f->classic_load_source_fd = f->fd;
            f->fd = snapshot;
        }
    }
"""
LOAD_CLONE_CLOSE = """        if (f->classic_load_source_fd >= 0) {
            const int saved_errno = errno;
            close(f->classic_load_source_fd);
            errno = saved_errno;
        }
"""
FILE_INFO = """SHIM(GetFileInformationByHandle) {
    GFile *f = h_get(ARG(0), H_FILE);
    struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0, 2);
"""
LOAD_FILE_FIXTURE = """
void classic_runtime_load_file_call(CPU *c, enum ClassicLoadFileCall operation) {
    switch (operation) {
    case ClassicLoadRead: shim_ReadFile(c); return;
    case ClassicLoadSeek: shim_SetFilePointer(c); return;
    case ClassicLoadSize: shim_GetFileSize(c); return;
    case ClassicLoadInformation: shim_GetFileInformationByHandle(c); return;
    }
    abort();
}
"""


def verified_source(raw: bytes) -> str:
    # The vendor checkout may use Git's Windows line-ending conversion.
    source = raw.replace(b"\r\n", b"\n")
    if hashlib.sha256(source).hexdigest() != KERNEL_SHA256:
        raise ValueError("The runtime does not match the reviewed Skydream source")
    return source.decode("utf-8")


def transform(source: str, load_witness: bool = False, load_clone: bool = False, *, app: bool = False) -> str:
    if load_clone and not load_witness:
        raise ValueError("Cloned load staging requires the read-only witness")
    load_header = '#include "runtime_load.h"\n' if load_witness else ""
    load_field = " uint64_t classic_load_generation;" if load_witness else ""
    observed_open = OPEN_OBSERVED
    observed_close = CLOSE_OBSERVED
    if load_witness:
        observed_open = observed_open.replace("    errno = original_errno;\n", """    const uint64_t load = classic_runtime_loaded_opened(c, f->path, f->fd, f->shared != 0, access, ARG(2), disp);
    f->classic_load_generation = load;
    errno = original_errno;
""", 1).replace("    RET(handle, 7);\n", """    if (load) classic_runtime_loaded_bound(c, load, handle);
    RET(handle, 7);
""", 1)
        observed_close = ("        if (f->classic_load_generation) classic_runtime_loaded_closed(f->classic_load_generation);\n"
                          + observed_close)
    if load_clone:
        load_field += " int classic_load_source_fd; struct stat classic_load_source;"
        observed_open = observed_open.replace("    errno = original_errno;\n",
                                             LOAD_CLONE_OPEN + "    errno = original_errno;\n", 1)
        observed_close = observed_close.replace("        if (!f->shared) {\n",
                                                LOAD_CLONE_CLOSE + "        if (!f->shared) {\n", 1)
    patches = (
        ('#include "rt.h"\n', '#include "rt.h"\n#include "runtime_save.h"\n' + load_header),
        (GFILE, GFILE + " uint64_t classic_save_token;" + load_field),
        (OPEN_RETURN, observed_open),
        (WRITE_CALL, WRITE_OBSERVED),
        (WRITE_EX_CALL, WRITE_EX_OBSERVED),
        (CLOSE_CALL, observed_close),
        ("SHIM(FlushFileBuffers) { RET(1, 1); }\n",
         "SHIM(FlushFileBuffers) { classic_runtime_saved_flush_stub(c); RET(1, 1); }\n"),
        (MOVE_CALL, MOVE_OBSERVED),
    )
    if load_clone:
        patches += ((FILE_INFO, FILE_INFO + "    if (f->classic_load_source_fd >= 0) st = f->classic_load_source;\n"),)
    if app:
        thread_exit = "static void thread_exit_slot(CPU *c) {\n"
        patches += (('#include "rt.h"\n', '#include "rt.h"\n#include "mod_save_host.h"\n'),
                    (thread_exit, thread_exit + "    skyrim_save_app_retire(c);\n"))
    for before, after in patches:
        if source.count(before) != 1:
            raise ValueError("Expected one reviewed runtime file observation site")
        source = source.replace(before, after, 1)

    if app:
        return source
    cases = "".join(f"    case {kind}: shim_{name}(c); return;\n" for kind, name in SHIMS)
    return (source + (LOAD_FILE_FIXTURE if load_clone else "") + "\nvoid classic_runtime_file_call(CPU *c, enum ClassicFileCall operation) {\n"
            "    switch (operation) {\n" + cases + "    }\n    abort();\n}\n")
