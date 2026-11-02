set(libchiaking_test_fixtures
    LIBCHIAKING_PSN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/psn"
    LIBCHIAKING_CONSOLE_REPLY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/console_replies"
    LIBCHIAKING_PLAY_TRANSCRIPTS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/play/transcripts"
    LIBCHIAKING_GRAPHQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/core/psn/graphql"
    LIBCHIAKING_JSON_STRICT_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/json/strict_corpus.txt"
    LIBCHIAKING_BASE64URL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/kakusan/draft-05"
    LIBCHIAKING_WIRE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/kakusan/draft-05")

add_library(libchiaking_test_support INTERFACE)
target_include_directories(libchiaking_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libchiaking_test_support INTERFACE libchiaking_vendored_headers)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(libchiaking_test_support INTERFACE /UNDEBUG)
else()
    target_compile_options(libchiaking_test_support INTERFACE -UNDEBUG)
endif()

add_library(libchiaking_test_crypto STATIC tests/support/crypto/software_crypto.cpp)
target_include_directories(libchiaking_test_crypto PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libchiaking_test_crypto PUBLIC libchiaking)
libchiaking_enable_strict_warnings(libchiaking_test_crypto)
if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(libchiaking_test_crypto PRIVATE -O2)
endif()


if(LIBCHIAKING_KAKUSAN)
    set(libchiaking_tests_have_kakusan 1)
else()
    set(libchiaking_tests_have_kakusan 0)
endif()

set(libchiaking_unity_excluded_sources
    src/core/play/adapters/links/internet/internet_link.cpp
    src/core/play/adapters/links/internet/internet_pairer.cpp
    src/core/play/adapters/links/internet/internet_start.cpp
    src/platform/memory/secure_zero_darwin.cpp
    tests/unit/core/catalog/model/launch_verdict_tests.cpp
    tests/unit/core/catalog/usecases/played_games_tests.cpp
    tests/unit/core/catalog/usecases/show_details/concept_details_tests.cpp
    tests/unit/core/catalog/usecases/show_details/game_images_tests.cpp
    tests/unit/core/catalog/usecases/show_details/listed_game_details_tests.cpp
    tests/unit/core/catalog/usecases/show_details/product_details_tests.cpp
    tests/unit/core/cloud/links/allocated_endpoint_tests.cpp
    tests/unit/core/cloud/links/calibration_tests.cpp
    tests/unit/core/cloud/links/handshake_key_tests.cpp
    tests/unit/core/cloud/links/ping_target_tests.cpp
    tests/unit/core/cloud/links/server_connect_tests.cpp
    tests/unit/core/cloud/links/server_route_tests.cpp
    tests/unit/core/cloud/links/services/old_service_launch_tests.cpp
    tests/unit/core/cloud/links/services/old_service_session_tests.cpp
    tests/unit/core/consoles/adapters/titles/title_start_cancel_tests.cpp
    tests/unit/core/consoles/usecases/pair_at_home_deadline_tests.cpp
    tests/unit/core/consoles/usecases/pair_at_home_progress_tests.cpp
    tests/unit/core/consoles/usecases/pair_at_home_tests.cpp
    tests/unit/core/consoles/usecases/pair_over_the_internet_tests.cpp
    tests/unit/core/kakusan/pair_cloud_session_tests.cpp
    tests/unit/core/kakusan/pair_devices_tests.cpp
    tests/unit/core/kakusan/pair_real_time_tests.cpp
    tests/unit/core/kakusan/pair_seat_reservation_tests.cpp
    tests/unit/core/kakusan/pair_visit_endings_tests.cpp
    tests/unit/core/kakusan/pair_visit_tests.cpp
    tests/unit/core/play/adapters/cloud_launch/launch_waits_tests.cpp
    tests/unit/core/play/adapters/links/internet/internet_route_progress_tests.cpp
    tests/unit/core/play/adapters/links/internet/wire/holepunch_offer_drive_tests.cpp
    tests/unit/core/play/adapters/links/internet/wire/holepunch_offer_inbox_tests.cpp
    tests/unit/core/play/adapters/links/internet/wire/holepunch_offer_psn_tests.cpp
    tests/unit/core/play/adapters/links/internet/wire/holepunch_offer_wait_tests.cpp
    tests/unit/core/play/adapters/links/internet/wire/remote_ctrl_inbox_tests.cpp
    tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_receiver_tests.cpp
    tests/unit/core/play/adapters/links/reconnecting_link_retry_tests.cpp
    tests/unit/core/play/adapters/links/reconnecting_link_start_tests.cpp
    tests/unit/core/play/adapters/links/reconnecting_link_stop_tests.cpp
    tests/unit/core/play/adapters/media/haptic_block_tests.cpp
    tests/unit/core/play/model/start_refusal_tests.cpp
    tests/unit/core/utils/json/strict_json_object_reader_tests.cpp
    tests/unit/core/utils/json/strict_json_tests.cpp
)
set_source_files_properties(${libchiaking_unity_excluded_sources} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/tests/googletest.cmake")

libchiaking_gtest_domain(core/trophies LIBS libchiaking)
libchiaking_gtest_domain(core/utils/concurrency LIBS libchiaking)
libchiaking_gtest_domain(core/utils/containers LIBS libchiaking)
libchiaking_gtest_domain(core/utils/timing LIBS libchiaking)
libchiaking_gtest_domain(core/utils/numeric LIBS libchiaking)
libchiaking_gtest_domain(core/utils/memory LIBS libchiaking)
libchiaking_gtest_domain(core/messages LIBS libchiaking)
libchiaking_gtest_domain(platform/gateway LIBS libchiaking
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/gateway/miniupnpc.cpp"
    IF LIBCHIAKING_MINIUPNPC AND LIBCHIAKING_MINIUPNPC_INCLUDE_DIR AND LIBCHIAKING_MINIUPNPC_LIBRARY)
libchiaking_gtest_domain(core/utils/bytes LIBS libchiaking)
libchiaking_gtest_domain(core/utils/text LIBS libchiaking)
libchiaking_gtest_domain(core/utils/json LIBS libchiaking)
libchiaking_gtest_domain(core/utils/diagnostics LIBS libchiaking)
libchiaking_gtest_domain(core/users LIBS libchiaking)
libchiaking_gtest_domain(core/psn
    LIBS libchiaking
    OPTIONAL_SOURCES graphql/query_documents_tests.cpp
    OPTIONAL_LIBS libchiaking_crypto_openssl)
libchiaking_gtest_domain(core/catalog LIBS libchiaking libchiaking_vendored_headers)
libchiaking_gtest_domain(core/cloud LIBS libchiaking)
libchiaking_gtest_domain(core/consoles LIBS libchiaking libchiaking_vendored_headers)
libchiaking_gtest_domain(core/net/kakusan
    LIBS libchiaking libchiaking_test_crypto
    OPTIONAL_SOURCES software_crypto_openssl_tests.cpp udp_transport_openssl_tests.cpp
        datagram_security_tests.cpp datagram_security_profile_tests.cpp
        datagram_security_staging_tests.cpp
    OPTIONAL_LIBS libchiaking_crypto_openssl libchiaking_dtls_openssl OpenSSL::SSL OpenSSL::Crypto
    IF LIBCHIAKING_KAKUSAN)
libchiaking_gtest_domain(core/kakusan
    LIBS libchiaking
        libchiaking_test_crypto
    OPTIONAL_SOURCES network_end_to_end_openssl_tests.cpp
    OPTIONAL_LIBS libchiaking_dtls_openssl
    IF LIBCHIAKING_KAKUSAN)

if(LIBCHIAKING_KAKUSAN)
    add_executable(net_kakusan_real_socket_smoke tests/unit/core/net/kakusan/real_socket_smoke.cpp)
    libchiaking_enable_strict_warnings(net_kakusan_real_socket_smoke)
    target_include_directories(net_kakusan_real_socket_smoke
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(net_kakusan_real_socket_smoke PRIVATE ${libchiaking_test_fixtures})
    target_link_libraries(net_kakusan_real_socket_smoke
        PRIVATE libchiaking libchiaking_test_crypto libchiaking_test_support GTest::gtest GTest::gtest_main)
    add_test(NAME net_kakusan_real_socket_smoke COMMAND net_kakusan_real_socket_smoke)
    set_tests_properties(net_kakusan_real_socket_smoke
        PROPERTIES LABELS network SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
endif()
libchiaking_gtest_domain(core/net/protocol/takion LIBS libchiaking
    OPTIONAL_SOURCES platform_crypto_provider_tests.cpp
    OPTIONAL_LIBS libchiaking_crypto_openssl)
libchiaking_gtest_domain(core/net/protocol/remote_play
    LIBS libchiaking libchiaking_vendored_headers libchiaking_jerasure
    OPTIONAL_SOURCES ecdh_wire_openssl_tests.cpp pairing/cipher_openssl_tests.cpp
        pairing/request_openssl_tests.cpp stream/forwarded_source_tests.cpp stream/launch_spec_openssl_tests.cpp
    OPTIONAL_LIBS libchiaking_crypto_openssl)
libchiaking_gtest_domain(core/net/protocol/senkusha LIBS libchiaking)
libchiaking_gtest_domain(api/catalog LIBS libchiaking)
libchiaking_gtest_domain(api/network LIBS libchiaking)
libchiaking_gtest_domain(api/chiaki_ng LIBS libchiaking_chiaki_ng libchiaking_crypto_openssl
    DEFINES LIBCHIAKING_TESTS_HAVE_KAKUSAN=${libchiaking_tests_have_kakusan}
    IF TARGET libchiaking_chiaki_ng AND TARGET libchiaking_crypto_openssl)
libchiaking_gtest_domain(api/consoles LIBS libchiaking)
libchiaking_gtest_domain(api/accounts LIBS libchiaking)
libchiaking_gtest_domain(api/cloud LIBS libchiaking)
libchiaking_gtest_domain(api/default_platform
    LIBS libchiaking_default_platform libchiaking libchiaking_chiaki_ng
    IF TARGET libchiaking_default_platform)
libchiaking_gtest_domain(core/detail LIBS libchiaking)
libchiaking_gtest_domain(core/net/deadline LIBS libchiaking)
libchiaking_gtest_domain(core/net/ntp LIBS libchiaking)
libchiaking_gtest_domain(core/net/nat/holepunch LIBS libchiaking libchiaking_test_crypto)
libchiaking_gtest_domain(core/net/nat/stun LIBS libchiaking libchiaking_test_crypto)
libchiaking_gtest_domain(core/net/nat/ice LIBS libchiaking libchiaking_test_crypto IF LIBCHIAKING_KAKUSAN)
libchiaking_gtest_domain(core/net/transport
    LIBS libchiaking)
libchiaking_gtest_domain(platform/threading LIBS libchiaking)
libchiaking_gtest_domain(platform/net/address LIBS libchiaking)
libchiaking_gtest_domain(platform/net/sockets LIBS libchiaking)
libchiaking_gtest_domain(platform/net/poller LIBS libchiaking)
libchiaking_gtest_domain(platform/net/posix LIBS libchiaking IF NOT WIN32)
libchiaking_gtest_domain(platform/net/win32 LIBS libchiaking IF WIN32)
libchiaking_gtest_domain(platform/dtls/fake LIBS libchiaking)
libchiaking_gtest_domain(platform/factory LIBS libchiaking)
libchiaking_gtest_domain(platform/diagnostics LIBS libchiaking)
libchiaking_gtest_domain(platform/storage LIBS libchiaking)
libchiaking_gtest_domain(platform/crypto/provider LIBS libchiaking)
libchiaking_gtest_domain(platform/crypto/openssl LIBS libchiaking_crypto_openssl libchiaking
    IF TARGET libchiaking_crypto_openssl)
libchiaking_gtest_domain(platform/crypto/boringssl LIBS libchiaking_crypto_boringssl libchiaking
    IF LIBCHIAKING_CRYPTO_BORINGSSL)
libchiaking_gtest_domain(platform/crypto/armv8
    LIBS libchiaking_crypto_armv8 libchiaking_crypto_openssl libchiaking
    IF TARGET libchiaking_crypto_armv8 AND TARGET libchiaking_crypto_openssl)
libchiaking_gtest_domain(platform/crypto/micro_ecc
    LIBS libchiaking_crypto_micro_ecc libchiaking_crypto_openssl libchiaking
    IF TARGET libchiaking_crypto_micro_ecc AND TARGET libchiaking_crypto_openssl)
libchiaking_gtest_domain(platform/http/provider LIBS libchiaking)
libchiaking_gtest_domain(platform/http/curl LIBS libchiaking_curl IF LIBCHIAKING_CURL)
libchiaking_gtest_domain(platform/websocket/provider LIBS libchiaking)
libchiaking_gtest_domain(platform/websocket/curl LIBS libchiaking_curl
    OPTIONAL_SOURCES aia_chain_tests.cpp
    OPTIONAL_LIBS libchiaking_crypto_openssl OpenSSL::Crypto
    IF LIBCHIAKING_CURL)
libchiaking_gtest_domain(platform/curl LIBS libchiaking_curl IF LIBCHIAKING_CURL)
libchiaking_gtest_domain(core/play/model LIBS libchiaking)
libchiaking_gtest_domain(core/play/usecases LIBS libchiaking libchiaking_test_crypto)
libchiaking_gtest_domain(core/play/adapters/cloud_launch LIBS libchiaking)
libchiaking_gtest_domain(core/play/adapters/links
    LIBS libchiaking libchiaking_test_crypto)
libchiaking_gtest_domain(core/play/adapters/media LIBS libchiaking
    OPTIONAL_SOURCES audio_decoder_tests.cpp microphone_encoder_tests.cpp opus_audio_decoder_tests.cpp
    OPTIONAL_LIBS libchiaking_media_opus PkgConfig::LIBCHIAKING_OPUS)
libchiaking_gtest_domain(core/play/adapters/session LIBS libchiaking)
libchiaking_gtest_domain(core/play/adapters/start LIBS libchiaking)
libchiaking_gtest_domain(core/play/openssl LIBS libchiaking libchiaking_crypto_openssl
    IF TARGET libchiaking_crypto_openssl AND NOT WIN32)
libchiaking_gtest_domain(core/play/default_platform LIBS libchiaking_default_platform libchiaking
    IF TARGET libchiaking_default_platform AND NOT WIN32)

option(LIBCHIAKING_NETWORK_TESTS "Run the tests labelled network, which need a real network interface" OFF)
if(NOT LIBCHIAKING_NETWORK_TESTS AND LIBCHIAKING_KAKUSAN)
    set_tests_properties(net_kakusan_real_socket_smoke PROPERTIES DISABLED TRUE)
endif()
if(TARGET libchiaking_dtls_openssl AND LIBCHIAKING_KAKUSAN)
    target_link_libraries(net_kakusan_real_socket_smoke PRIVATE libchiaking_dtls_openssl)
    target_compile_definitions(net_kakusan_real_socket_smoke PRIVATE WIRE_TEST_DTLS_OPENSSL=1)
endif()
if(TARGET platform_gateway_gtests)
    target_include_directories(platform_gateway_gtests SYSTEM PRIVATE "${LIBCHIAKING_MINIUPNPC_INCLUDE_DIR}")
    if(WIN32 AND LIBCHIAKING_MINIUPNPC_STATIC)
        target_compile_definitions(platform_gateway_gtests PRIVATE MINIUPNP_STATICLIB)
    endif()
endif()

if(TARGET libchiaking_crypto_openssl AND TARGET libchiaking_dtls_openssl AND LIBCHIAKING_KAKUSAN)
    add_executable(kakusan_host_demo tests/tools/kakusan_host_demo.cpp)
    target_include_directories(kakusan_host_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(kakusan_host_demo
        PRIVATE libchiaking
            libchiaking_crypto_openssl libchiaking_dtls_openssl)
    libchiaking_enable_strict_warnings(kakusan_host_demo)
endif()

function(libchiaking_add_guard name script)
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/${script}.cmake")
endfunction()

set(libchiaking_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
libchiaking_add_guard(platform_no_takion_dependency PlatformNoTakionGuard
    -DPLATFORM_DIR=${libchiaking_source_dir}/include/libchiaking/platform)
libchiaking_add_guard(public_faces_guard PublicFacesGuard -DSOURCE_DIR=${libchiaking_source_dir})
libchiaking_add_guard(face_api_guard FaceApiGuard -DSOURCE_DIR=${libchiaking_source_dir})
libchiaking_add_guard(platform_no_vendor_headers PlatformVendorHeaderGuard
    -DINCLUDE_DIR=${libchiaking_source_dir}/include/libchiaking)
libchiaking_add_guard(openssl_headers_stay_in_the_binding OpenSslHeaderGuard
    -DSOURCE_ROOT=${libchiaking_source_dir})
libchiaking_add_guard(platform_provider_boundaries PlatformProviderBoundaryGuard
    -DPLATFORM_SOURCE_DIR=${libchiaking_source_dir}/src/platform)
libchiaking_add_guard(play_launch_secrets_guard PlayLaunchSecretsGuard -DSOURCE_DIR=${libchiaking_source_dir})
libchiaking_add_guard(threading_through_the_provider ProviderBypassGuard
    -DSOURCE_ROOT=${libchiaking_source_dir} -DRULE=threading)
libchiaking_add_guard(clocks_through_the_provider ProviderBypassGuard
    -DSOURCE_ROOT=${libchiaking_source_dir} -DRULE=clocks)
libchiaking_add_guard(core_builds_no_stock_clock ProviderBypassGuard
    -DSOURCE_ROOT=${libchiaking_source_dir} -DRULE=core_stock_clocks)
libchiaking_add_guard(core_has_no_os_conditionals ProviderBypassGuard
    -DSOURCE_ROOT=${libchiaking_source_dir} -DRULE=core_os_conditionals)

set(libchiaking_package_prefix_path ${CMAKE_PREFIX_PATH})
list(JOIN libchiaking_package_prefix_path "|" libchiaking_package_prefix_argument)
add_test(NAME package_install_consumer
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_install_consumer"
        "-DPREFIX_PATH=${libchiaking_package_prefix_argument}"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DKAKUSAN=${LIBCHIAKING_KAKUSAN}"
        "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCONFIG_ARGS=$<$<BOOL:$<CONFIG>>:--config;$<CONFIG>>"
        "-DCURL_ROOT=${CURL_ROOT}"
        "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DOpenSSL_DIR=${OpenSSL_DIR}"
        "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
        "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/InstallConsumer.cmake")
set_tests_properties(package_install_consumer PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

foreach(libchiaking_takion_case IN ITEMS
        "accepts_defaults;9;20;12;23;FALSE"
        "accepts_cloud_stream_only_on_cloud_stream;9;20;12;25;FALSE"
        "rejects_cloud_stream_only_on_ps4;21;15;9;12;TRUE"
        "rejects_cloud_stream_only_on_ps5;9;23;9;12;TRUE"
        "rejects_cloud_stream_only_on_legacy_cloud_stream;9;15;25;12;TRUE"
        "rejects_v22_on_cloud_stream;9;15;9;22;TRUE"
        "rejects_v24_on_cloud_stream;9;15;9;24;TRUE"
        "rejects_v8;8;15;9;12;TRUE"
        "rejects_non_numbers;9;15;9;x;TRUE")
    list(GET libchiaking_takion_case 0 libchiaking_takion_case_name)
    list(GET libchiaking_takion_case 1 libchiaking_takion_case_ps4)
    list(GET libchiaking_takion_case 2 libchiaking_takion_case_ps5)
    list(GET libchiaking_takion_case 3 libchiaking_takion_case_legacy_cloud_stream)
    list(GET libchiaking_takion_case 4 libchiaking_takion_case_cloud_stream)
    list(GET libchiaking_takion_case 5 libchiaking_takion_case_fails)
    add_test(NAME takion_version_settings_${libchiaking_takion_case_name}
        COMMAND "${CMAKE_COMMAND}"
            -DLIBCHIAKING_TAKION_VERSION_PS4=${libchiaking_takion_case_ps4}
            -DLIBCHIAKING_TAKION_VERSION_PS5=${libchiaking_takion_case_ps5}
            -DLIBCHIAKING_TAKION_VERSION_LEGACY_CLOUD_STREAM=${libchiaking_takion_case_legacy_cloud_stream}
            -DLIBCHIAKING_TAKION_VERSION_CLOUD_STREAM=${libchiaking_takion_case_cloud_stream}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TakionVersions.cmake")
    set_tests_properties(takion_version_settings_${libchiaking_takion_case_name}
        PROPERTIES WILL_FAIL ${libchiaking_takion_case_fails})
endforeach()
