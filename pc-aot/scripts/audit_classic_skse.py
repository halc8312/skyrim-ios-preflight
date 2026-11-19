"""Compare classic SKSE 1.7.3 Papyrus names with Skydream's source table offline.

Run: python scripts/audit_classic_skse.py SKSE_DISTRIBUTION SKYDREAM_CHECKOUT [--details]
Use --candidate to compare a locally changed Skydream table against the same official baseline.
Only interface names, counts and input fingerprints are emitted. No game files are needed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


# SHA-256 over the official 7z's sorted *.cpp basenames, NUL and per-file SHA-256.
# Both fingerprints normalize CRLF to LF so Windows checkouts give the same result.
OFFICIAL_SOURCES = "33886d4a6e08e446a6fe61bd85abfcbd11769099ced8abc248d6012c30500ba8"
SKYDREAM_REVISION = "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a"
SKYDREAM_TABLE = "45ff5d9de3a27cf398a19bd55599031511e7334a58ca312b64767553105383e1"

# Read comments and entire literals as tokens. Registration-like text inside a
# string, raw string, character literal or comment must never become an API name.
TOKEN = re.compile(
    r'//[^\n]*|/\*.*?\*/|(?:u8|u|U|L)?R"(?P<tag>[^\s()\\]{0,16})\(.*?\)(?P=tag)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[a-zA-Z_]\w*|[0-9]+|[^\s]', re.S)
# Preserve the actual registered spelling, including anomalous class strings.
# A bare identifier (e.g. a macro or variable) isn't a known literal name.
LITERAL_NAME = re.compile(r'"([^"\\\x00-\x1f]+)"')
NATIVE = re.compile(r'(?:Latent)?NativeFunction[0-9]+')


def tokens(text):
    return [m[0] for m in TOKEN.finditer(text.replace("\\\n", ""))
            if not m[0].startswith(("//", "/*"))]


def read_lf(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def after_group(items, pos, opening, closing):
    if items[pos:pos + 1] != [opening]:
        raise ValueError(f"expected {opening} in a native registration")
    depth = 1
    for index in range(pos + 1, len(items)):
        token = items[index]
        if token == opening:
            depth += 1
        elif token == closing:
            depth -= 1
            if depth == 0:
                return index + 1
        elif token == ";":
            break
    raise ValueError(f"unterminated {opening}{closing} in a native registration")


def read_names(items, pos):
    args = items[pos:pos + 5]
    if len(args) != 5 or args[0] != "(" or args[2] != "," or args[4] != ",":
        raise ValueError("unrecognized native registration arguments")
    first, second = LITERAL_NAME.fullmatch(args[1]), LITERAL_NAME.fullmatch(args[3])
    if not first or not second:
        raise ValueError("expected literal Papyrus identifiers in a native registration")
    return first[1], second[1]


def official_names(text):
    items = tokens(text)
    for i, token in enumerate(items[:-1]):
        if token == "new" and NATIVE.fullmatch(items[i + 1]):
            pos = after_group(items, i + 2, "<", ">")
            name, cls = read_names(items, pos)
            yield cls, name


def skydream_names(text):
    items = tokens(text)
    signature = ["SkseNative", "natives", "[", "]", "=", "{"]
    starts = [i for i, token in enumerate(items) if token == signature[0]
              and items[i:i + len(signature)] == signature]
    if len(starts) != 1:
        raise ValueError("expected one SkseNative natives[] table")
    pos = starts[0] + len(signature)
    while pos < len(items) and items[pos] == "F":
        yield read_names(items, pos + 1)  # The table puts the class first.
        pos = after_group(items, pos + 1, "(", ")")
        if items[pos:pos + 1] == [","]:
            pos += 1
        else:
            break
    if items[pos:pos + 2] != ["}", ";"]:
        raise ValueError("unrecognized row or end of Skydream's native table")


def check_version(header):
    parts = ["SKSE_VERSION_INTEGER", "SKSE_VERSION_INTEGER_MINOR", "SKSE_VERSION_INTEGER_BETA"]
    items = tokens(header)
    values = {items[i + 2]: items[i + 3] for i in range(len(items) - 3)
              if items[i:i + 2] == ["#", "define"] and items[i + 2] in parts}
    if [values.get(part) for part in parts] != ["1", "7", "3"]:
        raise ValueError("expected the classic SKSE 1.7.3 version header")


def compare(official, skydream, details):
    # Papyrus identifiers are case insensitive. Display the official spelling
    # when a name is shared; identically named functions on different classes differ.
    def by_name(rows):
        return {(cls.casefold(), name.casefold()): (cls, name) for cls, name in rows}

    reference, candidate = by_name(official), by_name(skydream)
    if not reference or not candidate:
        raise ValueError("an input contains no comparable native registrations")
    groups = {"shared": reference.keys() & candidate.keys(),
              "official_only": reference.keys() - candidate.keys(),
              "skydream_only": candidate.keys() - reference.keys()}
    names = {**candidate, **reference}
    classes = {}
    for (cls, _), (display, _) in sorted(names.items()):
        classes.setdefault(cls, display)
    by_class = {display: dict.fromkeys(groups, 0) for display in classes.values()}
    for group, keys in groups.items():
        for cls, _ in keys:
            by_class[classes[cls]][group] += 1
    report = {
        "counts": {"official": len(reference), "skydream": len(candidate),
                   **{group: len(keys) for group, keys in groups.items()}},
        "per_class": by_class,
    }
    if details:
        report["names"] = {group: [".".join(names[key]) for key in sorted(keys)]
                           for group, keys in groups.items()}
    return report


def audit(skse, skydream, details=False, candidate=False):
    source = skse / "src/skse/skse"
    check_version(read_lf(source / "skse_version.h").decode("latin1"))
    fingerprint = hashlib.sha256()
    registrations = []
    for path in sorted(source.glob("*.cpp"), key=lambda file: file.name):
        contents = read_lf(path)
        fingerprint.update(path.name.encode("utf-8") + b"\0" + hashlib.sha256(contents).digest())
        registrations.extend(official_names(contents.decode("latin1")))
    if fingerprint.hexdigest() != OFFICIAL_SOURCES:
        raise ValueError("incomplete or modified official sources; use the unmodified classic 1.7.3 distribution")
    table = read_lf(skydream / "runtime/skse_compat_values.c")
    table_hash = hashlib.sha256(table).hexdigest()
    if table_hash != SKYDREAM_TABLE and not candidate:
        raise ValueError("Skydream table differs from the project pin; use --candidate for a local change")
    coverage = compare(registrations, skydream_names(table.decode("latin1")), details)
    return {
        "schema": 1, "skse_version": "1.7.3", "skse_source_fingerprint": fingerprint.hexdigest(),
        "skydream_reference_revision": SKYDREAM_REVISION, "skydream_table_sha256": table_hash,
        "skydream_table_matches_reference": table_hash == SKYDREAM_TABLE,
        "scope": ("Literal Papyrus registration names in C/C++ source, excluding comments and including "
                  "build-conditional code. Matching names do not verify signatures, behavior, DLL support, "
                  "co-saves, or execution on iPhone."),
        **coverage,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("skse", type=Path, help="extracted official SKSE 1.7.3 directory")
    parser.add_argument("skydream", type=Path, help="Skydream source directory")
    parser.add_argument("--details", action="store_true", help="list the qualified names in each group")
    parser.add_argument("--candidate", action="store_true", help="also accept a locally changed Skydream table")
    args = parser.parse_args()
    try:
        result = audit(args.skse, args.skydream, args.details, args.candidate)
    except (OSError, ValueError) as error:
        parser.exit(2, f"Audit failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
