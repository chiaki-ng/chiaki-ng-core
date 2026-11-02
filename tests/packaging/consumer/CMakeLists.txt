cmake_minimum_required(VERSION 3.24)

project(libchiaking_consumer LANGUAGES CXX)

find_package(libchiaking 0.1 REQUIRED COMPONENTS chiaki_ng)

add_executable(libchiaking_cpp_consumer main.cpp)
target_link_libraries(libchiaking_cpp_consumer PRIVATE libchiaking::chiaki_ng)
target_compile_definitions(libchiaking_cpp_consumer
    PRIVATE LIBCHIAKING_CONSUMER_PACKAGE_VERSION="${libchiaking_VERSION}")
if(libchiaking_curl_FOUND)
    target_link_libraries(libchiaking_cpp_consumer PRIVATE libchiaking::curl)
    target_compile_definitions(libchiaking_cpp_consumer PRIVATE LIBCHIAKING_CONSUMER_HAS_CURL=1)
endif()

if(libchiaking_chiaki_ng_FOUND)
    add_executable(libchiaking_faces_consumer faces.cpp)
    target_link_libraries(libchiaking_faces_consumer PRIVATE libchiaking::chiaki_ng)
    if(libchiaking_kakusan_FOUND)
        target_compile_definitions(libchiaking_faces_consumer PRIVATE LIBCHIAKING_CONSUMER_HAS_KAKUSAN=1)
    endif()
endif()

if(libchiaking_kakusan_FOUND)
    add_executable(libchiaking_kakusan_consumer kakusan.cpp)
    target_link_libraries(libchiaking_kakusan_consumer PRIVATE libchiaking::chiaki_ng)
    if(libchiaking_crypto_openssl_FOUND AND libchiaking_dtls_openssl_FOUND)
        target_link_libraries(libchiaking_kakusan_consumer
            PRIVATE libchiaking::crypto_openssl libchiaking::dtls_openssl)
        target_compile_definitions(libchiaking_kakusan_consumer PRIVATE LIBCHIAKING_CONSUMER_HAS_DTLS=1)
    endif()
endif()
