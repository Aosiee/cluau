# A host that adds cluau with add_subdirectory, as an engine or an editor
# plugin does: adding cluau leaves the host's own settings as they were, and
# the translator links into the host's shared library. Run by the release
# workflow and by hamr:
#
#   cmake -S cmake/host -B <dir> [-DCLUAU_BUILD_RUNTIME=ON] && cmake --build <dir>
cmake_minimum_required(VERSION 3.21)
project(cluau_host CXX)

option(BUILD_SHARED_LIBS "The host's libraries are shared" ON)
include(CTest)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. cluau)

foreach(setting BUILD_SHARED_LIBS BUILD_TESTING)
    get_property(cached CACHE ${setting} PROPERTY VALUE)
    if(NOT ${setting} OR NOT cached)
        message(FATAL_ERROR "adding cluau changed the host's ${setting} (now ${${setting}}, cached ${cached})")
    endif()
endforeach()

add_library(cluau_host_plugin plugin.cpp)
target_link_libraries(cluau_host_plugin PRIVATE cluau_translator)
