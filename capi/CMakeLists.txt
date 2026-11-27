# libplanck.so: the engine behind a plain C interface, for programs that are
# not written in C++ or do not want to be tied to planck's internals.
#
# The engine's sources are compiled a second time here, as position
# independent code with hidden symbols. The static libraries the venue is
# built from are left exactly as they were: making them position
# independent would change the code on the venue's hot path for the sake of
# a library the venue does not use.
set(planck_capi_engine_sources)
foreach(library planck_core planck_sys planck_mem planck_book planck_wire planck_engine)
    get_target_property(library_sources ${library} SOURCES)
    foreach(source ${library_sources})
        list(APPEND planck_capi_engine_sources ${PROJECT_SOURCE_DIR}/${source})
    endforeach()
endforeach()

add_library(planck_capi SHARED src/planck.cpp ${planck_capi_engine_sources})
add_library(planck::capi ALIAS planck_capi)
set_target_properties(planck_capi PROPERTIES
    OUTPUT_NAME planck
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    # Linked the way a C library is. The engine uses nothing from the C++
    # runtime, and this keeps it that way: a use of it would fail the link.
    LINKER_LANGUAGE C
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/planck.map
)
target_include_directories(planck_capi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
target_compile_definitions(planck_capi PRIVATE
    PLANCK_BUILDING_SHARED
    PLANCK_VERSION_STRING="${PROJECT_VERSION}"
)
target_link_libraries(planck_capi PRIVATE planck_flags Threads::Threads)
# Export the functions in planck.h and nothing else, and refuse to link if
# the library depends on a symbol nothing provides.
target_link_options(planck_capi PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/planck.map
)
if(NOT PLANCK_SANITIZE)
    # Sanitizer runtimes are found at load time, so this check only works
    # without them.
    target_link_options(planck_capi PRIVATE -Wl,--no-undefined)
endif()

if(PLANCK_BUILD_TESTS)
    # A C program, built as strict C11, that trades through the library.
    add_executable(planck_capi_smoke tests/capi_smoke.c)
    set_target_properties(planck_capi_smoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                                       C_EXTENSIONS OFF)
    target_compile_options(planck_capi_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror
                                                     -Wconversion -Wshadow)
    target_link_libraries(planck_capi_smoke PRIVATE planck::capi planck_sanitize)
    add_test(NAME capi_smoke COMMAND planck_capi_smoke)
    set_tests_properties(capi_smoke PROPERTIES LABELS capi TIMEOUT 60)

    # What the library exports, and what it needs. A sanitized build needs
    # its sanitizer's runtime, so the second half is skipped there.
    if(PLANCK_SANITIZE)
        set(planck_capi_check_runtime no)
    else()
        set(planck_capi_check_runtime yes)
    endif()
    add_test(NAME capi_exports
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/capi_exports.sh
                     $<TARGET_FILE:planck_capi> ${CMAKE_CURRENT_SOURCE_DIR}/include/planck.h
                     ${planck_capi_check_runtime})
    set_tests_properties(capi_exports PROPERTIES LABELS capi TIMEOUT 60)

    # The same library from Python, if cffi is there to load it with. Python
    # cannot load a sanitized library without the sanitizer loaded first.
    find_package(Python3 QUIET COMPONENTS Interpreter)
    set(planck_have_cffi 1)
    if(Python3_FOUND AND NOT PLANCK_SANITIZE)
        execute_process(COMMAND ${Python3_EXECUTABLE} -c "import cffi"
                        RESULT_VARIABLE planck_have_cffi OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(planck_have_cffi EQUAL 0)
        add_test(NAME capi_cffi
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/capi_cffi_test.py
                         $<TARGET_FILE:planck_capi> ${CMAKE_CURRENT_SOURCE_DIR}/include/planck.h)
        set_tests_properties(capi_cffi PROPERTIES LABELS capi TIMEOUT 60)
    else()
        message(STATUS "planck: capi_cffi test skipped (needs python3 with cffi, and no sanitizer)")
    endif()
endif()
