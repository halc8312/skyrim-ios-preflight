# Skyrim LE DLL compatibility experiment

The optional MOD screen checks a DLL selected from Files. It reads the file once and runs the SKSE initialization exports in an independent, bounded x86 interpreter. The result applies to initialization and the two data-free startup notifications inside that test environment.

The PE32 loader validates the x86 layout, section permissions, exports, import ownership and HIGHLOW relocations. Its mapped sections and temporary stack are ordinary data. A small table of virtual addresses provides the Classic SKSE release index and plugin handle, registration for `PostLoad` and `PostPostLoad`, Windows uptime and bounded CRT memory operations. The interpreter supports common repeated copies and stores on checked memory ranges. Unsupported imports, formats, instructions and SKSE calls have distinct results. Instruction count, bulk work and file/image sizes have explicit limits.

The app option `SKYRIM_IOS_SKSE_DLL_PROBE` defaults to OFF. In an experimental build, open Settings → MOD → Details → DLL Compatibility. SKSE gameplay, other DLL dependencies, Windows threads, native engine interfaces, and ENB rendering are outside the diagnostic's current coverage. The ordinary play path adds no per-frame work.

The tests generate their own DLLs. On Windows, `python -B -m unittest -v test_pe_image test_skse_plugin_vm` compares interpreted initialization to authored DLLs run by Windows, including relocations and CRT calls. On Mac, `bash test-apple-sdk.sh` also checks the iPhoneOS sources and runs the same UI controller in an iOS simulator. It supplies a file URL to the document picker's delegate and verifies the on-device diagnostic result in English and Japanese. Physical Files providers and gameplay need separate tests.
