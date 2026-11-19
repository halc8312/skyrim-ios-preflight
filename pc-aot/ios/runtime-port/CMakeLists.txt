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
option(SKYRIM_IOS_CLASSIC_SKSE "Build the experimental classic Papyrus interface" OFF)
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
    set(classic_overlay_script "${CMAKE_CURRENT_SOURCE_DIR}/prepare_classic_skse.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${classic_overlay_script}" "${CMAKE_CURRENT_SOURCE_DIR}/classic_skse_translation.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${classic_overlay_script}"
        --source "${SKYDREAM_SOURCE_DIR}" --runtime "${SKYRIM_IOS_RUNTIME_OVERLAY_DIR}"
        RESULT_VARIABLE classic_patch_result)
    if(NOT classic_patch_result EQUAL 0)
        message(FATAL_ERROR "Experimental classic Papyrus runtime overlay preparation failed")
    endif()
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
if(SKYRIM_IOS_CLASSIC_SKSE)
    target_compile_definitions(skyrim_ios_runtime PRIVATE SKYRIM_IOS_CLASSIC_SKSE=1 SKYDREAM_SKSE=1)
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
    set(host_plist "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
    set(host_bundle_identifier "${SKYRIM_IOS_BUNDLE_IDENTIFIER}")
    if(SKYRIM_IOS_CLASSIC_SKSE)
        # iOS assigns the experiment a separate app container, including its saves.
        set(host_bundle_identifier "org.skyrimios.pc2011.classic-lab")
        set_target_properties(SkyrimIOS PROPERTIES XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${host_bundle_identifier}")
        target_compile_definitions(SkyrimIOS PRIVATE SKYRIM_IOS_CLASSIC_SKSE=1)
        target_sources(SkyrimIOS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../private-abi/pc_guest_skse_host.c")
        # Keep the ordinary bundle template and its metadata unchanged. CMake
        # expands the MACOSX_BUNDLE placeholders when generating the app.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${host_plist}")
        file(READ "${host_plist}" normal_plist)
        string(REPLACE "</dict></plist>" "  <key>PcGuestClassicSkseExperiment</key><true/>\n</dict></plist>"
            classic_plist "${normal_plist}")
        if(classic_plist STREQUAL normal_plist)
            message(FATAL_ERROR "Unable to mark the experimental host Info.plist")
        endif()
        string(REPLACE "<key>CFBundleDisplayName</key><string>SkyrimIOS</string>"
            "<key>CFBundleDisplayName</key><string>SkyrimIOS Classic Lab</string>"
            classic_plist "${classic_plist}")
        set(host_plist "${CMAKE_CURRENT_BINARY_DIR}/ClassicGuestInfo.plist.in")
        file(CONFIGURE OUTPUT "${host_plist}" CONTENT "${classic_plist}" @ONLY)
    endif()
    if(_mod_archive_notice)
        target_sources(SkyrimIOS PRIVATE "${_mod_archive_notice}")
        set_source_files_properties("${_mod_archive_notice}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
    target_include_directories(SkyrimIOS PRIVATE "${SKYRIM_IOS_BINDING_INCLUDE_DIR}")
    target_compile_options(SkyrimIOS PRIVATE "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
    target_link_libraries(SkyrimIOS PRIVATE SDL2::SDL2main skyrim_ios_runtime skyrim_ios_dependencies)
    set_target_properties(SkyrimIOS PROPERTIES LINKER_LANGUAGE CXX
        BUILD_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH "@executable_path/Frameworks"
        MACOSX_BUNDLE_INFO_PLIST "${host_plist}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${host_bundle_identifier}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
endif()

install(TARGETS skyrim_ios_runtime ARCHIVE DESTINATION lib)
