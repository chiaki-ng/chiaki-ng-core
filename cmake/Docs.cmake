find_package(Doxygen OPTIONAL_COMPONENTS dot)
find_package(Python3 COMPONENTS Interpreter)
if(DOXYGEN_FOUND AND Python3_Interpreter_FOUND)
    add_custom_target(docs
        COMMAND "${Python3_EXECUTABLE}" tools/docs/docs.py build
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating the API reference in build/docs/html"
        VERBATIM)
    add_custom_target(docs-lint
        COMMAND "${Python3_EXECUTABLE}" tools/docs/docs.py lint
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking that every public declaration is documented"
        VERBATIM)
    add_custom_target(docs-lint-internal
        COMMAND "${Python3_EXECUTABLE}" tools/docs/docs.py lint-internal
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking that every internal declaration is documented"
        VERBATIM)
endif()
option(LIBCHIAKING_DOCS_LINT_INTERNAL "Gate the tests on documenting every internal declaration" ON)
