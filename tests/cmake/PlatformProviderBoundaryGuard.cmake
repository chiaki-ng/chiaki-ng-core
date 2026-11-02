if(NOT DEFINED PLATFORM_SOURCE_DIR)
    message(FATAL_ERROR "PLATFORM_SOURCE_DIR must be defined")
endif()

file(GLOB_RECURSE provider_sources
    "${PLATFORM_SOURCE_DIR}/*.cpp"
    "${PLATFORM_SOURCE_DIR}/*.hpp")
if(NOT provider_sources)
    message(FATAL_ERROR "no provider sources found under ${PLATFORM_SOURCE_DIR}")
endif()

foreach(source IN LISTS provider_sources)
    file(RELATIVE_PATH relative_path "${PLATFORM_SOURCE_DIR}" "${source}")
    file(READ "${source}" contents)
    if(contents MATCHES "#[ \t]*include[ \t]*[<\"][^\n]*\\.cpp[>\"]")
        message(FATAL_ERROR
            "provider implementation includes another .cpp file: ${relative_path}")
    endif()
    if(contents MATCHES
            "#[ \t]*(if|ifdef|ifndef|elif)[^\n]*(_WIN32|__APPLE__|__linux__|__ANDROID__|__FreeBSD__|__SWITCH__)")
        message(FATAL_ERROR
            "provider contains OS-selection logic instead of CMake selection: ${relative_path}")
    endif()
    string(REGEX MATCHALL "#[ \t]*include[ \t]*\"src/core/[^\"]*\"" core_includes "${contents}")
    foreach(core_include IN LISTS core_includes)
        if(NOT core_include MATCHES "\"src/core/utils/")
            message(FATAL_ERROR
                "provider reaches into core beyond its shared utilities: ${relative_path}: ${core_include}")
        endif()
    endforeach()
endforeach()

get_filename_component(library_source_dir "${PLATFORM_SOURCE_DIR}/.." ABSOLUTE)
file(GLOB_RECURSE core_sources
    "${library_source_dir}/core/*.cpp"
    "${library_source_dir}/core/*.hpp"
    "${library_source_dir}/../include/*.hpp")
foreach(source IN LISTS core_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#[ \t]*include[ \t]*\"src/platform/")
        message(FATAL_ERROR
            "core includes a provider's private header instead of its public interface: ${source}")
    endif()
endforeach()
