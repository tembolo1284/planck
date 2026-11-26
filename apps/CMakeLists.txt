# Helpers shared by the command line programs.
add_library(planck_apps_support STATIC support.cpp)
target_include_directories(planck_apps_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(planck_apps_support PUBLIC planck::net)

# The venue.
add_executable(planckd planckd.cpp)
target_link_libraries(planckd PRIVATE planck_apps_support planck::gateway planck::feed)

# An order entry client to type at or script.
add_executable(planck-client planck_client.cpp)
target_link_libraries(planck-client PRIVATE planck_apps_support planck::wire)

# A market data viewer.
add_executable(planck-mdtap planck_mdtap.cpp)
target_link_libraries(planck-mdtap PRIVATE planck_apps_support planck::feed)
