add_executable(core_tests test_core.c fake_hal.c)
target_link_libraries(core_tests PRIVATE np_core)

add_executable(driver_tests test_driver.c)
target_link_libraries(driver_tests PRIVATE el133uf1)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(core_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(driver_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# One ctest entry per case, so a failure names the behaviour that broke.
set(CORE_CASES
    json
    pairing_flow
    emulator_flag
    pairing_expiry
    repair_on_401
    version_change_redraws
    buttons_wrap
    min_refresh_interval
    page_timer
    frame_chunks_and_halves
    frame_errors
    restart_restores
    provisioning
    wifi_failure
    wifi_help_text
    setup_screens
)
foreach(case ${CORE_CASES})
    add_test(NAME core.${case} COMMAND core_tests ${case})
endforeach()
add_test(NAME driver.el133uf1 COMMAND driver_tests)

# Portable pieces of the Linux target (no libcurl, no hardware needed).
if(UNIX)
    set(NP_LINUX_SRC ${CMAKE_SOURCE_DIR}/targets/linux/src)
    add_executable(linux_tests test_linux.c
        ${NP_LINUX_SRC}/config.c
        ${NP_LINUX_SRC}/store.c
        ${NP_LINUX_SRC}/png.c
        ${NP_LINUX_SRC}/netinfo.c
        ${NP_LINUX_SRC}/wifi_file.c
    )
    target_include_directories(linux_tests PRIVATE ${NP_LINUX_SRC} ${CMAKE_SOURCE_DIR}/core ${CMAKE_SOURCE_DIR}/drivers/el133uf1)
    if(APPLE)
        target_compile_definitions(linux_tests PRIVATE _DARWIN_C_SOURCE)
    else()
        target_compile_definitions(linux_tests PRIVATE _GNU_SOURCE)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(linux_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    foreach(case config store png netinfo wifi_file)
        add_test(NAME linux.${case} COMMAND linux_tests ${case})
    endforeach()
endif()
