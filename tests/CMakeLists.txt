# Tests support
find_package(GTest REQUIRED)

add_executable(dosbox_tests
    ansi_code_markup_tests.cpp
    batch_file_tests.cpp
    bit_view_tests.cpp
    bitops_tests.cpp
    cmd_move_tests.cpp
    dos_files_tests.cpp
    dos_memory_struct_tests.cpp
    dosbox_test_fixture.h
    drives_tests.cpp
    fraction_tests.cpp
    fs_utils_tests.cpp
    int10_modes_tests.cpp
    language_territory_tests.cpp
    math_utils_tests.cpp
    messages_adjust_tests.cpp
    mixer_tests.cpp
    port_containers_tests.cpp
    program_mixer_tests.cpp
    rect_tests.cpp
    rgb_tests.cpp
    ring_buffer_tests.cpp
    rwqueue_tests.cpp
    shader_pragma_parser_tests.cpp
    shell_banner_tests.cpp
    shell_cmds_tests.cpp
    shell_redirection_tests.cpp
    string_utils_tests.cpp
    # stubs.cpp
    support_tests.cpp
    unicode_tests.cpp
    webserver_auth_tests.cpp
    osd_tests.cpp
    mount_policy_tests.cpp
    lua_engine_tests.cpp
    lua_coroutine_tests.cpp
    lua_api_tests.cpp
    lua_output_budget_tests.cpp
    lua_script_log_tests.cpp
    script_validator_tests.cpp
    text_input_tests.cpp
    webserver_mem_search_tests.cpp
    webserver_mem_scan_tests.cpp
    webserver_memory_snapshot_tests.cpp
    webserver_freeze_tests.cpp
    webserver_dos_map_tests.cpp
    webserver_alloc_tests.cpp
    webserver_batch_tests.cpp
    webserver_input_validation_tests.cpp
    webserver_error_tests.cpp
    webserver_io_port_tests.cpp
    webserver_cpu_write_tests.cpp
    webserver_frame_tap_tests.cpp
    webserver_video_etag_tests.cpp
    webserver_video_frame_params_tests.cpp
    webserver_bridge_tests.cpp
    webserver_wait_tests.cpp
    webserver_capabilities_tests.cpp
    webserver_debug_events_tests.cpp
    webserver_drive_tests.cpp
    mouseif_dos_driver_position_tests.cpp
    fnv_hash_tests.cpp
    multi_prefix_tests.cpp
)

# Disable some warnings for deliberately flawed test cases
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(dosbox_tests PRIVATE
        "-Wno-effc++"
        "-Wno-gnu-zero-variadic-macro-arguments"
    )
    # Clang does not support -Wno-format-overflow
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(dosbox_tests PRIVATE
            "-Wno-format-overflow"
        )
    endif()
endif()

target_link_libraries(dosbox_tests PRIVATE
    GTest::gmock_main
    dosboxcommon
    SDL3::Headers
)

include(GoogleTest)

# `DISCOVERY_MODE PRE_TEST` is needed for local builds on macOS for test
# discovery to work, otherwise the build will just fail with an error.
#
# Probably related to:
# https://gitlab.kitware.com/cmake/cmake/-/issues/21845
#
gtest_discover_tests(
  dosbox_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 60
)
