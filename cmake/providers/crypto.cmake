option(LIBCHIAKING_CRYPTO_BORINGSSL
    "Build the BoringSSL crypto backend instead of OpenSSL" OFF)

if(NOT DEFINED LIBCHIAKING_CRYPTO_OPENSSL)
    set(libchiaking_openssl_default OFF)
    if(NOT LIBCHIAKING_CRYPTO_BORINGSSL)
        find_package(OpenSSL QUIET COMPONENTS Crypto)
        if(OpenSSL_FOUND)
            set(libchiaking_openssl_default ON)
        endif()
    endif()
endif()
option(LIBCHIAKING_CRYPTO_OPENSSL
    "Build the OpenSSL crypto backend (default: ON when OpenSSL is found)"
    ${libchiaking_openssl_default})

if(LIBCHIAKING_CRYPTO_OPENSSL AND LIBCHIAKING_CRYPTO_BORINGSSL)
    message(FATAL_ERROR
        "LIBCHIAKING_CRYPTO_OPENSSL and LIBCHIAKING_CRYPTO_BORINGSSL cannot both be enabled because their symbols collide")
endif()

if(LIBCHIAKING_CRYPTO_OPENSSL)
    find_package(OpenSSL REQUIRED)

    add_library(libchiaking_crypto_openssl STATIC
        src/platform/crypto/evp/primitives.cpp
        src/platform/crypto/openssl/provider.cpp
        src/platform/crypto/openssl/pairing_primitives.cpp)
    target_include_directories(libchiaking_crypto_openssl
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_crypto_openssl
        PUBLIC OpenSSL::Crypto libchiaking)
    libchiaking_enable_strict_warnings(libchiaking_crypto_openssl)

    find_package(OpenSSL QUIET COMPONENTS SSL)
    if(TARGET OpenSSL::SSL AND NOT OPENSSL_VERSION VERSION_LESS 3.2)
        add_library(libchiaking_dtls_openssl STATIC
            src/platform/dtls/openssl/provider.cpp)
        target_include_directories(libchiaking_dtls_openssl
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(libchiaking_dtls_openssl
            PRIVATE OPENSSL_API_COMPAT=30000 OPENSSL_NO_DEPRECATED)
        target_link_libraries(libchiaking_dtls_openssl
            PUBLIC libchiaking
            PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        libchiaking_enable_strict_warnings(libchiaking_dtls_openssl)
    else()
        message(STATUS
            "libchiaking secure datagrams: the OpenSSL binding needs libssl 3.2 or newer and is left out")
    endif()
endif()

if(LIBCHIAKING_CRYPTO_BORINGSSL)
    find_package(OpenSSL CONFIG REQUIRED)
    add_library(libchiaking_crypto_boringssl STATIC
        src/platform/crypto/evp/primitives.cpp
        src/platform/crypto/boringssl/provider.cpp)
    target_include_directories(libchiaking_crypto_boringssl
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_crypto_boringssl
        PUBLIC OpenSSL::Crypto libchiaking)
    libchiaking_enable_strict_warnings(libchiaking_crypto_boringssl)
endif()

option(LIBCHIAKING_CRYPTO_LIBNX
    "Build the libnx crypto backend for Nintendo Switch homebrew" OFF)

set(libchiaking_pmull_flags -march=armv8-a+crypto)
set(libchiaking_pmull_host OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"
        AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("${libchiaking_pmull_flags}" LIBCHIAKING_HAVE_PMULL_FLAGS)
    set(libchiaking_pmull_host ${LIBCHIAKING_HAVE_PMULL_FLAGS})
endif()

if(LIBCHIAKING_CRYPTO_LIBNX AND NOT libchiaking_pmull_host)
    message(FATAL_ERROR
        "LIBCHIAKING_CRYPTO_LIBNX needs an AArch64 compiler that accepts ${libchiaking_pmull_flags}")
endif()

if(LIBCHIAKING_CRYPTO_LIBNX OR (libchiaking_pmull_host AND LIBCHIAKING_BUILD_TESTS AND LIBCHIAKING_CRYPTO_OPENSSL))
    add_library(libchiaking_microecc STATIC
        src/lib/microecc/uECC.c
        src/lib/microecc/uECC_p521.c)
    target_include_directories(libchiaking_microecc SYSTEM
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lib>")
    set_source_files_properties(src/lib/microecc/uECC.c
        PROPERTIES COMPILE_DEFINITIONS uECC_SUPPORTS_secp256r1=1)
    target_compile_definitions(libchiaking_microecc INTERFACE uECC_SUPPORTS_secp256r1=1)

    add_library(libchiaking_crypto_armv8 STATIC
        src/platform/crypto/armv8/gmac.cpp)
    target_compile_options(libchiaking_crypto_armv8 PRIVATE ${libchiaking_pmull_flags})
    target_include_directories(libchiaking_crypto_armv8
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_crypto_armv8 PUBLIC libchiaking)
    libchiaking_enable_strict_warnings(libchiaking_crypto_armv8)

    add_library(libchiaking_crypto_micro_ecc STATIC
        src/platform/crypto/micro_ecc/key_pair.cpp)
    target_include_directories(libchiaking_crypto_micro_ecc
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_crypto_micro_ecc
        PUBLIC libchiaking
        PRIVATE libchiaking_microecc)
    libchiaking_enable_strict_warnings(libchiaking_crypto_micro_ecc)
endif()

if(LIBCHIAKING_CRYPTO_LIBNX)
    add_library(libchiaking_crypto_libnx STATIC
        src/platform/crypto/libnx/provider.cpp)
    target_include_directories(libchiaking_crypto_libnx
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_crypto_libnx
        PUBLIC libchiaking
        PRIVATE libchiaking_crypto_armv8 libchiaking_crypto_micro_ecc)
    libchiaking_enable_strict_warnings(libchiaking_crypto_libnx)
endif()
