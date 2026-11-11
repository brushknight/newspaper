# Host build: the portable core, the panel driver and their unit tests.
# The ESP32 firmware and the browser emulator have their own build files
# in targets/esp32 and targets/wasm.
cmake_minimum_required(VERSION 3.16)
project(newspaper_display C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NP_VERSION LIMIT_COUNT 1)

add_library(np_core STATIC
    core/app.c
    core/json.c
    core/screen.c
    core/fonts/np_fonts.c
    third_party/qrcodegen/qrcodegen.c
)
target_include_directories(np_core PUBLIC core third_party/qrcodegen)

add_library(el133uf1 STATIC drivers/el133uf1/el133uf1.c)
target_include_directories(el133uf1 PUBLIC drivers/el133uf1)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(np_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(el133uf1 PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
