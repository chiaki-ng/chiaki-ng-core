if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR must be defined")
endif()

set(include_root "${SOURCE_DIR}/include")
set(domains users consoles catalog trophies messages play kakusan)
set(vocabulary_platform_headers
    libchiaking/platform/byte_units.hpp
    libchiaking/platform/diagnostics/provider.hpp
    libchiaking/platform/error.hpp
    libchiaking/platform/http/provider.hpp
    libchiaking/platform/http/values.hpp
    libchiaking/platform/net/error.hpp
    libchiaking/platform/net/types.hpp
    libchiaking/platform/runtime/clock_provider.hpp
    libchiaking/platform/runtime/stop_token.hpp
    libchiaking/platform/runtime/threading.hpp)
set(violations)

function(reached_from header out)
    set(pending "${header}")
    set(reached)
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST reached OR NOT EXISTS "${include_root}/${current}")
            continue()
        endif()
        list(APPEND reached "${current}")
        file(STRINGS "${include_root}/${current}" lines REGEX "^#include [\"<]libchiaking/")
        foreach(line IN LISTS lines)
            string(REGEX REPLACE "^#include [\"<](libchiaking/[^\">]+)[\">].*$" "\\1" included "${line}")
            list(APPEND pending "${included}")
        endforeach()
    endwhile()
    set(${out} "${reached}" PARENT_SCOPE)
endfunction()

foreach(domain IN LISTS domains)
    set(face "libchiaking/${domain}.hpp")
    reached_from("libchiaking/${domain}_types.hpp" values)
    file(STRINGS "${include_root}/${face}" listed REGEX "^(class|struct) [A-Za-z_][A-Za-z0-9_]*;$")
    list(TRANSFORM listed REPLACE "[;]$" "")
    file(GLOB headers RELATIVE "${include_root}" "${include_root}/libchiaking/detail/${domain}/*.hpp")
    foreach(header IN LISTS headers)
        if(header IN_LIST values)
            continue()
        endif()
        file(STRINGS "${include_root}/${header}" definitions REGEX "^(class|struct) [A-Za-z_][A-Za-z0-9_]*[^;]*{$")
        foreach(definition IN LISTS definitions)
            string(REGEX REPLACE "^(class|struct) ([A-Za-z_][A-Za-z0-9_]*).*$" "\\1 \\2" declaration "${definition}")
            if(NOT declaration IN_LIST listed)
                list(APPEND violations
                    "${face} does not list ${declaration} from ${header}, which ${domain}_types.hpp does not reach")
            endif()
        endforeach()
    endforeach()
endforeach()

set(faces libchiaking/chiaki_ng.hpp)
foreach(domain IN LISTS domains)
    list(APPEND faces "libchiaking/${domain}.hpp" "libchiaking/${domain}_types.hpp")
endforeach()
foreach(face IN LISTS faces)
    reached_from("${face}" reached)
    foreach(header IN LISTS reached)
        if(header MATCHES "^libchiaking/platform/" AND NOT header IN_LIST vocabulary_platform_headers)
            list(APPEND violations "${face} reaches ${header}, which is not a vocabulary platform header")
        endif()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n" report)
    message(FATAL_ERROR "the face lists are not as agreed:\n${report}")
endif()
