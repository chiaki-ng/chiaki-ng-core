if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR must be defined")
endif()

set(headers
    "${SOURCE_DIR}/src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
    "${SOURCE_DIR}/src/core/play/adapters/links/internet/internet_link_configuration.hpp"
    "${SOURCE_DIR}/src/core/play/adapters/links/home_link_configuration.hpp"
    "${SOURCE_DIR}/src/core/play/adapters/start/link_request.hpp"
    "${SOURCE_DIR}/include/libchiaking/play.hpp"
    "${SOURCE_DIR}/include/libchiaking/play_types.hpp")
file(GLOB_RECURSE play_headers "${SOURCE_DIR}/include/libchiaking/detail/play/*.hpp")
if(NOT play_headers)
    message(FATAL_ERROR "no public play headers found")
endif()
list(APPEND headers ${play_headers})

set(violations)
foreach(header IN LISTS headers)
    if(NOT EXISTS "${header}")
        message(FATAL_ERROR "missing launch header ${header}")
    endif()
    file(STRINGS "${header}" lines
        REGEX "[Nn]psso|BearerToken|bearer_token|AppToken|app_token|DeviceId|device_id|[Dd]uid")
    foreach(line IN LISTS lines)
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${header}")
        list(APPEND violations "${relative}: ${line}")
    endforeach()
endforeach()
if(violations)
    list(JOIN violations "\n" report)
    message(FATAL_ERROR "a launch header names a credential or a device id:\n${report}")
endif()
