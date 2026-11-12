# Public dependency closure for an iPhoneOS application. No game inputs are read.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(SkyrimIOSDependencies LANGUAGES C CXX)
endif()
string(TOLOWER "${CMAKE_OSX_SYSROOT}" _ios_sdk)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT _ios_sdk MATCHES "iphoneos")
  message(FATAL_ERROR "Dependencies require the Apple iPhoneOS SDK; macOS/simulator are not substitutes")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  message(FATAL_ERROR "Set CMAKE_OSX_ARCHITECTURES=arm64")
endif()
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SKYRIM_IOS_DEPS_PREFIX "${_root}/local/ios-dependencies/prefix" CACHE PATH "Prepared iPhoneOS static dependencies")
set(SKYDREAM_SOURCE_DIR "${_root}/vendor/skydream" CACHE PATH "Public Skydream source checkout")

find_package(SDL2 CONFIG REQUIRED GLOBAL PATHS "${SKYRIM_IOS_DEPS_PREFIX}" NO_DEFAULT_PATH)
if(NOT TARGET SDL2::SDL2-static OR NOT TARGET SDL2::SDL2main)
  message(FATAL_ERROR "The dependency prefix must provide static SDL2 and its UIKit SDL2main entry point")
endif()
find_package(Threads REQUIRED GLOBAL)

function(_skyrim_import_archive target basename)
  set(_path "${SKYRIM_IOS_DEPS_PREFIX}/lib/lib${basename}.a")
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR "Missing iPhoneOS archive: ${_path}; run build-dependencies.sh on macOS")
  endif()
  add_library(${target} STATIC IMPORTED GLOBAL)
  set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${_path}"
    INTERFACE_INCLUDE_DIRECTORIES "${SKYRIM_IOS_DEPS_PREFIX}/include")
endfunction()
_skyrim_import_archive(skyrim_vkd3d_shader vkd3d-shader)
_skyrim_import_archive(skyrim_moltenvk MoltenVK)
target_link_libraries(skyrim_vkd3d_shader INTERFACE m Threads::Threads)
foreach(_lib avformat avcodec swresample swscale avutil)
  _skyrim_import_archive(skyrim_${_lib} ${_lib})
endforeach()
add_library(skyrim_ffmpeg INTERFACE)
target_link_libraries(skyrim_ffmpeg INTERFACE skyrim_avformat skyrim_avcodec
  skyrim_swresample skyrim_swscale skyrim_avutil m Threads::Threads)
target_compile_definitions(skyrim_ffmpeg INTERFACE LB_HAVE_FFMPEG)
# FFmpeg's Darwin configure probes these unconditionally, including with
# --disable-autodetect. Match the static closure recorded in its pkg-config files.
foreach(_framework CoreFoundation CoreMedia CoreVideo CoreAudio)
  target_link_libraries(skyrim_ffmpeg INTERFACE "$<LINK_LIBRARY:FRAMEWORK,${_framework}>")
endforeach()

# Keep the patched public DXVK source list authoritative without editing vendor
# or producing an embedded dylib. Its checked-in SPIR-V headers need no shader tool.
set(_dxvk "${SKYDREAM_SOURCE_DIR}/third_party/dxvk")
file(READ "${_dxvk}/CMakeLists.txt" _dxvk_cmake)
string(REGEX MATCH "add_library\\(dxvk_d3d9 SHARED[^)]*\\)" _declaration "${_dxvk_cmake}")
if(NOT _declaration)
  message(FATAL_ERROR "Upstream DXVK declaration changed; review the static source-list adapter")
endif()
string(REGEX REPLACE "^add_library\\(dxvk_d3d9 SHARED[ \r\n\t]*" "" _sources "${_declaration}")
string(REGEX REPLACE "\\)$" "" _sources "${_sources}")
separate_arguments(_sources UNIX_COMMAND "${_sources}")
list(TRANSFORM _sources PREPEND "${_dxvk}/")
add_library(dxvk_d3d9 STATIC ${_sources})
set_target_properties(dxvk_d3d9 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(dxvk_d3d9 PUBLIC cxx_std_17)
target_include_directories(dxvk_d3d9 PRIVATE "${_dxvk}/generated"
  "${_dxvk}/include" "${_dxvk}/src/util" "${_dxvk}/src/dxvk"
  "${_dxvk}/src/d3d9" "${_dxvk}/src/dxso" "${_dxvk}/src/wsi"
  "${_dxvk}/src/spirv" "${_dxvk}/src/vulkan")
target_include_directories(dxvk_d3d9 PUBLIC "${_dxvk}/include/native/directx"
  "${_dxvk}/include/native/windows" "${_dxvk}/include/native"
  "${SKYRIM_IOS_DEPS_PREFIX}/include")
target_compile_definitions(dxvk_d3d9 PRIVATE NOMINMAX _WIN32_WINNT=0xa00
  DXVK_NATIVE DXVK_WSI_SDL2 _FILE_OFFSET_BITS=64)
target_compile_options(dxvk_d3d9 PRIVATE -O2
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>"
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstring>"
  -Wno-deprecated-declarations -fvisibility=default)
target_link_libraries(dxvk_d3d9 PUBLIC SDL2::SDL2-static Threads::Threads)

add_library(skyrim_ios_dependencies INTERFACE)
target_link_libraries(skyrim_ios_dependencies INTERFACE dxvk_d3d9
  skyrim_ffmpeg skyrim_vkd3d_shader skyrim_moltenvk)
target_include_directories(skyrim_ios_dependencies INTERFACE
  "${SKYRIM_IOS_DEPS_PREFIX}/include/vkd3d")
foreach(_framework Metal Foundation QuartzCore CoreGraphics IOSurface UIKit)
  target_link_libraries(skyrim_moltenvk INTERFACE "$<LINK_LIBRARY:FRAMEWORK,${_framework}>")
endforeach()
target_link_libraries(skyrim_moltenvk INTERFACE c++)
# SDL's UIKit Vulkan backend resolves this symbol via dlsym(RTLD_DEFAULT).
# Pull it from the archive and retain/export it through application dead stripping.
target_link_options(skyrim_ios_dependencies INTERFACE
  "LINKER:-u,_vkGetInstanceProcAddr" "LINKER:-export_dynamic")
