# The Morning Newspaper display as a Linux program: the same core and panel
# driver as the ESP32 firmware, with a HAL over spidev, the GPIO character
# device and libcurl. Builds on Linux (with panel support) and on macOS (in
# no-hardware mode only).
#
#   cmake -S targets/linux -B build-linux && cmake --build build-linux
#   ./build-linux/newspaper --no-hardware --api http://127.0.0.1:8787 --data-dir /tmp/np
#
# Options:
#   -DNP_HARDWARE=OFF   leave out SPI and GPIO (default ON on Linux, OFF elsewhere)
#   -DNP_STATIC=ON      link everything statically (release binaries, see .github/workflows)
#   -DNP_VERSION=X      firmware version string (default: the VERSION file)
cmake_minimum_required(VERSION 3.16)
project(newspaper_linux C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(NP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(NP_ROOT "${NP_ROOT}" ABSOLUTE)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NP_HARDWARE_DEFAULT ON)
else()
    set(NP_HARDWARE_DEFAULT OFF)
endif()
option(NP_HARDWARE "Panel and buttons over spidev and the GPIO character device (Linux only)" ${NP_HARDWARE_DEFAULT})
option(NP_STATIC "Link statically" OFF)
if(NOT NP_VERSION)
    file(STRINGS "${NP_ROOT}/VERSION" NP_VERSION LIMIT_COUNT 1)
endif()

set(NP_CORE_SOURCES
    ${NP_ROOT}/core/app.c
    ${NP_ROOT}/core/json.c
    ${NP_ROOT}/core/screen.c
    ${NP_ROOT}/core/fonts/np_fonts.c
    ${NP_ROOT}/third_party/qrcodegen/qrcodegen.c
    ${NP_ROOT}/drivers/el133uf1/el133uf1.c
)

add_executable(newspaper
    src/main.c
    src/config.c
    src/store.c
    src/png.c
    src/netinfo.c
    src/net.c
    src/panel_sim.c
    src/buttons.c
    ${NP_CORE_SOURCES}
)
if(NP_HARDWARE)
    target_sources(newspaper PRIVATE src/gpio.c src/panel_spi.c)
    target_compile_definitions(newspaper PRIVATE NP_HARDWARE=1)
endif()
target_include_directories(newspaper PRIVATE src ${NP_ROOT}/core ${NP_ROOT}/third_party/qrcodegen ${NP_ROOT}/drivers/el133uf1)
target_compile_definitions(newspaper PRIVATE NP_FIRMWARE_VERSION="${NP_VERSION}")

add_executable(newspaper-wifi src/newspaper_wifi.c src/wifi_file.c)
target_include_directories(newspaper-wifi PRIVATE src ${NP_ROOT}/core ${NP_ROOT}/drivers/el133uf1)

foreach(t newspaper newspaper-wifi)
    # POSIX and the platform extensions (CLOCK_BOOTTIME, fchmod, gethostname).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${t} PRIVATE _GNU_SOURCE)
    elseif(APPLE)
        target_compile_definitions(${t} PRIVATE _DARWIN_C_SOURCE)
    else()
        target_compile_definitions(${t} PRIVATE _DEFAULT_SOURCE)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

if(NP_STATIC)
    # Release binaries: one file that runs on any Raspberry Pi OS of its architecture.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURL REQUIRED libcurl)
    target_include_directories(newspaper PRIVATE ${CURL_STATIC_INCLUDE_DIRS})
    target_link_libraries(newspaper PRIVATE ${CURL_STATIC_LDFLAGS})
    target_link_options(newspaper PRIVATE -static)
    target_link_options(newspaper-wifi PRIVATE -static)
    target_compile_definitions(newspaper PRIVATE NP_STATIC=1 CURL_STATICLIB)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(newspaper PRIVATE CURL::libcurl)
endif()

install(TARGETS newspaper newspaper-wifi RUNTIME DESTINATION bin)
