option(LIBCHIAKING_VENDORED_GTEST "Build the tests against the vendored GoogleTest even when one is installed" OFF)
option(LIBCHIAKING_TESTS_PER_BINARY "Run each test binary as one ctest test instead of one process per test" OFF)

if(NOT LIBCHIAKING_VENDORED_GTEST)
    find_package(GTest QUIET)
endif()

if(TARGET GTest::gtest AND TARGET GTest::gtest_main)
    message(STATUS "GoogleTest: installed ${GTest_VERSION}")
else()
    find_package(Threads REQUIRED)
    set(libchiaking_gtest_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/lib/googletest")
    add_library(libchiaking_gtest STATIC "${libchiaking_gtest_dir}/src/gtest-all.cc")
    target_include_directories(libchiaking_gtest SYSTEM PUBLIC "${libchiaking_gtest_dir}/include")
    target_include_directories(libchiaking_gtest PRIVATE "${libchiaking_gtest_dir}")
    target_link_libraries(libchiaking_gtest PUBLIC Threads::Threads)
    add_library(libchiaking_gtest_main STATIC "${libchiaking_gtest_dir}/src/gtest_main.cc")
    target_link_libraries(libchiaking_gtest_main PUBLIC libchiaking_gtest)
    add_library(GTest::gtest ALIAS libchiaking_gtest)
    add_library(GTest::gtest_main ALIAS libchiaking_gtest_main)
    message(STATUS "GoogleTest: vendored 1.17.0 from tests/lib/googletest")
endif()

include(GoogleTest)

option(LIBCHIAKING_TEST_PRECOMPILED_HEADERS "Share one precompiled header across the test binaries on Clang" ON)

function(libchiaking_share_test_headers target)
    if(NOT LIBCHIAKING_TEST_PRECOMPILED_HEADERS
            OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
            OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        return()
    endif()
    get_property(owner GLOBAL PROPERTY libchiaking_test_headers_owner)
    if(owner)
        target_precompile_headers(${target} REUSE_FROM ${owner})
    else()
        target_precompile_headers(${target} PRIVATE
            <gtest/gtest.h> <algorithm> <array> <chrono> <cstddef> <cstdint> <expected> <format>
            <functional> <map> <memory> <optional> <span> <string> <string_view> <utility>
            <variant> <vector>)
        set_property(GLOBAL PROPERTY libchiaking_test_headers_owner ${target})
    endif()
endfunction()

function(libchiaking_gtest_domain folder)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "LIBS;DEFINES;SOURCES;IF;OPTIONAL_SOURCES;OPTIONAL_LIBS;FOLDERS")
    if(arg_IF AND NOT (${arg_IF}))
        return()
    endif()
    if(NOT arg_FOLDERS)
        set(arg_FOLDERS "${folder}")
    endif()
    set(sources)
    foreach(source_folder IN LISTS arg_FOLDERS)
        file(GLOB_RECURSE folder_sources CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/${source_folder}/*_tests.cpp")
        list(APPEND sources ${folder_sources})
    endforeach()
    set(optional_found TRUE)
    foreach(optional_lib IN LISTS arg_OPTIONAL_LIBS)
        if(NOT TARGET ${optional_lib})
            set(optional_found FALSE)
        endif()
    endforeach()
    if(optional_found)
        list(APPEND arg_LIBS ${arg_OPTIONAL_LIBS})
    else()
        foreach(optional_source IN LISTS arg_OPTIONAL_SOURCES)
            list(REMOVE_ITEM sources "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/${folder}/${optional_source}")
        endforeach()
    endif()
    list(APPEND sources ${arg_SOURCES})
    foreach(excluded IN LISTS libchiaking_test_excludes)
        list(FILTER sources EXCLUDE REGEX "${excluded}")
    endforeach()
    if(NOT sources)
        return()
    endif()
    string(REPLACE "/" "_" name "${folder}")
    set(name "${name}_gtests")
    add_executable(${name} ${sources})
    libchiaking_enable_strict_warnings(${name})
    # GCC's flow analysis reports array bounds and restrict overlaps on paths
    # its speculative devirtualisation invents between the real and fake test
    # clocks, inside the standard library; the library itself stays checked.
    target_compile_options(${name} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-restrict>
        $<$<CXX_COMPILER_ID:GNU,AppleClang>:-O0>)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(${name} PRIVATE ${libchiaking_test_fixtures} ${arg_DEFINES})
    target_link_libraries(${name} PRIVATE ${arg_LIBS} libchiaking_test_support GTest::gtest GTest::gtest_main)
    libchiaking_share_test_headers(${name})
    if(LIBCHIAKING_TESTS_PER_BINARY)
        add_test(NAME ${folder}
            COMMAND ${name} --gtest_brief=1 "--gtest_filter=${libchiaking_gtest_filter}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        gtest_discover_tests(${name}
            TEST_PREFIX "${folder}/"
            TEST_FILTER "${libchiaking_gtest_filter}"
            DISCOVERY_MODE PRE_TEST
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endfunction()
