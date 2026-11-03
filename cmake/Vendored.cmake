# The supplied single-file boost-ext/sml 1.2.0 dependency is intentionally
# offline and isolated. Its header declares the Boost Software License 1.0.
# The supplied single-header rigtorp/SPSCQueue 1.1 dependency is used unchanged
# under the MIT licence kept beside it. Neither appears in libchiaking's public
# headers.
add_library(libchiaking_vendored_headers INTERFACE)
target_include_directories(libchiaking_vendored_headers SYSTEM
    INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib>")
target_compile_definitions(libchiaking_vendored_headers INTERFACE GLZ_DISABLE_ALWAYS_INLINE)

option(LIBCHIAKING_SYSTEM_SIMDUTF "Use an installed simdutf instead of the vendored copy" OFF)
if(LIBCHIAKING_SYSTEM_SIMDUTF)
    find_package(simdutf CONFIG REQUIRED)
    add_library(libchiaking_simdutf INTERFACE)
    target_link_libraries(libchiaking_simdutf INTERFACE simdutf::simdutf)
else()
    add_library(libchiaking_simdutf STATIC src/lib/simdutf/simdutf.cpp)
    target_include_directories(libchiaking_simdutf SYSTEM
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib/simdutf>")
endif()

add_library(libchiaking_nanopb STATIC src/lib/nanopb/pb_common.c
    src/lib/nanopb/pb_encode.c src/lib/nanopb/pb_decode.c)
target_include_directories(libchiaking_nanopb SYSTEM
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib/nanopb>")
target_compile_definitions(libchiaking_nanopb
    PUBLIC PB_ENABLE_MALLOC=1 PB_ENCODE_ARRAYS_UNPACKED=1 PB_FIELD_32BIT=1
    PB_MESSAGE_NESTING_MAX=8)

add_library(libchiaking_picohttpparser STATIC src/lib/picohttpparser/picohttpparser.c)
target_include_directories(libchiaking_picohttpparser SYSTEM
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib>")

add_library(libchiaking_gf_complete STATIC
    src/lib/gf-complete/src/gf.c
    src/lib/gf-complete/src/gf_cpu.c
    src/lib/gf-complete/src/gf_w4.c
    src/lib/gf-complete/src/gf_w8.c
    src/lib/gf-complete/src/gf_w16.c
    src/lib/gf-complete/src/gf_w32.c
    src/lib/gf-complete/src/gf_w64.c
    src/lib/gf-complete/src/gf_w128.c
    src/lib/gf-complete/src/gf_wgen.c)
target_include_directories(libchiaking_gf_complete SYSTEM
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gf-complete/include>")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"
        AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    target_sources(libchiaking_gf_complete PRIVATE
        src/lib/gf-complete/src/neon/gf_w4_neon.c
        src/lib/gf-complete/src/neon/gf_w8_neon.c
        src/lib/gf-complete/src/neon/gf_w16_neon.c
        src/lib/gf-complete/src/neon/gf_w32_neon.c
        src/lib/gf-complete/src/neon/gf_w64_neon.c)
    target_compile_definitions(libchiaking_gf_complete PRIVATE ARM_NEON ARCH_AARCH64)
endif()

add_library(libchiaking_jerasure STATIC
    src/lib/jerasure/src/jerasure.c
    src/lib/jerasure/src/cauchy.c
    src/lib/jerasure/src/galois.c)
target_include_directories(libchiaking_jerasure SYSTEM
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib/jerasure/include>")
target_link_libraries(libchiaking_jerasure PUBLIC libchiaking_gf_complete)

set_target_properties(libchiaking_nanopb libchiaking_picohttpparser libchiaking_gf_complete libchiaking_jerasure
    PROPERTIES UNITY_BUILD OFF)
