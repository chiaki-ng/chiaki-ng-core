if(TARGET libchiaking_crypto_openssl)
    set(LIBCHIAKING_DEFAULT_CRYPTO libchiaking_crypto_openssl)
elseif(TARGET libchiaking_crypto_boringssl)
    set(LIBCHIAKING_DEFAULT_CRYPTO libchiaking_crypto_boringssl)
else()
    set(LIBCHIAKING_DEFAULT_CRYPTO "")
endif()

if(LIBCHIAKING_DEFAULT_CRYPTO AND TARGET libchiaking_curl)
    add_library(libchiaking_default_platform STATIC src/assembly/default_platform.cpp)
    target_include_directories(libchiaking_default_platform
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_default_platform
        PUBLIC libchiaking ${LIBCHIAKING_DEFAULT_CRYPTO}
            libchiaking_curl libchiaking_gateway)
    if(LIBCHIAKING_DEFAULT_CRYPTO STREQUAL "libchiaking_crypto_boringssl")
        target_compile_definitions(libchiaking_default_platform PRIVATE LIBCHIAKING_DEFAULT_PLATFORM_BORINGSSL=1)
    elseif(TARGET libchiaking_dtls_openssl)
        target_link_libraries(libchiaking_default_platform PUBLIC libchiaking_dtls_openssl)
        target_compile_definitions(libchiaking_default_platform
            PRIVATE LIBCHIAKING_DEFAULT_PLATFORM_DTLS_OPENSSL=1)
    endif()
    libchiaking_enable_strict_warnings(libchiaking_default_platform)
endif()
