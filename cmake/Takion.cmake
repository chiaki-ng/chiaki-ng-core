set(LIBCHIAKING_TAKION_VERSION_OVERRIDE 0 CACHE STRING
    "Takion version every session uses unless the caller overrides it; 0 keeps negotiation")
if(NOT LIBCHIAKING_TAKION_VERSION_OVERRIDE MATCHES "^[0-9]+$")
    message(FATAL_ERROR "LIBCHIAKING_TAKION_VERSION_OVERRIDE must be a non-negative integer")
endif()

set(LIBCHIAKING_TAKION_VERSION_PS4 9 CACHE STRING
    "Takion version pinned for PS4 console streams")
set(LIBCHIAKING_TAKION_VERSION_PS5 20 CACHE STRING
    "Takion version pinned for PS5 console streams")
set(LIBCHIAKING_TAKION_VERSION_LEGACY_CLOUD_STREAM 12 CACHE STRING
    "Takion version pinned for LegacyCloudStream streams")
set(LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM 23 CACHE STRING
    "Takion version pinned for CloudStream streams")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/TakionVersions.cmake")
set(LIBCHIAKING_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/takion_build_config.hpp.in
    "${LIBCHIAKING_GENERATED_DIR}/takion_build_config.hpp" @ONLY)
