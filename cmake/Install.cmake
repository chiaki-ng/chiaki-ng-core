get_property(libchiaking_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(LIBCHIAKING_EXPORTED_TARGETS ${libchiaking_targets})
list(FILTER LIBCHIAKING_EXPORTED_TARGETS INCLUDE REGEX "^libchiaking")
set(LIBCHIAKING_EXPORT_NAMES)
foreach(libchiaking_target IN LISTS LIBCHIAKING_EXPORTED_TARGETS)
    string(REGEX REPLACE "^libchiaking_" "" libchiaking_export_name "${libchiaking_target}")
    list(APPEND LIBCHIAKING_EXPORT_NAMES ${libchiaking_export_name})
    set_target_properties(${libchiaking_target} PROPERTIES EXPORT_NAME ${libchiaking_export_name})
    add_library(libchiaking::${libchiaking_export_name} ALIAS ${libchiaking_target})
    get_target_property(libchiaking_target_type ${libchiaking_target} TYPE)
    if(libchiaking_target_type STREQUAL "INTERFACE_LIBRARY")
        target_compile_features(${libchiaking_target} INTERFACE cxx_std_23)
    else()
        target_compile_features(${libchiaking_target} PUBLIC cxx_std_23)
    endif()
endforeach()

include(CMakePackageConfigHelpers)
set(LIBCHIAKING_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/libchiaking")
install(TARGETS ${LIBCHIAKING_EXPORTED_TARGETS}
    EXPORT libchiakingTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT libchiakingTargets
    NAMESPACE libchiaking::
    DESTINATION "${LIBCHIAKING_INSTALL_CMAKEDIR}")
configure_package_config_file(cmake/libchiakingConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/libchiakingConfig.cmake"
    INSTALL_DESTINATION "${LIBCHIAKING_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libchiakingConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libchiakingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/libchiakingConfigVersion.cmake"
    DESTINATION "${LIBCHIAKING_INSTALL_CMAKEDIR}")
