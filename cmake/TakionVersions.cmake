foreach(libchiaking_takion_target IN ITEMS PS4 PS5 LEGACY_CLOUD_STREAM)
    if(NOT LIBCHIAKING_TAKION_VERSION_${libchiaking_takion_target} MATCHES "^(7|9|1[0-9]|20)$")
        message(FATAL_ERROR "LIBCHIAKING_TAKION_VERSION_${libchiaking_takion_target} must be "
            "7 or 9 through 20; 21, 23 and 25 are CloudStream only")
    endif()
endforeach()
if(NOT LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM MATCHES "^(7|9|1[0-9]|2[0135])$")
    message(FATAL_ERROR "LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM must be 7, 9 through 21, 23 or 25")
endif()
