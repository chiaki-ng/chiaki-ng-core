foreach(libchiaking_curl_root IN ITEMS "${CURL_ROOT}" "$ENV{CURL_ROOT}")
    if(libchiaking_curl_root AND IS_DIRECTORY "${libchiaking_curl_root}/lib/pkgconfig")
        if(CMAKE_HOST_WIN32)
            set(ENV{PKG_CONFIG_PATH} "${libchiaking_curl_root}/lib/pkgconfig;$ENV{PKG_CONFIG_PATH}")
        else()
            set(ENV{PKG_CONFIG_PATH} "${libchiaking_curl_root}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        endif()
        break()
    endif()
endforeach()

if(NOT DEFINED LIBCHIAKING_CURL)
    find_package(CURL 7.86 QUIET COMPONENTS HTTP HTTPS WS WSS)
    set(libchiaking_curl_default ${CURL_FOUND})
endif()
option(LIBCHIAKING_CURL
    "Build the curl HTTP and WebSocket backends (default: ON when curl with WS/WSS is found)"
    ${libchiaking_curl_default})

if(LIBCHIAKING_CURL)
    find_package(CURL 7.86 REQUIRED COMPONENTS HTTP HTTPS WS WSS)

    add_library(libchiaking_curl STATIC
        src/platform/curl/transfer.cpp
        src/platform/http/curl/curl_transfer.cpp
        src/platform/http/curl/multi_provider.cpp
        src/platform/http/curl/provider.cpp
        src/platform/http/curl/selected_provider.cpp
        src/platform/http/curl/transport.cpp
        src/platform/http/curl/transport_log.cpp
        src/platform/websocket/curl/provider.cpp)
    target_include_directories(libchiaking_curl
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(libchiaking_curl
        PUBLIC ${LIBCHIAKING_PLATFORM_CURL_DEFINITIONS})
    target_link_libraries(libchiaking_curl
        PRIVATE CURL::libcurl
        PUBLIC libchiaking)
    if(LIBCHIAKING_CRYPTO_OPENSSL)
        target_sources(libchiaking_curl PRIVATE src/platform/websocket/curl/aia_chain.cpp)
        target_compile_definitions(libchiaking_curl PRIVATE LIBCHIAKING_CURL_AIA=1)
        target_link_libraries(libchiaking_curl PRIVATE OpenSSL::Crypto)
        target_sources(libchiaking_curl PRIVATE "${LIBCHIAKING_PLATFORM_AIA_ROOTS_SOURCE}")
        target_link_libraries(libchiaking_curl PRIVATE ${LIBCHIAKING_PLATFORM_AIA_ROOTS_LIBRARIES})
    else()
        target_compile_definitions(libchiaking_curl PRIVATE LIBCHIAKING_CURL_AIA=0)
    endif()
    libchiaking_enable_strict_warnings(libchiaking_curl)
endif()
