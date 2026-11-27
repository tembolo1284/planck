# Timing helpers shared by the benchmark programs.
add_library(planck_bench_support STATIC harness.cpp)
target_include_directories(planck_bench_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(planck_bench_support PUBLIC planck::perf planck::sys planck_apps_support)

# The pieces of the engine, timed in process.
add_executable(planck-bench planck_bench.cpp)
target_link_libraries(planck-bench PRIVATE planck_bench_support planck::engine)

# A running planckd, timed from outside over TCP.
add_executable(planck-loadgen planck_loadgen.cpp)
target_link_libraries(planck-loadgen PRIVATE planck_bench_support planck::net planck::wire)
