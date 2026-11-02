cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" libchiaking_version LIMIT_COUNT 1)
if(NOT libchiaking_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION.txt must hold one MAJOR.MINOR.PATCH line, not '${libchiaking_version}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt")

project(libchiaking LANGUAGES C CXX VERSION ${libchiaking_version})

include(GNUInstallDirs)

if(PROJECT_IS_TOP_LEVEL)
    option(BUILD_TESTING "Build the libchiaking tests" ON)
    set(LIBCHIAKING_BUILD_TESTS "${BUILD_TESTING}")
else()
    option(LIBCHIAKING_BUILD_TESTS "Build the libchiaking tests" OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Toolchain.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform/select.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Vendored.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Takion.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Core.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/providers/crypto.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/providers/curl.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/providers/gateway.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/providers/default_platform.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/providers/opus.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ChiakiNg.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Install.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Docs.cmake")

if(LIBCHIAKING_BUILD_TESTS)
    enable_testing()
    if(TARGET docs-lint)
        add_test(NAME docs_lint
            COMMAND "${Python3_EXECUTABLE}" tools/docs/docs.py lint
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(TARGET docs-lint-internal AND LIBCHIAKING_DOCS_LINT_INTERNAL)
        add_test(NAME docs_lint_internal
            COMMAND "${Python3_EXECUTABLE}" tools/docs/docs.py lint-internal
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    include(tests/tests.cmake)
endif()
