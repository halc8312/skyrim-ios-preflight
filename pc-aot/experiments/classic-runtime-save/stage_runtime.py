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


def verified_source(raw: bytes) -> str:
    # The vendor checkout may use Git's Windows line-ending conversion.
    source = raw.replace(b"\r\n", b"\n")
    if hashlib.sha256(source).hexdigest() != KERNEL_SHA256:
        raise ValueError("The runtime does not match the reviewed Skydream source")
    return source.decode("utf-8")


def transform(source: str) -> str:
    patches = (
        ('#include "rt.h"\n', '#include "rt.h"\n#include "runtime_save.h"\n'),
        (GFILE, GFILE + " uint64_t classic_save_token;"),
        (OPEN_RETURN, OPEN_OBSERVED),
        (WRITE_CALL, WRITE_OBSERVED),
        (WRITE_EX_CALL, WRITE_EX_OBSERVED),
        (CLOSE_CALL, CLOSE_OBSERVED),
        ("SHIM(FlushFileBuffers) { RET(1, 1); }\n",
         "SHIM(FlushFileBuffers) { classic_runtime_saved_flush_stub(c); RET(1, 1); }\n"),
        (MOVE_CALL, MOVE_OBSERVED),
    )
    for before, after in patches:
        if source.count(before) != 1:
            raise ValueError("Expected one reviewed runtime file observation site")
        source = source.replace(before, after, 1)

    cases = "".join(f"    case {kind}: shim_{name}(c); return;\n" for kind, name in SHIMS)
    return (source + "\nvoid classic_runtime_file_call(CPU *c, enum ClassicFileCall operation) {\n"
            "    switch (operation) {\n" + cases + "    }\n    abort();\n}\n")
