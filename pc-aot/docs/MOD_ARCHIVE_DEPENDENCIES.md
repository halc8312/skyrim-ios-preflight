# MOD archive dependencies

`bash scripts/build-dependencies.sh` prepares the iPhoneOS renderer and the
static libraries used to read 7z, RAR and RAR5 packages. The renderer keeps its
own build recipe and shader-cache identity. ZIP imports use the existing zlib
reader. The app enables the other three formats when both static archives,
the libarchive headers and their distribution notices are available in the
iPhoneOS prefix.

For a renderer-only prefix, run `bash ios/dependencies/build-dependencies.sh`
and fetch the texture headers as usual. The app then accepts ZIP and folder
imports and identifies 7z/RAR as unavailable. A separate archive prefix can
be supplied with `-DSKYRIM_MOD_ARCHIVE_PREFIX=/path/to/prefix`. The Mac host
workflow links the iPhone app once with an absent archive prefix, then again
with the pinned libraries and bundled notices for distribution.

The archive recipe verifies the official release tarballs by SHA-256. It builds
the SDK's zlib support and the pinned LZMA decoder into the reader's dependency
chain. For example, 7z packages using LZMA or LZMA2 are supported. A 7z method
requiring a disabled optional codec, such as bzip2 or Zstandard, can fail to
extract. Encrypted archives are refused by the importer.

| Library | Pinned release and license |
| --- | --- |
| libarchive 3.8.11 | Predominantly BSD-2-Clause. Upstream lists per-file exceptions, including its BLAKE2 sources used for RAR5, for which this build uses the CC0 option. The app bundle carries upstream's license and source notices in `ModArchiveLicenses.txt`. [Upstream license](https://github.com/libarchive/libarchive/blob/03036ffa409a2aa1c5bbad35be2202641a040fda/COPYING) |
| XZ Utils 5.8.4 | The linked liblzma uses 0BSD. The command-line programs and scripts are excluded. [Upstream license](https://github.com/tukaani-project/xz/blob/d3e650e63c110e830fd5391e7f8b45df0b91d3da/COPYING) |
| bcdec and stb_dxt | The texture importer's verified headers each offer a public-domain dedication and fallback license under the Unlicense. This build uses that option. [bcdec.h](https://github.com/iOrange/bcdec/blob/80859ed3b7afb1c527a2a99d70c61457bea72d0c/bcdec.h) · [stb_dxt.h](https://github.com/nothings/stb/blob/9600522de53973336fa033a289858a4354cbb04c/stb_dxt.h) |

`bash ios/dependencies/build-mod-archive-dependencies.sh macosx` prepares the
native Mac libraries for importer tests. The `iphonesimulator` argument builds
the same sources against the simulator SDK. Each uses its own prefix below
`$SKYRIM_IOS_DEPS_ROOT/mod-archive/<platform>/prefix`. Set
`SKYRIM_MOD_ARCHIVE_TEST_PREFIX` to the native prefix to require the pinned
libraries in `tests/test_mod_libarchive.py`.
