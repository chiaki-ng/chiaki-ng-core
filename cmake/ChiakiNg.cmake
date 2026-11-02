add_library(libchiaking_chiaki_ng STATIC src/core/chiaki_ng.cpp src/assembly/stock_chiaki_ng.cpp)
target_include_directories(libchiaking_chiaki_ng PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libchiaking_chiaki_ng PUBLIC libchiaking)
target_compile_definitions(libchiaking_chiaki_ng PRIVATE
    LIBCHIAKING_HAS_KAKUSAN=$<BOOL:${LIBCHIAKING_KAKUSAN}>
    LIBCHIAKING_HAS_DEFAULT_PLATFORM=$<TARGET_EXISTS:libchiaking_default_platform>)
if(TARGET libchiaking_default_platform)
    target_link_libraries(libchiaking_chiaki_ng PUBLIC libchiaking_default_platform)
endif()
libchiaking_enable_strict_warnings(libchiaking_chiaki_ng)
