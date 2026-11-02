option(LIBCHIAKING_MEDIA_OPUS "Build the Opus audio decoder and microphone encoder when Opus is found" ON)
set(LIBCHIAKING_HAS_MEDIA_OPUS 0)
if(LIBCHIAKING_MEDIA_OPUS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBCHIAKING_OPUS QUIET IMPORTED_TARGET GLOBAL opus)
    endif()
endif()
if(TARGET PkgConfig::LIBCHIAKING_OPUS)
    add_library(libchiaking_media_opus STATIC
        src/core/media/adapters/microphone_encoder.cpp
        src/core/media/adapters/audio_decoder.cpp)
    target_include_directories(libchiaking_media_opus
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(libchiaking_media_opus
        PUBLIC libchiaking
        PRIVATE PkgConfig::LIBCHIAKING_OPUS)
    libchiaking_enable_strict_warnings(libchiaking_media_opus)
    set(LIBCHIAKING_HAS_MEDIA_OPUS 1)
endif()
message(STATUS "libchiaking Opus media: ${LIBCHIAKING_HAS_MEDIA_OPUS}")
