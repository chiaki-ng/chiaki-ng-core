if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT must be defined")
endif()
if(NOT DEFINED RULE)
    message(FATAL_ERROR "RULE must be defined")
endif()

set(stock_providers
    "${SOURCE_ROOT}/src/platform/threading/"
    "${SOURCE_ROOT}/src/platform/clock/"
    "${SOURCE_ROOT}/src/lib/"
    "${SOURCE_ROOT}/tests/lib/")

set(provider_self_checks
    "${SOURCE_ROOT}/include/libchiaking/platform/conformance.hpp"
    "${SOURCE_ROOT}/tests/unit/platform/threading/extended_tests.cpp")

if(RULE STREQUAL "threading")
    set(roots src include/libchiaking tests)
    set(forbidden
        "std::(thread|jthread|async|future|shared_future|promise|packaged_task|mutex|recursive_mutex|timed_mutex|recursive_timed_mutex|shared_mutex|shared_timed_mutex|condition_variable|condition_variable_any|lock_guard|unique_lock|scoped_lock|shared_lock|latch|barrier|counting_semaphore|binary_semaphore)([^a-zA-Z0-9_]|$)|this_thread::|#[ \t]*include[ \t]*<(thread|mutex|shared_mutex|condition_variable|future|latch|barrier|semaphore|stop_token|pthread\\.h)>")
    set(exempt ${provider_self_checks})
    set(meaning "uses the standard threading layer instead of platform::Thread, Mutex, Lock, ConditionVariable and Sleeper")
elseif(RULE STREQUAL "clocks")
    set(roots src include/libchiaking tests)
    set(forbidden
        "(steady_clock|system_clock|high_resolution_clock|utc_clock|tai_clock|gps_clock|file_clock)::now|clock_gettime|gettimeofday|mach_absolute_time|QueryPerformanceCounter|GetSystemTimeAsFileTime")
    set(exempt ${provider_self_checks})
    set(meaning "reads a clock directly instead of through a platform::ClockProvider")
elseif(RULE STREQUAL "core_stock_clocks")
    set(roots src/core src/assembly)
    set(forbidden "(^|[^a-zA-Z0-9_])(platform::)?(SteadyClock|SystemClock)([^a-zA-Z0-9_]|$)")
    set(exempt "${SOURCE_ROOT}/src/assembly/")
    set(meaning "builds a stock clock instead of using the one the factory injects")
elseif(RULE STREQUAL "core_os_conditionals")
    set(roots src/core src/assembly)
    set(forbidden "#[ \t]*(if|ifdef|ifndef|elif).*(_WIN32|_WIN64|__APPLE__|__MACH__|__linux__|__ANDROID__|__FreeBSD__|__SWITCH__|__unix__|TARGET_OS_)")
    set(exempt)
    set(meaning "selects code by operating system; that choice belongs to CMake and src/platform")
else()
    message(FATAL_ERROR "unknown RULE ${RULE}")
endif()

set(violations)
set(checked 0)
foreach(root IN LISTS roots)
    file(GLOB_RECURSE files
        "${SOURCE_ROOT}/${root}/*.hpp" "${SOURCE_ROOT}/${root}/*.h"
        "${SOURCE_ROOT}/${root}/*.cpp" "${SOURCE_ROOT}/${root}/*.cc"
        "${SOURCE_ROOT}/${root}/*.c" "${SOURCE_ROOT}/${root}/*.mm")
    foreach(file IN LISTS files)
        set(skip FALSE)
        foreach(prefix IN LISTS stock_providers exempt)
            string(FIND "${file}" "${prefix}" at)
            if(at EQUAL 0)
                set(skip TRUE)
            endif()
        endforeach()
        if(skip)
            continue()
        endif()
        math(EXPR checked "${checked} + 1")
        file(STRINGS "${file}" lines REGEX "${forbidden}")
        foreach(line IN LISTS lines)
            string(STRIP "${line}" line)
            if(line MATCHES "^///")
                continue()
            endif()
            file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${file}")
            list(APPEND violations "${relative}: ${line}")
        endforeach()
    endforeach()
endforeach()

if(checked EQUAL 0)
    message(FATAL_ERROR "no sources found under ${SOURCE_ROOT}")
endif()

if(violations)
    list(JOIN violations "\n" report)
    message(FATAL_ERROR "code ${meaning}:\n${report}")
endif()
