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
