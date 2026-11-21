# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.24)
project(SkyrimIOSRuntime LANGUAGES C CXX OBJC)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "This target requires an iOS device toolchain")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "The runtime diagnostic target supports iPhone arm64 only")
endif()
if(CMAKE_OSX_SYSROOT MATCHES "[Ii][Pp]hone[Ss]imulator")
    message(FATAL_ERROR "The runtime diagnostic target requires the iOS device SDK")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)

get_filename_component(SKYRIM_IOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SKYDREAM_SOURCE_DIR "${SKYRIM_IOS_ROOT}/vendor/skydream" CACHE PATH "Pinned public source")
set(SKYRIM_IOS_RUNTIME_OVERLAY_DIR "" CACHE PATH "Previously prepared public runtime; ABI overlay may be applied here")
set(SKYRIM_IOS_BINDING_INCLUDE_DIR "${SKYRIM_IOS_ROOT}/ios/private-abi" CACHE PATH "Public PcGuestABI.h directory")
set(SKYRIM_IOS_BINDING_SOURCES "" CACHE STRING "Public dynamic guest-binding source files")
option(SKYRIM_IOS_BUILD_APP "Link the public iOS application shell" OFF)
option(SKYRIM_IOS_CLASSIC_SKSE "Include the opt-in Classic Papyrus and SkyUI support" OFF)
option(SKYRIM_IOS_SKSE_DLL_PROBE "Include the isolated LE DLL compatibility check" OFF)
option(SKYRIM_IOS_CLASSIC_SETTINGS_PROBE "Exercise Classic profile settings in an isolated launch probe" OFF)
if(SKYRIM_IOS_CLASSIC_SETTINGS_PROBE AND NOT (SKYRIM_IOS_CLASSIC_SKSE AND SKYRIM_IOS_SKSE_DLL_PROBE))
    message(FATAL_ERROR "The Classic settings probe requires Classic SKSE and the DLL probe")
endif()
option(SKYRIM_IOS_MOD_HOOKS "Bind the optional guest MOD hook interface (PcGuestModHooksABI.h)" OFF)
if(SKYRIM_IOS_MOD_HOOKS AND NOT SKYRIM_IOS_CLASSIC_SKSE)
    message(FATAL_ERROR "The guest MOD hook interface requires Classic SKSE")
endif()
set(SKYRIM_IOS_MOD_HOOK_SEAMS "${SKYRIM_IOS_ROOT}/experiments/fuz-voice")
option(SKYRIM_IOS_CLASSIC_PAPYRUSUTIL "Publish PapyrusUtil JsonUtil natives during play (MOD_FIRST_WAVE_PLAN P3-2)" OFF)
if(SKYRIM_IOS_CLASSIC_PAPYRUSUTIL AND NOT (SKYRIM_IOS_CLASSIC_SKSE AND SKYRIM_IOS_SKSE_DLL_PROBE AND SKYRIM_IOS_MOD_HOOKS))
    message(FATAL_ERROR "PapyrusUtil during play requires Classic SKSE, the DLL probe and the MOD hook interface")
endif()
set(SKYRIM_IOS_BUNDLE_IDENTIFIER "local.skyrim.ios.diagnostic" CACHE STRING "Local application identifier")

if(NOT TARGET skyrim_ios_dependencies)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../dependencies" dependencies)
endif()
if(NOT EXISTS "${SKYRIM_IOS_BINDING_INCLUDE_DIR}/PcGuestABI.h")
    message(FATAL_ERROR "Public PcGuestABI.h is required; no placeholder guest binding is generated")
endif()

if(NOT SKYRIM_IOS_RUNTIME_OVERLAY_DIR OR SKYRIM_IOS_CLASSIC_SKSE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
if(NOT SKYRIM_IOS_RUNTIME_OVERLAY_DIR)
    set(SKYRIM_IOS_RUNTIME_OVERLAY_DIR "${CMAKE_CURRENT_BINARY_DIR}/public-runtime")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_runtime.py"
        --source "${SKYDREAM_SOURCE_DIR}" --output "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}"
        RESULT_VARIABLE runtime_patch_result)
    if(NOT runtime_patch_result EQUAL 0)
        message(FATAL_ERROR "Pinned public runtime overlay preparation failed")
    endif()
endif()
if(NOT EXISTS "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/main.c")
    message(FATAL_ERROR "Prepared public runtime directory does not contain main.c")
endif()
if(SKYRIM_IOS_CLASSIC_SKSE)
    if(NOT EXISTS "${SKYRIM_IOS_BINDING_INCLUDE_DIR}/PcGuestSkseABI.h")
        message(FATAL_ERROR "Classic Papyrus support requires the matching guest ABI header")
    endif()
    if(SKYRIM_IOS_MOD_HOOKS AND (NOT EXISTS "${SKYRIM_IOS_BINDING_INCLUDE_DIR}/PcGuestModHooksABI.h"
            OR NOT EXISTS "${SKYRIM_IOS_MOD_HOOK_SEAMS}/lift_world_seam.h"))
        message(FATAL_ERROR "The MOD hook interface requires its ABI header and the reviewed seam headers")
    endif()
    # The Classic adapters change some ordinary runtime entry points as well. Use a separate
    # build-tree copy so switching the option off or sharing the input overlay is safe.
    file(GLOB classic_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}"
        "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/*.c" "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/*.cpp"
        "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/*.h" "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/*.in")
    string(SHA256 classic_file_set "${classic_inputs}")
    set(classic_overlay "${CMAKE_CURRENT_BINARY_DIR}/classic-${classic_file_set}")
    foreach(file IN LISTS classic_inputs)
        configure_file("${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/${file}" "${classic_overlay}/runtime/${file}" COPYONLY)
    endforeach()
    set(SKYRIM_IOS_RUNTIME_OVERLAY_DIR "${classic_overlay}/runtime")
    set(classic_adapter "${CMAKE_CURRENT_SOURCE_DIR}/prepare_classic_skse.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${classic_adapter}" "${CMAKE_CURRENT_SOURCE_DIR}/classic_skse_translation.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/classic_skse_extra.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../private-abi/prepare_private_abi.py"
        "${classic_overlay}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${classic_adapter}"
        --source "${SKYDREAM_SOURCE_DIR}" --runtime "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endif()

file(GLOB runtime_sources CONFIGURE_DEPENDS
    "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/*.c" "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}/*.cpp")
# These are fixed game replacement objects or require the private linked image.
# The private binding owns g_modules and related dynamic dispatch metadata.
list(FILTER runtime_sources EXCLUDE REGEX "/(modules|embedded_image|embedded_smoke|engine_primitives_bridge|engine_strings_bridge|physics_timing_bridge)\\.c$")
if(NOT SKYRIM_IOS_CLASSIC_SKSE)
    list(FILTER runtime_sources EXCLUDE REGEX "/skse_[^/]*\\.c$")
endif()
add_library(skyrim_ios_runtime STATIC ${runtime_sources}
    "${SKYDREAM_SOURCE_DIR}/third_party/f3daudio/F3DAudio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ios_runtime.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mobile_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_language.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_plugins.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_archive.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_deploy.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_analyze.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_optimize.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_manager.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_fomod.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/mod_browser.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic_upload.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle_controller.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle_runtime.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/mobile_ui.m")
target_include_directories(skyrim_ios_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}"
    "${SKYRIM_IOS_BINDING_INCLUDE_DIR}"
    PRIVATE "${SKYDREAM_SOURCE_DIR}/runtime")
target_compile_definitions(skyrim_ios_runtime PRIVATE GAME_SKYRIM DXVK_NATIVE
    SKYRIM_IOS=1 SKYDREAM_MOBILE=1 SKYRIM_IOS_GUEST_DIAGNOSTIC=1
    SDL_MAIN_HANDLED _FILE_OFFSET_BITS=64)
set(cosave_source "${SKYRIM_IOS_ROOT}/experiments/skse-cosave")
target_sources(skyrim_ios_runtime PRIVATE "${cosave_source}/copy_pair.c")
target_include_directories(skyrim_ios_runtime PRIVATE "${cosave_source}")
if(SKYRIM_IOS_CLASSIC_SKSE)
    set(skse_vm_source "${SKYRIM_IOS_ROOT}/experiments/skse-plugin-vm")
    target_sources(skyrim_ios_runtime PRIVATE
        "${cosave_source}/inspect.c" "${cosave_source}/records.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/mod_requirements.c" "${CMAKE_CURRENT_SOURCE_DIR}/mod_features.c" "${CMAKE_CURRENT_SOURCE_DIR}/mod_fuz.c"
        "${skse_vm_source}/classic_pex.c")
    target_include_directories(skyrim_ios_runtime PRIVATE "${skse_vm_source}")
    target_compile_definitions(skyrim_ios_runtime PRIVATE SKYRIM_IOS_CLASSIC_SKSE=1 SKYDREAM_SKSE=1)
    if(SKYRIM_IOS_MOD_HOOKS)
        target_sources(skyrim_ios_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/classic_vm_epoch.c"
            "${SKYRIM_IOS_ROOT}/experiments/classic-mod-events/mod_event.c")
        target_include_directories(skyrim_ios_runtime PRIVATE "${SKYRIM_IOS_ROOT}/experiments/classic-mod-events")
        target_include_directories(skyrim_ios_runtime PRIVATE "${SKYRIM_IOS_MOD_HOOK_SEAMS}")
        target_compile_definitions(skyrim_ios_runtime PRIVATE SKYRIM_IOS_MOD_HOOKS=1)
    endif()
endif()
if(SKYRIM_IOS_SKSE_DLL_PROBE)
    set(dll_probe_source "${SKYRIM_IOS_ROOT}/experiments/skse-plugin-vm")
    target_sources(skyrim_ios_runtime PRIVATE
        "${dll_probe_source}/pe_image.c"
        "${dll_probe_source}/x86_call.c"
        "${dll_probe_source}/x86_heap.c"
        "${dll_probe_source}/x86_winthread.c"
        "${dll_probe_source}/x86_wintext.c"
        "${dll_probe_source}/x86_winenv.c"
        "${dll_probe_source}/papyrus_trial.c"
        "${dll_probe_source}/x86_winfile.c"
        "${dll_probe_source}/x86_winsync.c"
        "${dll_probe_source}/x86_skse_records.c"
        "${dll_probe_source}/probe_asset_file.c"
        "${dll_probe_source}/dependency_plan.c"
        "${dll_probe_source}/plugin_trial.c"
        "${dll_probe_source}/probe_directory.c"
        "${dll_probe_source}/probe_local_file.c"
        "${dll_probe_source}/mod_dll_probe.m")
    target_include_directories(skyrim_ios_runtime PRIVATE "${dll_probe_source}")
    target_compile_definitions(skyrim_ios_runtime PRIVATE SKYRIM_IOS_SKSE_DLL_PROBE=1)
    if(SKYRIM_IOS_CLASSIC_SKSE)
        target_sources(skyrim_ios_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mod_papyrus_inputs.c")
    endif()
endif()
if(SKYRIM_IOS_MOD_HOOKS)
    # App half of the Fuz hooks. The reviewed experiment sources compile unchanged;
    # their funcs_exe.h and lift_prelude.h come from fuz-runtime-abi (rt_call and
    # the runtime's x86.h), so this library keeps its own include order.
    set(fuz_seams "${SKYRIM_IOS_MOD_HOOK_SEAMS}")
    set(fuz_host_sources "${CMAKE_CURRENT_SOURCE_DIR}/mod_fuz_host.c"
        "${fuz_seams}/voice_fallback.c" "${fuz_seams}/lift_voice_seam.c" "${fuz_seams}/lift_menu_seam.c"
        "${fuz_seams}/lift_world_seam.c" "${fuz_seams}/lift_resource_lookup.c")
    if(NOT SKYRIM_IOS_SKSE_DLL_PROBE)
        list(APPEND fuz_host_sources "${SKYRIM_IOS_ROOT}/experiments/skse-plugin-vm/x86_winfile.c")
    endif()
    add_library(skyrim_ios_fuz_host OBJECT ${fuz_host_sources})
    target_include_directories(skyrim_ios_fuz_host BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fuz-runtime-abi"
        "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${SKYRIM_IOS_BINDING_INCLUDE_DIR}"
        "${fuz_seams}" "${SKYRIM_IOS_ROOT}/experiments/skse-plugin-vm")
    target_compile_definitions(skyrim_ios_fuz_host PRIVATE GAME_SKYRIM SKYRIM_IOS=1 SKYDREAM_MOBILE=1
        SKYRIM_IOS_GUEST_DIAGNOSTIC=1 SKYRIM_IOS_CLASSIC_SKSE=1 SKYDREAM_SKSE=1 SKYRIM_IOS_MOD_HOOKS=1)
    target_compile_options(skyrim_ios_fuz_host PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
        -include "${CMAKE_CURRENT_SOURCE_DIR}/ios_build_contract.h")
    target_sources(skyrim_ios_runtime PRIVATE $<TARGET_OBJECTS:skyrim_ios_fuz_host>
        "${CMAKE_CURRENT_SOURCE_DIR}/mod_hook_selection.c")
endif()
if(SKYRIM_IOS_CLASSIC_PAPYRUSUTIL)
    # The staged value publication (classic_papyrus_values.c.in) is compiled into skse_compat.c.
    target_sources(skyrim_ios_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/classic_papyrusutil.c")
    target_compile_definitions(skyrim_ios_runtime PRIVATE SKYRIM_IOS_CLASSIC_INT0_PROBE=1 SKYRIM_IOS_CLASSIC_PAPYRUSUTIL=1)
endif()
if(SKYRIM_IOS_CLASSIC_SETTINGS_PROBE OR SKYRIM_IOS_CLASSIC_PAPYRUSUTIL)
    set(profile_json_source "${SKYRIM_IOS_ROOT}/experiments/skse-profile-json")
    target_sources(skyrim_ios_runtime PRIVATE
        "${dll_probe_source}/classic_scalar.c"
        "${profile_json_source}/profile_json.c" "${profile_json_source}/profile_overlay.c")
    target_compile_definitions(skyrim_ios_runtime PRIVATE SKYRIM_CLASSIC_SETTINGS_BACKEND=1)
endif()
target_compile_options(skyrim_ios_runtime PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
    "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
    -include "${CMAKE_CURRENT_SOURCE_DIR}/ios_build_contract.h")
target_link_libraries(skyrim_ios_runtime PUBLIC skyrim_ios_dependencies)
# MOD import: zlib for zip, UTType for the document picker. Texture codecs
# are pinned header-only checkouts (scripts/fetch-mod-codecs.sh).
set(SKYRIM_MOD_CODEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor" CACHE PATH "bcdec and stb checkouts")
if(NOT EXISTS "${SKYRIM_MOD_CODEC_DIR}/bcdec/bcdec.h" OR NOT EXISTS "${SKYRIM_MOD_CODEC_DIR}/stb/stb_dxt.h")
    message(FATAL_ERROR "MOD texture codecs missing; run scripts/fetch-mod-codecs.sh")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/mod_optimize.c" PROPERTIES
    INCLUDE_DIRECTORIES "${SKYRIM_MOD_CODEC_DIR}/bcdec;${SKYRIM_MOD_CODEC_DIR}/stb")
target_link_libraries(skyrim_ios_runtime PUBLIC z "-framework UniformTypeIdentifiers" "-framework WebKit")
# 7z / rar: libarchive (+ liblzma for 7z) from the prepared dependency prefix.
# Without them the app still builds; 7z/rar imports then show a message.
set(SKYRIM_MOD_ARCHIVE_PREFIX "${SKYRIM_IOS_DEPS_PREFIX}" CACHE PATH "Optional static MOD archive libraries")
if(EXISTS "${SKYRIM_MOD_ARCHIVE_PREFIX}/lib/libarchive.a"
    AND EXISTS "${SKYRIM_MOD_ARCHIVE_PREFIX}/lib/liblzma.a"
    AND EXISTS "${SKYRIM_MOD_ARCHIVE_PREFIX}/include/archive.h"
    AND EXISTS "${SKYRIM_MOD_ARCHIVE_PREFIX}/include/archive_entry.h"
    AND EXISTS "${SKYRIM_MOD_ARCHIVE_PREFIX}/share/skyrim-ios/ModArchiveLicenses.txt")
    target_sources(skyrim_ios_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mod_libarchive.c")
    set_property(SOURCE mod_manager.m APPEND PROPERTY COMPILE_DEFINITIONS SKYRIM_HAVE_LIBARCHIVE=1)
    set_property(SOURCE mod_libarchive.c APPEND PROPERTY INCLUDE_DIRECTORIES "${SKYRIM_MOD_ARCHIVE_PREFIX}/include")
    target_link_libraries(skyrim_ios_runtime PUBLIC "${SKYRIM_MOD_ARCHIVE_PREFIX}/lib/libarchive.a"
        "${SKYRIM_MOD_ARCHIVE_PREFIX}/lib/liblzma.a" z iconv)
    set(_mod_archive_notice "${SKYRIM_MOD_ARCHIVE_PREFIX}/share/skyrim-ios/ModArchiveLicenses.txt")
else()
    message(STATUS "Optional MOD archive dependency prefix incomplete: 7z/rar import disabled")
endif()

if(SKYRIM_IOS_BUILD_APP)
    if(NOT SKYRIM_IOS_BINDING_SOURCES)
        message(FATAL_ERROR "App linking requires real public guest-binding sources")
    endif()
    if(NOT TARGET SDL2::SDL2main)
        message(FATAL_ERROR "SDL2's UIKit main target SDL2::SDL2main is required")
    endif()
    add_executable(SkyrimIOS MACOSX_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/ios_entry.m"
        ${SKYRIM_IOS_BINDING_SOURCES})
    if(SKYRIM_IOS_CLASSIC_SETTINGS_PROBE)
        target_compile_definitions(SkyrimIOS PRIVATE SKYRIM_IOS_CLASSIC_SETTINGS_PROBE=1 SKYRIM_IOS_SKSE_DLL_PROBE=1)
        target_include_directories(SkyrimIOS PRIVATE "${dll_probe_source}")
    endif()
    if(SKYRIM_IOS_CLASSIC_PAPYRUSUTIL)
        target_compile_definitions(SkyrimIOS PRIVATE SKYRIM_IOS_CLASSIC_PAPYRUSUTIL=1 SKYRIM_IOS_SKSE_DLL_PROBE=1
            SKYRIM_CLASSIC_SETTINGS_BACKEND=1)
        target_include_directories(SkyrimIOS PRIVATE "${dll_probe_source}" "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}")
    endif()
    set(host_plist "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
    if(SKYRIM_IOS_CLASSIC_SKSE)
        target_compile_definitions(SkyrimIOS PRIVATE SKYRIM_IOS_CLASSIC_SKSE=1)
        target_sources(SkyrimIOS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../private-abi/pc_guest_skse_host.c")
        # This existing marker identifies the guest callback ABI independently of the product ID.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${host_plist}")
        file(READ "${host_plist}" ordinary_plist)
        string(REPLACE "</dict></plist>" "  <key>PcGuestClassicSkseExperiment</key><true/>\n</dict></plist>"
            classic_plist "${ordinary_plist}")
        if(classic_plist STREQUAL ordinary_plist)
            message(FATAL_ERROR "Unable to mark the Classic guest ABI in the host Info.plist")
        endif()
        set(host_plist "${CMAKE_CURRENT_BINARY_DIR}/ClassicGuestInfo.plist.in")
        file(CONFIGURE OUTPUT "${host_plist}" CONTENT "${classic_plist}" @ONLY)
    endif()
    if(SKYRIM_IOS_MOD_HOOKS)
        target_compile_definitions(SkyrimIOS PRIVATE SKYRIM_IOS_MOD_HOOKS=1)
        target_include_directories(SkyrimIOS PRIVATE "${SKYRIM_IOS_MOD_HOOK_SEAMS}")
        target_sources(SkyrimIOS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../private-abi/pc_guest_mod_hooks_host.c")
        # The host accepts guests with or without the interface; this records its version.
        file(READ "${host_plist}" classic_plist)
        string(REPLACE "</dict></plist>" "  <key>PcGuestModHooksABI</key><integer>1</integer>\n</dict></plist>"
            hooks_plist "${classic_plist}")
        if(hooks_plist STREQUAL classic_plist)
            message(FATAL_ERROR "Unable to mark the MOD hook ABI in the host Info.plist")
        endif()
        set(host_plist "${CMAKE_CURRENT_BINARY_DIR}/ModHooksGuestInfo.plist.in")
        file(CONFIGURE OUTPUT "${host_plist}" CONTENT "${hooks_plist}" @ONLY)
    endif()
    if(_mod_archive_notice)
        target_sources(SkyrimIOS PRIVATE "${_mod_archive_notice}")
        set_source_files_properties("${_mod_archive_notice}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
    target_include_directories(SkyrimIOS PRIVATE "${SKYRIM_IOS_BINDING_INCLUDE_DIR}")
    # On-device translation for MOD installers (mod_translate.swift). The
    # Makefile generator cannot build Swift, so swiftc compiles one object that
    # is linked straight into the app (a static-library member would be dropped:
    # Objective-C finds the class only by name). OFF: the installer falls back
    # to its built-in glossary.
    option(SKYRIM_IOS_TRANSLATION "Build on-device installer translation (Swift)" ON)
    if(SKYRIM_IOS_TRANSLATION)
        if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 18.0)
            message(FATAL_ERROR "On-device MOD translation requires an iPhoneOS deployment target of 18.0 or newer")
        endif()
        execute_process(COMMAND xcrun --sdk iphoneos --show-sdk-path
            OUTPUT_VARIABLE _swift_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND xcrun --sdk iphoneos --find swiftc
            OUTPUT_VARIABLE _swiftc OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        get_filename_component(_swift_bin "${_swiftc}" DIRECTORY)
        set(_translate_src "${CMAKE_CURRENT_SOURCE_DIR}/mod_translate.swift")
        set(_translate_obj "${CMAKE_CURRENT_BINARY_DIR}/mod_translate.o")
        add_custom_command(OUTPUT "${_translate_obj}"
            COMMAND "${_swiftc}" -sdk "${_swift_sdk}" -target "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}"
                    -swift-version 6 -strict-concurrency=complete -parse-as-library -O -module-name SkyrimTranslate
                    -c "${_translate_src}" -o "${_translate_obj}"
            DEPENDS "${_translate_src}" VERBATIM)
        set_source_files_properties("${_translate_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        target_sources(SkyrimIOS PRIVATE "${_translate_obj}")
        target_link_libraries(SkyrimIOS PRIVATE "-framework SwiftUI" "-framework Translation")
        # Require the very class that Objective-C looks up dynamically, even with dead stripping.
        target_link_options(SkyrimIOS PRIVATE "-L${_swift_sdk}/usr/lib/swift"
            "-L${_swift_bin}/../lib/swift/iphoneos" "-Wl,-rpath,/usr/lib/swift"
            "LINKER:-u,_OBJC_CLASS_$_SKTranslator")
    endif()
    target_compile_options(SkyrimIOS PRIVATE "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
    target_link_libraries(SkyrimIOS PRIVATE SDL2::SDL2main skyrim_ios_runtime skyrim_ios_dependencies)
    set_target_properties(SkyrimIOS PROPERTIES LINKER_LANGUAGE CXX
        BUILD_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH "@executable_path/Frameworks"
        MACOSX_BUNDLE_INFO_PLIST "${host_plist}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SKYRIM_IOS_BUNDLE_IDENTIFIER}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
endif()

install(TARGETS skyrim_ios_runtime ARCHIVE DESTINATION lib)
