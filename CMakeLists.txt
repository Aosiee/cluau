# cluau - C-syntax-flavoured Luau. The translator (`cluau_translator`) has
# no dependencies; the runtime and the checker behind `cluau check`
# (`cluau_runtime`, `cluau_check`) build against Luau (fetched, or
# CLUAU_LUAU_DIR's checkout). This file is the standalone build and what a
# host's CMake build (Kiln's external build) adds with add_subdirectory,
# where the runtime is off and cluau_check builds against CLUAU_LUAU_DIR's
# headers for a host that links its own Luau.
cmake_minimum_required(VERSION 3.16)
project(cluau CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cluau_translator STATIC
    src/translate/Lexer.cpp
    src/translate/Translate.cpp
    src/convert/Convert.cpp
)
target_include_directories(cluau_translator PUBLIC include PRIVATE src/translate)

set(CLUAU_LUAU_DIR "" CACHE PATH "A Luau checkout (the runtime's Luau; without the runtime, builds cluau_check)")

# The runtime (`cluau script.cluau`, the REPL, require, the standard
# modules, `cluau check`) runs on Luau's VM, compiler and analysis: CLUAU_LUAU_DIR's checkout when
# given, else Luau fetched at CLUAU_LUAU_TAG. A host that embeds only the
# translator leaves it off (the default under add_subdirectory).
option(CLUAU_BUILD_RUNTIME "Build the script runtime (needs Luau)" ${PROJECT_IS_TOP_LEVEL})
set(CLUAU_LUAU_TAG "0.688" CACHE STRING "The Luau release the runtime fetches (without CLUAU_LUAU_DIR)")
if(CLUAU_BUILD_RUNTIME)
    if(NOT TARGET Luau.VM)
        set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
        set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        if(CLUAU_LUAU_DIR)
            add_subdirectory(${CLUAU_LUAU_DIR} ${CMAKE_BINARY_DIR}/luau EXCLUDE_FROM_ALL)
        else()
            include(FetchContent)
            FetchContent_Declare(luau
                GIT_REPOSITORY https://github.com/luau-lang/luau.git
                GIT_TAG ${CLUAU_LUAU_TAG}
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(luau)
            # Only what the runtime links is built.
            set_property(DIRECTORY ${luau_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
        endif()
    endif()
    # The strict rules, over Luau's analysis.
    add_library(cluau_check STATIC src/check/Check.cpp)
    target_link_libraries(cluau_check PUBLIC cluau_translator Luau.Analysis)
    add_library(cluau_runtime STATIC src/runtime/Runtime.cpp src/runtime/Std.cpp src/runtime/Resolve.cpp src/runtime/Spawn.cpp
        src/runtime/Check.cpp)
    target_include_directories(cluau_runtime PUBLIC include)
    target_link_libraries(cluau_runtime PUBLIC cluau_translator PRIVATE cluau_check Luau.VM Luau.Compiler Luau.Analysis)
    if(TARGET isocline) # Luau's line editor: the REPL's history and editing
        target_link_libraries(cluau_runtime PRIVATE isocline)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_ISOCLINE)
    endif()
endif()

# Without the runtime, the strict rules alone build against a Luau source
# tree's headers (a host links its own Luau Analysis).
if(NOT TARGET cluau_check AND CLUAU_LUAU_DIR)
    add_library(cluau_check STATIC src/check/Check.cpp)
    target_include_directories(cluau_check PUBLIC include PRIVATE
        ${CLUAU_LUAU_DIR}/Analysis/include ${CLUAU_LUAU_DIR}/Ast/include ${CLUAU_LUAU_DIR}/Common/include
        ${CLUAU_LUAU_DIR}/EqSat/include ${CLUAU_LUAU_DIR}/Config/include)
    target_link_libraries(cluau_check PUBLIC cluau_translator)
endif()

option(CLUAU_BUILD_TESTS "Build the translator's tests" ${PROJECT_IS_TOP_LEVEL})
if(CLUAU_BUILD_TESTS)
    enable_testing()
    add_executable(cluau_tests tests/main.cpp tests/translate_tests.cpp tests/convert_tests.cpp)
    target_link_libraries(cluau_tests PRIVATE cluau_translator)
    add_test(NAME cluau_tests COMMAND cluau_tests)
endif()

# The command line: `cmake --build build --target cluau`.
add_executable(cluau tools/cluau_cli.cpp)
target_link_libraries(cluau PRIVATE cluau_translator)
if(CLUAU_BUILD_RUNTIME)
    target_link_libraries(cluau PRIVATE cluau_runtime)
    target_compile_definitions(cluau PRIVATE CLUAU_HAS_RUNTIME)
    if(CLUAU_BUILD_TESTS)
        # Each tests/scripts/<name>.cluau runs and its output is compared
        # with <name>.expected, and `cluau check` passes on it (or reports
        # what <name>.checked says); each tests/check/<name>.cluau is checked
        # only (tests/run_script.cmake).
        file(GLOB CLUAU_SCRIPT_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/*.cluau)
        foreach(script ${CLUAU_SCRIPT_TESTS})
            get_filename_component(name ${script} NAME_WE)
            add_test(NAME script:${name}
                COMMAND ${CMAKE_COMMAND} -DCLUAU=$<TARGET_FILE:cluau> -DSCRIPT=${script}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_script.cmake)
            if(NOT name MATCHES "^repl_")
                add_test(NAME check:scripts/${name}
                    COMMAND ${CMAKE_COMMAND} -DCLUAU=$<TARGET_FILE:cluau> -DSCRIPT=${script} -DMODE=check
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_script.cmake)
            endif()
        endforeach()
        file(GLOB CLUAU_CHECK_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/check/*.cluau)
        foreach(script ${CLUAU_CHECK_TESTS})
            get_filename_component(name ${script} NAME_WE)
            add_test(NAME check:${name}
                COMMAND ${CMAKE_COMMAND} -DCLUAU=$<TARGET_FILE:cluau> -DSCRIPT=${script} -DMODE=check
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_script.cmake)
        endforeach()
    endif()
endif()

# `cmake --install build [--prefix <dir>]`: the tool (on PATH, so a script's
# `#!/usr/bin/env cluau` line finds it) and its documentation. A host that
# embeds cluau installs what it needs itself.
if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    install(TARGETS cluau RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES README.md LICENSE docs/syntax.md docs/scripting.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
