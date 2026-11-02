set(LIBCHIAKING_COMMIT "" CACHE STRING
    "Source commit the build reports as its version's commit; read from git when empty")
set(libchiaking_commit "${LIBCHIAKING_COMMIT}")
if(libchiaking_commit STREQUAL "")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE libchiaking_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE libchiaking_commit_result
            ERROR_QUIET)
        if(NOT libchiaking_commit_result EQUAL 0)
            set(libchiaking_commit "")
        endif()
    endif()
endif()
if(NOT libchiaking_commit MATCHES "^[0-9a-f]*$")
    message(FATAL_ERROR "LIBCHIAKING_COMMIT must be a lowercase hexadecimal commit, not '${libchiaking_commit}'")
endif()
set(LIBCHIAKING_VERSION_PRERELEASE "" CACHE STRING
    "Pre-release label after the version, such as rc1 for 0.1.0-rc1; empty for a release")
if(NOT LIBCHIAKING_VERSION_PRERELEASE MATCHES "^([0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
    message(FATAL_ERROR "LIBCHIAKING_VERSION_PRERELEASE must be dot-separated letters, digits and hyphens, not '${LIBCHIAKING_VERSION_PRERELEASE}'")
endif()
configure_file(cmake/version_config.hpp.in
    "${LIBCHIAKING_GENERATED_DIR}/version_config.hpp" @ONLY)
