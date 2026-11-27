cmake_minimum_required(VERSION 3.25)

project(planck VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PLANCK_NATIVE "Tune for the build machine (-march=native)" ON)
option(PLANCK_BUILD_TESTS "Build the test suite (fetches GoogleTest if not installed)" ON)
option(PLANCK_BUILD_APPS "Build planckd, planck-client, planck-mdtap and planck-replay" ON)
option(PLANCK_BUILD_CAPI "Build libplanck.so, the engine behind a C interface" ON)
option(PLANCK_BUILD_BENCH "Build planck-bench and planck-loadgen (needs PLANCK_BUILD_APPS)" ON)
set(PLANCK_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined or thread")
set(PLANCK_CONTRACTS "auto" CACHE STRING "Contract mode: auto, off, assume, checked")
set_property(CACHE PLANCK_CONTRACTS PROPERTY STRINGS auto off assume checked)

# Flags shared by every planck target.
add_library(planck_flags INTERFACE)
target_compile_options(planck_flags INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion -Wshadow
    -fno-exceptions -fno-rtti
)
if(PLANCK_NATIVE)
    target_compile_options(planck_flags INTERFACE -march=native)
endif()

# Sanitizer flags on their own, so that code in other languages can be built
# to match. A program must be built with the sanitizers its libraries use.
add_library(planck_sanitize INTERFACE)
if(PLANCK_SANITIZE)
    target_compile_options(planck_sanitize INTERFACE -fsanitize=${PLANCK_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(planck_sanitize INTERFACE -fsanitize=${PLANCK_SANITIZE})
endif()
target_link_libraries(planck_flags INTERFACE planck_sanitize)

if(PLANCK_CONTRACTS STREQUAL "off")
    target_compile_definitions(planck_flags INTERFACE PLANCK_CONTRACT_MODE=0)
elseif(PLANCK_CONTRACTS STREQUAL "assume")
    target_compile_definitions(planck_flags INTERFACE PLANCK_CONTRACT_MODE=1)
elseif(PLANCK_CONTRACTS STREQUAL "checked")
    target_compile_definitions(planck_flags INTERFACE PLANCK_CONTRACT_MODE=2)
elseif(NOT PLANCK_CONTRACTS STREQUAL "auto")
    message(FATAL_ERROR "PLANCK_CONTRACTS must be auto, off, assume or checked")
endif()

add_library(planck_core STATIC
    src/core/contract.cpp
    src/core/status.cpp
)
add_library(planck::core ALIAS planck_core)
target_include_directories(planck_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(planck_core PUBLIC planck_flags)

find_package(Threads REQUIRED)

add_library(planck_sys STATIC
    src/sys/clock.cpp
    src/sys/cpu.cpp
    src/sys/error.cpp
    src/sys/memory.cpp
)
add_library(planck::sys ALIAS planck_sys)
target_link_libraries(planck_sys PUBLIC planck_core Threads::Threads)

add_library(planck_mem STATIC
    src/mem/arena.cpp
)
add_library(planck::mem ALIAS planck_mem)
target_link_libraries(planck_mem PUBLIC planck_sys)

add_library(planck_book STATIC
    src/book/ladder_bitmap.cpp
    src/book/order_book.cpp
    src/book/order_store.cpp
    src/book/types.cpp
)
add_library(planck::book ALIAS planck_book)
target_link_libraries(planck_book PUBLIC planck_mem)

# Header only.
add_library(planck_ring INTERFACE)
add_library(planck::ring ALIAS planck_ring)
target_link_libraries(planck_ring INTERFACE planck_mem)

add_library(planck_wire STATIC
    src/wire/codec.cpp
    src/wire/types.cpp
)
add_library(planck::wire ALIAS planck_wire)
target_link_libraries(planck_wire PUBLIC planck_core)

add_library(planck_engine STATIC
    src/engine/shard.cpp
)
add_library(planck::engine ALIAS planck_engine)
target_link_libraries(planck_engine PUBLIC planck_book planck_ring planck_wire)

add_library(planck_net STATIC
    src/net/address.cpp
    src/net/poller.cpp
    src/net/socket.cpp
)
add_library(planck::net ALIAS planck_net)
target_link_libraries(planck_net PUBLIC planck_sys)

add_library(planck_perf STATIC
    src/perf/histogram.cpp
)
add_library(planck::perf ALIAS planck_perf)
target_link_libraries(planck_perf PUBLIC planck_core)

add_library(planck_gateway STATIC
    src/gateway/gateway.cpp
)
add_library(planck::gateway ALIAS planck_gateway)
target_link_libraries(planck_gateway PUBLIC planck_engine planck_net planck_perf)

add_library(planck_feed STATIC
    src/feed/publisher.cpp
    src/feed/receiver.cpp
)
add_library(planck::feed ALIAS planck_feed)
target_link_libraries(planck_feed PUBLIC planck_engine planck_net)

add_library(planck_journal STATIC
    src/journal/format.cpp
    src/journal/reader.cpp
    src/journal/recorder.cpp
    src/journal/writer.cpp
)
add_library(planck::journal ALIAS planck_journal)
target_link_libraries(planck_journal PUBLIC planck_engine)

# Before any subdirectory, so that tests declared next to the code they
# check are registered too.
if(PLANCK_BUILD_TESTS)
    enable_testing()
endif()

if(PLANCK_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(PLANCK_BUILD_CAPI)
    add_subdirectory(capi)
endif()

if(PLANCK_BUILD_BENCH)
    if(PLANCK_BUILD_APPS)
        add_subdirectory(bench)
    else()
        message(STATUS "planck: benchmarks skipped, they need PLANCK_BUILD_APPS=ON")
    endif()
endif()

if(PLANCK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
