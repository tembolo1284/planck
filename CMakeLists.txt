cmake_minimum_required(VERSION 3.25)

project(planck VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PLANCK_NATIVE "Tune for the build machine (-march=native)" ON)
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
