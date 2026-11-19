# Link only into the separate Classic Lab app. The translation comes from a
# single SHA-256-verified Windows DLL built from the project's own fixture.
function(skyrim_add_classic_plugin_probe destination runtime_headers)
    set(probe "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(port "${probe}/../../../ios/runtime-port")
    add_library(skyrim_classic_serial_probe OBJECT
        "${probe}/serial_fixture_aot.c"
        "${probe}/../serialization_translated.c"
        "${probe}/../serialization_session.c"
        "${probe}/../serialization_files.c"
        "${port}/classic_plugin_probe.c")
    target_include_directories(skyrim_classic_serial_probe PRIVATE "${probe}" "${probe}/.." "${port}" "${runtime_headers}")
    target_compile_definitions(skyrim_classic_serial_probe PRIVATE SKYRIM_CLASSIC_EMBEDDED_FIXTURE=1 LB_STRICT_ORDER=1)
    target_compile_options(skyrim_classic_serial_probe PRIVATE -include "${probe}/fixture_scope.h"
        -fno-strict-aliasing -fwrapv)
    set_target_properties(skyrim_classic_serial_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES
        C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE YES)
    target_sources("${destination}" PRIVATE $<TARGET_OBJECTS:skyrim_classic_serial_probe>)
endfunction()
