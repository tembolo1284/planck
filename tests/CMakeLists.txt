# Use an installed GoogleTest if there is one, otherwise fetch a pinned release.
find_package(GTest 1.14 QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

# planck_add_test(<name> LABEL <label> SOURCES <files...>)
# Builds one test executable and registers each of its cases with CTest.
function(planck_add_test name)
    cmake_parse_arguments(ARG "" "LABEL" "SOURCES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE planck::gateway planck::feed planck::journal
                                          GTest::gtest_main)
    # A test that hangs is a failed test. Without a timeout CTest waits 25 minutes.
    gtest_discover_tests(${name} PROPERTIES LABELS ${ARG_LABEL} TIMEOUT 120 DISCOVERY_TIMEOUT 60)
endfunction()

# Fast, deterministic checks of one module at a time.
planck_add_test(planck_unit_tests LABEL unit SOURCES
    unit/core_test.cpp
    unit/sys_test.cpp
    unit/mem_test.cpp
    unit/ring_test.cpp
    unit/ladder_bitmap_test.cpp
    unit/order_book_test.cpp
    unit/wire_test.cpp
    unit/shard_test.cpp
    unit/net_test.cpp
    unit/perf_test.cpp
    unit/journal_test.cpp
)

# Randomized checks against an independent model. Scale with PLANCK_TEST_SCALE.
planck_add_test(planck_property_tests LABEL property SOURCES
    property/book_differential_test.cpp
    property/shard_audit_test.cpp
    property/journal_replay_test.cpp
)

# More than one thread, or real sockets on the loopback interface.
planck_add_test(planck_integration_tests LABEL integration SOURCES
    integration/ring_stress_test.cpp
    integration/shard_thread_test.cpp
    integration/gateway_test.cpp
    integration/feed_test.cpp
    integration/full_stack_test.cpp
    integration/journal_thread_test.cpp
)

if(TARGET planckd)
    # Argument parsing and formatting shared by the programs in apps/.
    planck_add_test(planck_apps_tests LABEL unit SOURCES
        unit/apps_support_test.cpp
    )
    target_link_libraries(planck_apps_tests PRIVATE planck_apps_support)

    # The programs themselves, run as separate processes against each other.
    add_test(NAME apps_smoke
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/apps_smoke.sh $<TARGET_FILE_DIR:planckd>)
    set_tests_properties(apps_smoke PROPERTIES LABELS e2e TIMEOUT 120)
endif()
