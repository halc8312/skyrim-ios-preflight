#!/usr/bin/env python3
"""Compare two local Classic .ess/.skse pairs by content, using the first as an independently trusted original."""
import argparse
from contextlib import ExitStack, contextmanager
import hashlib
import os
from pathlib import Path
import stat
import struct
import sys

MAX_ESS = 512 * 1024 * 1024
MAX_SKSE = 64 * 1024 * 1024
BLOCK = 256 * 1024
CLASSIC_RUNTIME = 0x09200000
ASCII_LOWER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


class PairCheckError(ValueError):
    pass


def check_local_path(path):
    """Reject reparse/offline entries on Windows and symbolic path components on all hosts."""
    if not path.is_absolute() or ".." in path.parts or path.anchor.startswith(("\\\\", "//")):
        raise PairCheckError("nonlocal_or_ambiguous_path")
    if os.name == "nt":
        import ctypes
        if ctypes.windll.kernel32.GetDriveTypeW(ctypes.c_wchar_p(path.anchor)) == 4:
            raise PairCheckError("network_drive")
    for component in (*reversed(path.parents), path):
        info = component.lstat()
        attributes = getattr(info, "st_file_attributes", 0)
        # Reparse point, offline, recall-on-open and recall-on-data-access.
        if stat.S_ISLNK(info.st_mode) or attributes & (0x400 | 0x1000 | 0x40000 | 0x400000):
            raise PairCheckError("link_or_nonlocal_file")


def local_file(path):
    if os.name != "nt":
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
        return os.fdopen(fd, "rb")
    import ctypes
    from ctypes import wintypes
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    open_file = kernel.CreateFileW
    open_file.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                          wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    open_file.restype = wintypes.HANDLE
    # Generic read, read sharing only, OPEN_EXISTING, no reparse traversal.
    handle = open_file(str(path), 0x80000000, 1, None, 3, 0x00200000 | 0x08000000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle(handle)
        raise
    return os.fdopen(fd, "rb")


def pair_paths(selected):
    path = Path(selected).absolute()
    if path.suffix.lower() != ".ess":
        raise PairCheckError("select_ess")
    check_local_path(path)
    # Match the in-app inspector: only ASCII letters are case-insensitive.
    base_name = path.name.translate(ASCII_LOWER)
    companion_name = path.with_suffix(".skse").name.translate(ASCII_LOWER)
    bases = 0
    matches = []
    with os.scandir(path.parent) as siblings:
        for item in siblings:
            name = item.name.translate(ASCII_LOWER)
            if name == base_name:
                bases += 1
            elif name == companion_name:
                matches.append(Path(item.path))
    if bases != 1:
        raise PairCheckError("missing_or_ambiguous_base")
    if len(matches) != 1:
        raise PairCheckError("missing_or_ambiguous_companion")
    check_local_path(matches[0])
    return path, matches[0]


def identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


@contextmanager
def open_pairs(original, copy):
    paths = (*pair_paths(original), *pair_paths(copy))
    with ExitStack() as stack:
        files = [stack.enter_context(local_file(path)) for path in paths]
        initial = []
        members = set()
        for i, (file, path) in enumerate(zip(files, paths)):
            info = os.fstat(file.fileno())
            named = path.lstat()
            if not stat.S_ISREG(info.st_mode) or not os.path.samestat(info, named):
                raise PairCheckError("file_identity")
            if not 0 < info.st_size <= (MAX_ESS if i % 2 == 0 else MAX_SKSE):
                raise PairCheckError("file_size")
            key = info.st_dev, info.st_ino
            if not info.st_ino or key in members:
                raise PairCheckError("same_file_or_identity_unavailable")
            members.add(key)
            # Windows pathname and open-handle stat APIs can expose different
            # change-time values for the same unchanged file. Compare each
            # view against its own baseline while holding all four file handles.
            initial.append((identity(info), identity(named)))
        yield files
        for file, path, (before, named) in zip(files, paths, initial):
            if before != identity(os.fstat(file.fileno())) or named != identity(path.lstat()):
                raise PairCheckError("file_changed")


def fingerprint(file, kind):
    size = os.fstat(file.fileno()).st_size
    header = file.read(21)
    if kind == "ess":
        if len(header) < 21 or header[:13] != b"TESV_SAVEGAME":
            raise PairCheckError("base_header")
        header_size, save_version = struct.unpack_from("<II", header, 13)
        if not 8 <= header_size < 65536 or 17 + header_size > size or not 1 <= save_version <= 9:
            raise PairCheckError("base_header")
    else:
        if len(header) < 20:
            raise PairCheckError("companion_header")
        magic, file_version, unused_skse_version, runtime, owners = struct.unpack_from("<4s4I", header)
        if magic != b"SKSE" or file_version != 1 or runtime != CLASSIC_RUNTIME or owners > min(100000, (size - 20) // 12):
            raise PairCheckError("companion_header")
    digest = hashlib.sha256(header)
    read = len(header)
    while read < size:
        data = file.read(min(BLOCK, size - read))
        if not data:
            raise PairCheckError("file_changed")
        digest.update(data)
        read += len(data)
    if file.read(1):
        raise PairCheckError("file_changed")
    return size, digest.digest()


def compare(original, received):
    with open_pairs(original, received) as files:
        values = [fingerprint(f, "skse" if i % 2 else "ess") for i, f in enumerate(files)]
    return {"ess_match": values[0] == values[2], "skse_match": values[1] == values[3],
            "reference_ess_bytes": values[0][0], "reference_skse_bytes": values[1][0],
            "received_ess_bytes": values[2][0], "received_skse_bytes": values[3][0]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original_ess", help="Independently trusted original .ess alongside its .skse")
    parser.add_argument("received_ess", help="Separate local copy of the .ess alongside its .skse")
    args = parser.parse_args(argv)
    try:
        results = compare(args.original_ess, args.received_ess)
    except PairCheckError as error:
        print("check_error=" + str(error), file=sys.stderr)
        return 2
    except OSError as error:
        print("check_error=filesystem errno=" + str(error.errno), file=sys.stderr)
        return 2
    print(" ".join(f"{name}={int(value)}" for name, value in results.items()))
    return 0 if results["ess_match"] and results["skse_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
