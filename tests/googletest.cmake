option(LIBCHIAKING_VENDORED_GTEST "Build the tests against the vendored GoogleTest even when one is installed" OFF)

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

function(libchiaking_gtest_domain folder)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "LIBS;DEFINES;SOURCES;IF;OPTIONAL_SOURCES;OPTIONAL_LIBS")
    if(arg_IF AND NOT (${arg_IF}))
        return()
    endif()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/${folder}/*_tests.cpp")
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
    string(REPLACE "/" "_" name "${folder}")
    set(name "${name}_gtests")
    add_executable(${name} ${sources})
    libchiaking_enable_strict_warnings(${name})
    # GCC's flow analysis reports array bounds and restrict overlaps on paths
    # its speculative devirtualisation invents between the real and fake test
    # clocks, inside the standard library; the library itself stays checked.
    target_compile_options(${name} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds>
        $<$<CXX_COMPILER_ID:GNU>:-Wno-restrict>)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(${name} PRIVATE ${libchiaking_test_fixtures} ${arg_DEFINES})
    target_link_libraries(${name} PRIVATE ${arg_LIBS} libchiaking_test_support GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name}
        TEST_PREFIX "${folder}/"
        DISCOVERY_MODE PRE_TEST
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
