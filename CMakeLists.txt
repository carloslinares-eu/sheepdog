cmake_minimum_required(VERSION 3.25)

project(sheepdog
    VERSION 0.0.0
    DESCRIPTION "Catalog, tag and organise files"
    HOMEPAGE_URL "https://github.com/carloslinares/sheepdog"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHEEPDOG_BUILD_GUI "Build the Qt Widgets GUI" ON)
option(SHEEPDOG_BUILD_TESTS "Build the Catch2 tests" ON)
option(SHEEPDOG_BUILD_FUZZ "Build the libFuzzer targets (Clang only)" OFF)
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(SHEEPDOG_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)
include(Version)
include(WindowsResources)

add_subdirectory(src/core)
add_subdirectory(src/cli)
if(SHEEPDOG_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(SHEEPDOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
