if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT must be defined")
endif()

set(guarded_roots
    "${SOURCE_ROOT}/src/core"
    "${SOURCE_ROOT}/src/assembly"
    "${SOURCE_ROOT}/include/libchiaking")

set(binding_roots
    "${SOURCE_ROOT}/src/platform/crypto/openssl"
    "${SOURCE_ROOT}/src/platform/crypto/boringssl"
    "${SOURCE_ROOT}/src/platform/crypto/evp"
    "${SOURCE_ROOT}/src/platform/dtls/openssl")

foreach(root IN LISTS binding_roots)
    if(NOT IS_DIRECTORY "${root}")
        message(FATAL_ERROR "binding directory is missing: ${root}")
    endif()
endforeach()

set(checked 0)
foreach(root IN LISTS guarded_roots)
    file(GLOB_RECURSE files "${root}/*.hpp" "${root}/*.h" "${root}/*.cpp" "${root}/*.c")
    foreach(file IN LISTS files)
        math(EXPR checked "${checked} + 1")
        file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]openssl/")
        if(includes)
            message(FATAL_ERROR "${file} includes an OpenSSL header outside the bindings: ${includes}")
        endif()
    endforeach()
endforeach()

if(checked EQUAL 0)
    message(FATAL_ERROR "no sources found under ${SOURCE_ROOT}")
endif()
