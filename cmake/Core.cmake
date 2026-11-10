option(LIBCHIAKING_KAKUSAN "Build Kakusan, which lets invited players join a live play session" ON)
option(LIBCHIAKING_CLOUD "Build cloud streaming; without it every cloud catalog call and cloud launch is refused before it reaches PS" OFF)
option(LIBCHIAKING_PS5_CLOUD_LAUNCH "Let cloud launches start on PS5 cloud streaming; the older service is unaffected" ON)
option(LIBCHIAKING_PS4_CLOUD_LAUNCH "Let cloud launches start on the older PS4 cloud service; PS5 cloud streaming is unaffected" ON)

set(libchiaking_core_excludes
    "^src/core/chiaki_ng\\.cpp$"
    "^src/core/media/adapters/(microphone_encoder|audio_decoder)\\.cpp$"
    "^src/platform/.*/(posix|win32|darwin|switch|utc_offset_[a-z0-9]+)\\.cpp$")
if(NOT LIBCHIAKING_KAKUSAN)
    list(APPEND libchiaking_core_excludes "^src/core/kakusan/" "^src/core/net/kakusan/")
endif()

if(LIBCHIAKING_CLOUD)
    list(APPEND libchiaking_core_excludes "^src/core/catalog/catalog_without_cloud\\.cpp$")
else()
    list(APPEND libchiaking_core_excludes
        "^src/core/catalog/catalog\\.cpp$"
        "^src/core/catalog/usecases/(list_games|clear_for_launch|launch_checks|show_details|recently_played|read_streamable_list|read_library|read_categories|fetch_progress)\\.cpp$"
        "^src/core/psn/gk/(client|launch_spec)\\.cpp$"
        "^src/core/play/adapters/cloud_launch/(cloud_launch_run|launch_observer)\\.cpp$"
        "^src/core/play/adapters/links/cloud/(cloud_link|cloud_link_configuration|datacenter_survey|server_connect|server_reservation)\\.cpp$"
        "^src/core/play/adapters/links/cloud/services/"
        "^src/core/catalog/usecases/(cloud_stream|legacy_cloud_stream)/"
        "^src/core/catalog/model/legacy_cloud_stream/"
        "^src/core/catalog/model/cloud_stream/(access_route|cloud_game_listing|games|generation_links)\\.cpp$"
        "^src/core/catalog/model/my_games/my_games_assembly\\.cpp$"
        "^src/core/catalog/adapters/ps/(cloud_exclusions|cloud_game_resolver|old_service_shelves|ownership_records|ps5_library|ps5_store_lists|public_store_walk|store_app_title|streaming_copies)\\.cpp$")
endif()

libchiaking_glob_sources(libchiaking_core_sources
    RECURSE src/core
    FLAT src/platform/clock src/platform/crypto src/platform/diagnostics src/platform/factory
        src/platform/http src/platform/net src/platform/random src/platform/storage src/platform/threading
    EXCLUDE ${libchiaking_core_excludes})

add_library(libchiaking STATIC
    ${libchiaking_core_sources}
    src/core/net/protocol/takion/generated/takion.pb.c
    "${LIBCHIAKING_PLATFORM_SECURE_ZERO_SOURCE}"
    "${LIBCHIAKING_PLATFORM_UTC_OFFSET_SOURCE}"
    "${LIBCHIAKING_PLATFORM_THREADING_SOURCE}"
    "${LIBCHIAKING_PLATFORM_RANDOM_SOURCE}"
    "${LIBCHIAKING_PLATFORM_NETWORK_SOURCE}"
    "${LIBCHIAKING_PLATFORM_DIAGNOSTICS_SOURCE}"
    "${LIBCHIAKING_PLATFORM_STORAGE_SOURCE}")
set_source_files_properties(src/core/net/protocol/takion/generated/takion.pb.c
    PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

file(GLOB_RECURSE libchiaking_public_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/libchiaking/*.hpp")
target_sources(libchiaking PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    FILES ${libchiaking_public_headers})

target_include_directories(libchiaking
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    PUBLIC "$<BUILD_INTERFACE:${LIBCHIAKING_GENERATED_DIR}>")
target_compile_definitions(libchiaking PRIVATE ${LIBCHIAKING_PLATFORM_DEFINITIONS}
    LIBCHIAKING_HAS_CLOUD=$<BOOL:${LIBCHIAKING_CLOUD}>
    LIBCHIAKING_HAS_PS5_CLOUD_LAUNCH=$<BOOL:${LIBCHIAKING_PS5_CLOUD_LAUNCH}>
    LIBCHIAKING_HAS_PS4_CLOUD_LAUNCH=$<BOOL:${LIBCHIAKING_PS4_CLOUD_LAUNCH}>)
target_link_libraries(libchiaking
    PUBLIC ${LIBCHIAKING_PLATFORM_LIBRARIES} libchiaking_vendored_headers libchiaking_simdutf
    PRIVATE libchiaking_nanopb libchiaking_picohttpparser libchiaking_jerasure)
libchiaking_enable_strict_warnings(libchiaking)
target_compile_options(libchiaking PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<CONFIG:RelWithDebInfo>>:-g1>)

option(LIBCHIAKING_UNITY_BUILD "Compile the core library's sources in unity batches" ON)
if(LIBCHIAKING_UNITY_BUILD)
    set_target_properties(libchiaking PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)
    target_compile_options(libchiaking PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-subobject-linkage>)
    set_source_files_properties(
        src/core/play/adapters/links/internet/internet_link.cpp
        src/core/play/adapters/links/internet/internet_pairer.cpp
        src/core/play/adapters/links/internet/internet_start.cpp
        "${LIBCHIAKING_PLATFORM_SECURE_ZERO_SOURCE}"
        "${LIBCHIAKING_PLATFORM_UTC_OFFSET_SOURCE}"
        "${LIBCHIAKING_PLATFORM_THREADING_SOURCE}"
        "${LIBCHIAKING_PLATFORM_RANDOM_SOURCE}"
        "${LIBCHIAKING_PLATFORM_NETWORK_SOURCE}"
        "${LIBCHIAKING_PLATFORM_DIAGNOSTICS_SOURCE}"
        "${LIBCHIAKING_PLATFORM_STORAGE_SOURCE}"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endif()
