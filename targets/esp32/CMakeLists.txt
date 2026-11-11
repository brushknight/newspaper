# ESP-IDF project for the Morning Newspaper display (ESP32-S3).
cmake_minimum_required(VERSION 3.16)

# Firmware version: NP_VERSION from the environment (CI sets it to the release
# tag), else the VERSION file at the repo root.
if(DEFINED ENV{NP_VERSION} AND NOT "$ENV{NP_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{NP_VERSION}")
else()
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" PROJECT_VER LIMIT_COUNT 1)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(newspaper-display)
