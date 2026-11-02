foreach(required IN ITEMS BUILD_DIR SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(GLOB consumer_sources "${SOURCE_DIR}/tests/packaging/consumer/*.cpp")
foreach(source IN LISTS consumer_sources)
    file(STRINGS "${source}" consumer_includes REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]libchiaking/")
    foreach(line IN LISTS consumer_includes)
        if(NOT line MATCHES "[\"<]libchiaking/([a-z_]+\\.hpp|platform/(platform|defaults|stock_providers)\\.hpp|testing/[a-z_/]+\\.hpp)[\">]")
            message(FATAL_ERROR "${source} includes a header outside the public contract: ${line}")
        endif()
    endforeach()
endforeach()
set(prefix "${WORK_DIR}/prefix")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" ${CONFIG_ARGS}
    RESULT_VARIABLE installed)
if(NOT installed EQUAL 0)
    message(FATAL_ERROR "install failed")
endif()
foreach(expected IN ITEMS include/libchiaking/platform/default.hpp include/libchiaking/platform/platform.hpp
        include/libchiaking/users.hpp include/libchiaking/users_types.hpp
        include/libchiaking/consoles.hpp include/libchiaking/consoles_types.hpp
        include/libchiaking/catalog.hpp include/libchiaking/catalog_types.hpp
        include/libchiaking/trophies.hpp include/libchiaking/trophies_types.hpp
        include/libchiaking/detail/trophies/trophies.hpp
        include/libchiaking/messages.hpp include/libchiaking/messages_types.hpp
        include/libchiaking/detail/messages/messages.hpp
        include/libchiaking/play.hpp include/libchiaking/play_types.hpp
        include/libchiaking/detail/consoles/known_consoles.hpp include/libchiaking/detail/consoles/console.hpp
        include/libchiaking/detail/consoles/network_search.hpp include/libchiaking/testing/fake_console.hpp
        include/libchiaking/testing/in_memory_network.hpp
        include/libchiaking/detail/play/internet_pairer.hpp include/libchiaking/detail/diagnostics/log_queue.hpp
        include/libchiaking/detail/diagnostics/session_journal.hpp include/libchiaking/detail/diagnostics/api_capture.hpp
        include/libchiaking/detail/users/credentials.hpp include/libchiaking/detail/users/credential.hpp
        include/libchiaking/detail/catalog/catalog.hpp
        include/libchiaking/detail/catalog/my_games.hpp include/libchiaking/detail/catalog/launch_verdict.hpp
        include/libchiaking/platform/storage/record_store.hpp
        include/libchiaking/detail/consoles/pairing_target.hpp include/libchiaking/detail/wire_sizes.hpp
        include/libchiaking/detail/users/account_id.hpp include/libchiaking/detail/play/access_code.hpp
        include/libchiaking/detail/play/play.hpp include/libchiaking/detail/play/session_share.hpp
        include/libchiaking/testing/manual_clock.hpp
        include/libchiaking/detail/play/session.hpp include/libchiaking/detail/media/media_sink.hpp
        include/libchiaking/detail/play/controllers.hpp include/libchiaking/detail/play/seats.hpp
        include/libchiaking/detail/controllers/feedback_inbox.hpp include/libchiaking/detail/media/audio_decoder.hpp
        include/libchiaking/testing/play/scripted_link.hpp)
    if(NOT EXISTS "${prefix}/${expected}")
        message(FATAL_ERROR "install is missing ${expected}")
    endif()
endforeach()
file(GLOB installed_top RELATIVE "${prefix}/include" "${prefix}/include/*")
if(NOT installed_top STREQUAL "libchiaking")
    message(FATAL_ERROR "install carries more than include/libchiaking: ${installed_top}")
endif()
file(GLOB_RECURSE installed_headers RELATIVE "${prefix}/include/libchiaking" "${prefix}/include/libchiaking/*")
file(GLOB_RECURSE shipped_headers RELATIVE "${SOURCE_DIR}/include/libchiaking" "${SOURCE_DIR}/include/libchiaking/*")
foreach(header IN LISTS installed_headers)
    if(NOT header IN_LIST shipped_headers)
        message(FATAL_ERROR "install carries include/libchiaking/${header}, which is outside the public tree")
    endif()
    if(NOT header MATCHES "^((users|consoles|catalog|trophies|messages|play|kakusan)(_types)?\\.hpp|(chiaki_ng|version)\\.hpp|(detail|platform|testing)/.+\\.hpp)$")
        message(FATAL_ERROR "install carries include/libchiaking/${header}, which is outside the agreed public tree")
    endif()
endforeach()
foreach(header IN LISTS shipped_headers)
    if(NOT header IN_LIST installed_headers)
        message(FATAL_ERROR "install is missing include/libchiaking/${header}")
    endif()
endforeach()
foreach(removed IN ITEMS include/libchiaking/stream include/libchiaking/stream.hpp include/libchiaking/local_stream.hpp
        include/libchiaking/remote_stream.hpp include/libchiaking/cloud_stream.hpp include/libchiaking/cloud
        include/libchiaking/testing/scripted_session.hpp include/libchiaking/testing/recording_media.hpp
        include/libchiaking/media/opus_audio_decoder.hpp include/libchiaking/senkusha include/libchiaking/senkusha.hpp
        include/libchiaking/takion include/libchiaking/takion.hpp include/libchiaking/rp_reason.hpp
        include/libchiaking/ports.hpp include/libchiaking/registration include/libchiaking/psn include/libchiaking/net
        include/libchiaking/media include/libchiaking/memory include/libchiaking/diagnostics include/libchiaking/progress
        include/libchiaking/users include/libchiaking/consoles include/libchiaking/catalog include/libchiaking/trophies
        include/libchiaking/play
        include/libchiaking/kakusan include/libchiaking/platform.hpp include/libchiaking/expected.hpp)
    if(EXISTS "${prefix}/${removed}")
        message(FATAL_ERROR "install still carries ${removed}")
    endif()
endforeach()

set(prefix_path "${prefix}")
if(PREFIX_PATH)
    string(REPLACE "|" ";" forwarded_prefix_path "${PREFIX_PATH}")
    list(APPEND prefix_path ${forwarded_prefix_path})
endif()
set(consumer_cache "${WORK_DIR}/consumer-cache.cmake")
file(WRITE "${consumer_cache}" "set(CMAKE_PREFIX_PATH [==[${prefix_path}]==] CACHE STRING \"\")\n")
set(configure_args
    -C "${consumer_cache}"
    -S "${SOURCE_DIR}/tests/packaging/consumer"
    -B "${WORK_DIR}/consumer"
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE})
if(GENERATOR)
    list(APPEND configure_args -G "${GENERATOR}")
endif()
foreach(forwarded IN ITEMS CURL_ROOT OPENSSL_ROOT_DIR OpenSSL_DIR CMAKE_C_COMPILER CMAKE_CXX_COMPILER
        CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS)
    if(${forwarded})
        list(APPEND configure_args "-D${forwarded}=${${forwarded}}")
    endif()
endforeach()
string(REPLACE "|" ";" configure_args "${configure_args}")
execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args} RESULT_VARIABLE configured)
if(NOT configured EQUAL 0)
    message(FATAL_ERROR "consumer configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/consumer" ${CONFIG_ARGS}
    RESULT_VARIABLE built)
if(NOT built EQUAL 0)
    message(FATAL_ERROR "consumer build failed")
endif()
set(consumer_names libchiaking_cpp_consumer)
foreach(expected IN ITEMS kakusan.hpp shared_session.hpp guest_visit.hpp remembered_devices.hpp
        device_identity.hpp device_store.hpp invitation.hpp values.hpp)
    if(NOT EXISTS "${prefix}/include/libchiaking/detail/kakusan/${expected}")
        message(FATAL_ERROR "install is missing include/libchiaking/detail/kakusan/${expected}")
    endif()
endforeach()
if(KAKUSAN)
    list(APPEND consumer_names libchiaking_kakusan_consumer)
endif()
if(EXISTS "${prefix}/include/libchiaking/chiaki_ng.hpp")
    foreach(expected IN ITEMS users.hpp users_types.hpp consoles.hpp consoles_types.hpp catalog.hpp catalog_types.hpp
            trophies.hpp trophies_types.hpp messages.hpp messages_types.hpp
            play.hpp play_types.hpp kakusan.hpp kakusan_types.hpp detail/expected.hpp
            detail/consoles/starting.hpp)
        if(NOT EXISTS "${prefix}/include/libchiaking/${expected}")
            message(FATAL_ERROR "install is missing include/libchiaking/${expected}")
        endif()
    endforeach()
    list(APPEND consumer_names libchiaking_faces_consumer)
endif()
if(EXISTS "${prefix}/include/libchiaking/kakusan")
    message(FATAL_ERROR "install still carries include/libchiaking/kakusan")
endif()
foreach(consumer_name IN LISTS consumer_names)
    file(GLOB_RECURSE consumer LIST_DIRECTORIES false
        "${WORK_DIR}/consumer/${consumer_name}" "${WORK_DIR}/consumer/${consumer_name}.exe")
    list(GET consumer 0 consumer)
    execute_process(COMMAND "${consumer}" RESULT_VARIABLE ran)
    if(NOT ran EQUAL 0)
        message(FATAL_ERROR "${consumer_name} exited with ${ran}")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")
