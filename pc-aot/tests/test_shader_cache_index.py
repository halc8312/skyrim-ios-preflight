"""Pinned-writer differential checks and malformed synthetic cache records."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("shader_cache_index", ROOT / "experiments/shader-inputs/cache_index.py")
index = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(index)
SOURCE = ROOT / "vendor/skydream/third_party/dxvk"
HEADER = struct.pack("<4sII", b"DXVK", 10, 0)


def key(stage):
    return struct.pack("<I", stage) + hashlib.sha1(bytes([stage])).digest()


def payload(mask=17, attrs=0, bindings=0, constants=0):
    data = b"".join(key(stage) for stage, _ in index.STAGES if stage & mask)
    if mask & 32:
        data += bytes(48)
    else:
        data += bytes([1, 0, 0]) + bytes(16 + 48 + 2) + bytes([attrs, bindings])
        data += bytes(20 + 8 + 32 + attrs * 4 + bindings * 4)
    return data + struct.pack("<I", constants) + b"".join(struct.pack("<I", n + 1) for n in range(32) if constants & (1 << n))


def record(data, mask=17):
    return struct.pack("<I", mask | len(data) << 8) + hashlib.sha1(data).digest() + data


class CacheIndexTests(unittest.TestCase):
    def reject(self, raw, reason, **options):
        with self.assertRaises(index.Invalid) as caught:
            index.parse_cache(raw, **options)
        self.assertEqual(caught.exception.reason, reason)

    def test_all_stages_and_counts_are_not_usage_frequency(self):
        gp = record(payload(mask=31, attrs=32, bindings=32, constants=0x801), 31)
        cp = record(payload(mask=32, constants=0x800), 32)
        result = index.parse_cache(HEADER + gp + gp + cp)
        self.assertEqual(result["record_count"], 3)
        self.assertEqual(result["unique_recipes"], 2)
        self.assertEqual([r["kind"] for r in result["records"]], ["graphics", "graphics", "compute"])
        self.assertEqual(result["records"][0]["specialization_count"], 2)
        for stage, name in index.STAGES:
            identity = name + "_" + hashlib.sha1(bytes([stage])).hexdigest()
            self.assertEqual(result["key_recipe_counts"][identity], 1 if stage == 32 else 2)
        for name in ("render_semantics_verified", "replay_authorized", "usage_frequency_measured", "game_rendering_verified"):
            self.assertIs(result[name], False)

    def test_empty_cache_and_exact_file_limit(self):
        self.assertEqual(index.parse_cache(HEADER, max_bytes=12)["record_count"], 0)
        self.reject(HEADER, "file_size_limit", max_bytes=11)

    def test_every_nonboundary_truncation_is_rejected(self):
        raw = HEADER + record(payload(constants=0x801))
        for end in range(len(raw)):
            if end == len(HEADER):
                continue  # A complete empty cache is valid.
            with self.subTest(end=end), self.assertRaises(index.Invalid):
                index.parse_cache(raw[:end])

    def test_old_future_wrong_magic_and_noncanonical_header(self):
        self.reject(b"abcd" + HEADER[4:], "magic")
        for version in (0, 8, 9, 11, 0xffffffff):
            self.reject(struct.pack("<4sII", b"DXVK", version, 0), "unsupported_version")
        self.reject(struct.pack("<4sII", b"DXVK", 10, 1), "noncanonical_header")

    def test_checksum_detects_payload_mutation(self):
        raw = bytearray(HEADER + record(payload()))
        raw[-1] ^= 1
        self.reject(raw, "checksum")

    def test_masks_and_key_stages_are_cross_checked(self):
        for mask in (0, 64, 128, 255):
            self.reject(HEADER + record(payload(), mask), "stage_mask")
        self.reject(HEADER + record(payload(mask=33), 33), "mixed_compute_graphics")
        self.reject(HEADER + record(struct.pack("<I", 16) + payload()[4:]), "key_stage_mismatch")

    def test_length_and_entry_count_limits(self):
        for size in (0, 1025, 0xffffff):
            self.reject(HEADER + struct.pack("<I", size << 8 | 17), "entry_size")
        self.reject(HEADER + record(payload()) * 2, "entry_count_limit", max_entries=1)

    def test_layout_counts_and_trailing_bytes(self):
        self.reject(HEADER + record(payload(attrs=33)), "input_layout_counts")
        self.reject(HEADER + record(payload(bindings=33)), "input_layout_counts")
        self.reject(HEADER + record(payload() + b"padding"), "trailing_record_bytes")

    def test_specialization_bounds_and_nonzero_contract(self):
        self.reject(HEADER + record(payload(constants=1 << 12)), "specialization_mask")
        data = payload(constants=1)[:-4] + bytes(4)
        self.reject(HEADER + record(data), "noncanonical_zero_constant")

    def test_render_format_basic_checks(self):
        data = bytearray(payload())
        for offset, value, reason in ((48, 3, "sample_count"), (49, 1, "depth_layout"), (51, 1, "color_layout")):
            changed = data.copy()
            changed[offset] = value
            self.reject(HEADER + record(changed), reason)

    def test_output_cannot_touch_tracked_or_original_paths(self):
        with self.assertRaises(ValueError):
            index.inspect("unused", ROOT / "experiments/cache-index-output")
        with self.assertRaises(ValueError):
            index.inspect("unused", ROOT / "private/analysis")

    def test_private_root_link_cannot_redirect_output_into_vendor(self):
        with tempfile.TemporaryDirectory(prefix="shader-cache-output-") as temporary:
            root = Path(temporary).resolve()
            (root / "vendor").mkdir()
            (root / "private").mkdir()
            try:
                (root / "private/analysis").symlink_to(root / "vendor", target_is_directory=True)
            except OSError as error:
                self.skipTest("Symbolic links unavailable: " + type(error).__name__)
            with patch.object(index, "ROOT", root), self.assertRaises(ValueError):
                index.inspect("unused", root / "private/analysis/run")
            self.assertFalse((root / "vendor/run").exists())


def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


def native_writer():
    source = (SOURCE / "src/dxvk/dxvk_state_cache.cpp").read_text()
    assert hashlib.sha256(source.encode()).hexdigest() == "5b0942b71353c1731710f40a6f9573c8371c4cad65c661598aef06771b87026d"
    helpers = section(source, "  struct DxvkStateCacheEntryHeader {", "  template<typename T>\n  bool readCacheEntryTyped")
    writer = section(source, "  void DxvkStateCache::writeCacheEntry(", "  bool DxvkStateCache::convertEntryV2(")
    pack = section(source, "  uint8_t DxvkStateCache::packImageLayout(", "  VkImageLayout DxvkStateCache::unpackImageLayout(")
    return r'''
#include <fstream>
#include <iostream>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#if __has_warning("-Wnontrivial-memcall")
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#endif
#pragma clang diagnostic ignored "-Wunused-private-field"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#endif
#include "dxvk/dxvk_state_cache_types.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#else
#pragma GCC diagnostic pop
#endif
namespace dxvk {
static const DxvkShaderKey g_nullShaderKey;
''' + helpers + r'''
class DxvkStateCache {
public:
    static uint8_t packImageLayout(VkImageLayout);
    void writeCacheEntry(std::ostream&, DxvkStateCacheEntry&) const;
};
''' + writer + pack + r'''
}
int main(int argc, char** argv) {
    using namespace dxvk;
    if(argc != 2) return 2;
    static_assert(sizeof(DxvkShaderKey)==24 && sizeof(Sha1Hash)==20);
    static_assert(sizeof(DxvkBindingMask)==48 && sizeof(DxvkIlBinding)==4);
    static_assert(sizeof(DxvkIaInfo)==2 && sizeof(DxvkIlInfo)==2);
    static_assert(sizeof(DxvkRsInfo)==4 && sizeof(DxvkMsInfo)==4);
    static_assert(sizeof(DxvkDsInfo)==2 && sizeof(DxvkOmInfo)==2);
    static_assert(sizeof(DxvkDsStencilOp)==4 && sizeof(DxvkOmAttachmentSwizzle)==1);
    std::ofstream output(argv[1], std::ios::binary);
    DxvkStateCacheHeader header;
    output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    DxvkStateCache writer;
    for(unsigned ordinal=0; ordinal<4; ++ordinal) {
        DxvkStateCacheEntry entry;
        const unsigned mask = ordinal==3 ? 32 : ordinal==2 ? 31 : 17;
        auto* keys = &entry.shaders.vs;
        for(unsigned bit=0; bit<6; ++bit) {
            if(mask & (1u<<bit)) {
                uint8_t byte = uint8_t(1u<<bit);
                keys[bit] = DxvkShaderKey(VkShaderStageFlagBits(byte), Sha1Hash::compute(&byte,1));
                std::cout << ordinal << ':' << keys[bit].toString() << '\n';
            }
        }
        unsigned count = ordinal==2 ? 32 : ordinal;
        entry.gpState.il = DxvkIlInfo(count,count);
        if(ordinal==1) {
            entry.format.sampleCount = VK_SAMPLE_COUNT_4_BIT;
            entry.format.depth.format = VK_FORMAT_D32_SFLOAT;
            entry.format.depth.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
            entry.format.color[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            entry.format.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        auto& constants = ordinal==3 ? entry.cpState.sc : entry.gpState.sc;
        constants.specConstants[0]=3;
        constants.specConstants[11]=7;
        writer.writeCacheEntry(output,entry);
    }
    output.close();
    return output ? 0 : 1;
}
'''


@unittest.skipUnless(os.name == "posix" and shutil.which("c++"), "POSIX C++ pinned-writer oracle required")
class PinnedWriterTests(unittest.TestCase):
    def test_actual_writer_keys_and_lengths_match_python_index(self):
        with tempfile.TemporaryDirectory(prefix="shader-cache-writer-") as temporary:
            folder = Path(temporary)
            source, binary, cache = folder / "writer.cpp", folder / "writer", folder / "authored.dxvk-cache"
            source.write_text(native_writer())
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.getenv("SKYRIM_TEST_SANITIZERS") == "1" else []
            command = ["c++", "-std=c++17", "-O1", "-g", "-DDXVK_NATIVE", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-deprecated-declarations", *flags,
                       *["-I" + str(SOURCE / path) for path in ("src", "include", "include/native", "include/native/windows", "include/native/directx")],
                       "-x", "c++", str(source), str(SOURCE / "src/dxvk/dxvk_shader_key.cpp"),
                       str(SOURCE / "src/util/sha1/sha1_util.cpp"), str(SOURCE / "src/util/sha1/sha1.c"), "-o", str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr[-7000:])
            ran = subprocess.run([str(binary), str(cache)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stderr[-7000:])
            parsed = index.parse_cache(cache.read_bytes())
            actual = [str(row["ordinal"]) + ":" + key["dxvk_key"] for row in parsed["records"] for key in row["keys"]]
            self.assertEqual(actual, ran.stdout.splitlines())
            self.assertEqual([row["attributes"] for row in parsed["records"]], [0, 1, 32, 0])
            self.assertEqual([row["specialization_count"] for row in parsed["records"]], [2, 2, 2, 2])


if __name__ == "__main__":
    unittest.main()
