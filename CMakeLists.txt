# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Force version update from git on every build
execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Fallback if not in git repo
if(NOT PROJECT_VER)
    set(PROJECT_VER "unknown")
endif()

# Force CMake to use our version
set(PROJECT_VER ${PROJECT_VER} CACHE STRING "Project version from git" FORCE)

# Monitor git files to trigger reconfigure when git state changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/.git/index
    ${CMAKE_SOURCE_DIR}/.git/HEAD
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pool-controller)