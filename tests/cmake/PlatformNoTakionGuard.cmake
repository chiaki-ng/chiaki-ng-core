if(NOT DEFINED PLATFORM_DIR)
    message(FATAL_ERROR "PLATFORM_DIR must be defined")
endif()

file(GLOB_RECURSE platform_headers "${PLATFORM_DIR}/*.hpp")

if(NOT platform_headers)
    message(FATAL_ERROR "no platform headers found under ${PLATFORM_DIR}")
endif()

foreach(header ${platform_headers})
    file(READ "${header}" contents)
    string(TOLOWER "${contents}" lowered)
    if(lowered MATCHES "takion")
        message(FATAL_ERROR "${header} depends on Takion")
    endif()
endforeach()
