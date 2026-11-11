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
    setup_screens
)
foreach(case ${CORE_CASES})
    add_test(NAME core.${case} COMMAND core_tests ${case})
endforeach()
add_test(NAME driver.el133uf1 COMMAND driver_tests)
