# cluau - C-syntax-flavoured Luau. The translator (`cluau`) has no
# dependencies; the strict-rule checker (`cluau_check`) builds against the
# host's Luau (pass CLUAU_LUAU_DIR to build it here) and is tested in the
# hosts. This file is the standalone build and what a host's CMake build
# (Kiln's external build) adds with add_subdirectory.
cmake_minimum_required(VERSION 3.16)
project(cluau CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cluau STATIC
    src/translate/Lexer.cpp
    src/translate/Translate.cpp
    src/convert/Convert.cpp
)
target_include_directories(cluau PUBLIC include PRIVATE src/translate)

# The strict rules, when a Luau source tree is given (its Analysis headers).
set(CLUAU_LUAU_DIR "" CACHE PATH "A Luau checkout (builds cluau_check)")
if(CLUAU_LUAU_DIR)
    add_library(cluau_check STATIC src/check/Check.cpp)
    target_include_directories(cluau_check PUBLIC include PRIVATE
        ${CLUAU_LUAU_DIR}/Analysis/include ${CLUAU_LUAU_DIR}/Ast/include ${CLUAU_LUAU_DIR}/Common/include
        ${CLUAU_LUAU_DIR}/EqSat/include ${CLUAU_LUAU_DIR}/Config/include)
    target_link_libraries(cluau_check PUBLIC cluau)
endif()

option(CLUAU_BUILD_TESTS "Build the translator's tests" ${PROJECT_IS_TOP_LEVEL})
if(CLUAU_BUILD_TESTS)
    enable_testing()
    add_executable(cluau_tests tests/main.cpp tests/translate_tests.cpp tests/convert_tests.cpp)
    target_link_libraries(cluau_tests PRIVATE cluau)
    add_test(NAME cluau_tests COMMAND cluau_tests)
endif()

add_executable(cluau_cli tools/cluau_cli.cpp)
target_link_libraries(cluau_cli PRIVATE cluau)
set_target_properties(cluau_cli PROPERTIES OUTPUT_NAME cluau)
