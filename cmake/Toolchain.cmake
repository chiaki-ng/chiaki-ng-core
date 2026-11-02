set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR
        "libchiaking needs GCC 16 or newer; found GCC ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
#include <expected>
#include <format>
#include <string>
int main () {
    const std::expected<int, std::string> value{ 42 };
    return value && std::format ("{}", *value) == "42" ? 0 : 1;
}
]=] LIBCHIAKING_HAS_CXX23_LIBRARY)
if(NOT LIBCHIAKING_HAS_CXX23_LIBRARY)
    message(FATAL_ERROR
        "libchiaking needs a C++23 standard library with <format> and <expected>. "
        "Use GCC 16 or newer, Clang 17 or newer (Android NDK r26 or newer), "
        "or Visual Studio 2022.")
endif()

function(libchiaking_enable_strict_warnings target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
            OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options("${target}" PRIVATE
            /W4 /permissive- /w14242 /w14254 /w14263 /w14265 /w14287
            /w14296 /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14640
            /w14826 /w14905 /w14906 /w14928 /WX)
    else()
        target_compile_options("${target}" PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Wno-stringop-overflow>
            $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Werror)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options("${target}" PRIVATE -Wno-missing-field-initializers)
        endif()
    endif()
endfunction()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(
        src/core/net/nat/holepunch/holepunch_conn_request.cpp
        src/core/net/transport/rudp/rudp.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-redundant-move")
endif()

function(libchiaking_glob_sources out)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "RECURSE;FLAT;EXCLUDE")
    set(sources)
    foreach(dir IN LISTS arg_RECURSE)
        file(GLOB_RECURSE found CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
        list(APPEND sources ${found})
    endforeach()
    foreach(dir IN LISTS arg_FLAT)
        file(GLOB found CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
        list(APPEND sources ${found})
    endforeach()
    foreach(pattern IN LISTS arg_EXCLUDE)
        list(FILTER sources EXCLUDE REGEX "${pattern}")
    endforeach()
    set("${out}" ${sources} PARENT_SCOPE)
endfunction()
