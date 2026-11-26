# cluau - C-syntax-flavoured Luau. The translator (`cluau_translator`) has
# no dependencies; the runtime and the checker behind `cluau check`
# (`cluau_runtime`, `cluau_check`) build against Luau (fetched, or
# CLUAU_LUAU_DIR's checkout). This file is the standalone build and what a
# host's CMake build (Kiln's external build) adds with add_subdirectory,
# where the runtime is off and cluau_check builds against CLUAU_LUAU_DIR's
# headers for a host that links its own Luau.
cmake_minimum_required(VERSION 3.21)
project(cluau CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A fetched archive's files are dated when they are extracted (CMake 3.24 and
# later; older versions keep the archive's dates), so that a new pin rebuilds
# what changed.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(PROJECT_IS_TOP_LEVEL)
    # The standalone build is an optimised tool unless a build type is given
    # (a multi-config generator picks it at build time instead).
    get_property(cluau_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT cluau_multi_config AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "The build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    endif()
    # With MSVC, the C and C++ runtime is linked in (/MT), in cluau and in
    # every library it builds, so that cluau.exe needs no Visual C++
    # redistributable. A host that embeds cluau chooses its own.
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_library(cluau_translator STATIC
    src/translate/Lexer.cpp
    src/translate/Translate.cpp
    src/convert/Convert.cpp
    src/translate/Format.cpp
)
target_include_directories(cluau_translator PUBLIC include PRIVATE src/translate)
# A host may link the translator into a shared library (a plugin, an editor
# module); it has no dependencies, so this costs nothing elsewhere.
set_target_properties(cluau_translator PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(CLUAU_LUAU_DIR "" CACHE PATH "A Luau checkout, 0.748 or later (the runtime's Luau; without the runtime, builds cluau_check)")

# The runtime (`cluau script.cluau`, the REPL, require, the standard
# modules, `cluau check`) runs on Luau's VM, compiler and analysis: CLUAU_LUAU_DIR's checkout when
# given, else Luau fetched at the pinned release. A host that embeds only the
# translator leaves it off (the default under add_subdirectory).
option(CLUAU_BUILD_RUNTIME "Build the script runtime (needs Luau)" ${PROJECT_IS_TOP_LEVEL})
# The Luau release cluau is built and tested with. CLUAU_LUAU_VERSION
# overrides it; left empty, a build directory follows the pinned release
# when cluau updates it (a cached default would keep the old one).
set(CLUAU_LUAU_PINNED "0.748")
# The pinned release's source archive, by its hash (a moved tag cannot
# change what is built); another version is fetched by its tag.
set(CLUAU_LUAU_PINNED_SHA256 "810b13d1480a7a2efa66fe356d7e551d2ba3548823a69a4df5a6597f2beb35e5")
set(CLUAU_LUAU_VERSION "" CACHE STRING "A Luau release to fetch instead of cluau's pinned one (without CLUAU_LUAU_DIR)")
if(CLUAU_LUAU_VERSION)
    set(cluau_luau_tag ${CLUAU_LUAU_VERSION})
else()
    set(cluau_luau_tag ${CLUAU_LUAU_PINNED})
endif()

# Adds a library cluau builds on (fetched with FetchContent, or DIR's
# checkout) with its options set to the NAME=VALUE pairs after SETTINGS. The
# settings are normal variables of this function, which the library's
# option() and set(CACHE) take in place of their defaults: none of them is
# written to the cache, and a host's own values (BUILD_SHARED_LIBS,
# BUILD_TESTING, ...) are the same afterwards. Sets <name>_SOURCE_DIR.
function(cluau_add_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "DIR" "SETTINGS")
    foreach(setting IN LISTS arg_SETTINGS)
        string(FIND "${setting}" "=" at)
        string(SUBSTRING "${setting}" 0 ${at} key)
        math(EXPR at "${at} + 1")
        string(SUBSTRING "${setting}" ${at} -1 value)
        set(${key} "${value}")
    endforeach()
    # The policies that make a library with an older CMake minimum take
    # those variables (option(), set(CACHE), find_*), and choose its MSVC
    # runtime from CMAKE_MSVC_RUNTIME_LIBRARY as cluau's own targets do.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0125 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    if(arg_DIR)
        add_subdirectory(${arg_DIR} ${CMAKE_BINARY_DIR}/${name} EXCLUDE_FROM_ALL)
        set(${name}_SOURCE_DIR ${arg_DIR} PARENT_SCOPE)
    else()
        FetchContent_MakeAvailable(${name})
        set(${name}_SOURCE_DIR ${${name}_SOURCE_DIR} PARENT_SCOPE)
        # Only what cluau links is built.
        set_property(DIRECTORY ${${name}_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
endfunction()

if(CLUAU_BUILD_RUNTIME)
    if(NOT TARGET Luau.VM)
        set(cluau_luau_settings LUAU_BUILD_CLI=OFF LUAU_BUILD_TESTS=OFF)
        if(CLUAU_LUAU_DIR)
            cluau_add_dependency(luau DIR ${CLUAU_LUAU_DIR} SETTINGS ${cluau_luau_settings})
            set(cluau_luau_source ${CLUAU_LUAU_DIR})
        else()
            include(FetchContent)
            if(CLUAU_LUAU_VERSION)
                FetchContent_Declare(luau
                    GIT_REPOSITORY https://github.com/luau-lang/luau.git
                    GIT_TAG ${cluau_luau_tag}
                    GIT_SHALLOW TRUE)
            else()
                FetchContent_Declare(luau
                    URL https://github.com/luau-lang/luau/archive/refs/tags/${CLUAU_LUAU_PINNED}.tar.gz
                    URL_HASH SHA256=${CLUAU_LUAU_PINNED_SHA256})
            endif()
            cluau_add_dependency(luau SETTINGS ${cluau_luau_settings})
            set(cluau_luau_source ${luau_SOURCE_DIR})
        endif()
    endif()
    # @std/http: libcurl (HTTP and HTTPS only) over mbedTLS, both fetched at
    # pinned releases and linked in, so the tool stays one file. The
    # certificates come from the system at run time (src/runtime/Http.cpp).
    option(CLUAU_HTTP "Build @std/http (fetches libcurl and mbedTLS)" ON)
    # @std/crypto on the same mbedTLS. TLS in @std/net is built with either
    # (it needs mbedTLS only), and @std/websocket needs @std/crypto.
    option(CLUAU_CRYPTO "Build @std/crypto (fetches mbedTLS)" ON)
    if(CLUAU_HTTP OR CLUAU_CRYPTO)
        include(FetchContent)
        # With its threading (cmake/mbedtls/cluau_config.h): transfers and
        # connections run on several threads at once. Whatever includes its
        # headers sees the same configuration through its targets.
        FetchContent_Declare(mbedtls
            URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.8/mbedtls-3.6.8.tar.bz2
            URL_HASH SHA256=bc54b818e87f49c0b3281dd85d24adaede0e63b4e822e3fbd49c005a38b9595b)
        # mbedTLS 3.6 asks for an old CMake minimum, which CMake 4 warns about
        # (CMAKE_POLICY_VERSION_MINIMUM).
        cluau_add_dependency(mbedtls SETTINGS
            MBEDTLS_USER_CONFIG_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls/cluau_config.h
            ENABLE_PROGRAMS=OFF ENABLE_TESTING=OFF MBEDTLS_FATAL_WARNINGS=OFF USE_SHARED_MBEDTLS_LIBRARY=OFF
            USE_STATIC_MBEDTLS_LIBRARY=ON GEN_FILES=OFF DISABLE_PACKAGE_CONFIG_AND_INSTALL=ON
            CMAKE_POLICY_VERSION_MINIMUM=3.10)
        find_package(Threads REQUIRED)
        foreach(library mbedcrypto mbedx509 mbedtls)
            target_include_directories(${library} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls) # threading_alt.h
            target_link_libraries(${library} PUBLIC Threads::Threads)
        endforeach()
    endif()
    if(CLUAU_HTTP)
        FetchContent_Declare(curl
            URL https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.gz
            URL_HASH SHA256=e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49)
        # No bundle path from the build machine: the run decides (it differs by
        # system). curl takes these two from the cache only, and removes them
        # from it again.
        set(CURL_CA_BUNDLE none CACHE STRING "" FORCE)
        set(CURL_CA_PATH none CACHE STRING "" FORCE)
        cluau_add_dependency(curl SETTINGS
            # curl finds this mbedTLS through these (its targets, not installed
            # files); its link test for DES cannot run before mbedTLS is built.
            MBEDTLS_INCLUDE_DIR=${mbedtls_SOURCE_DIR}/include MBEDTLS_LIBRARY=mbedtls MBEDX509_LIBRARY=mbedx509
            MBEDCRYPTO_LIBRARY=mbedcrypto HAVE_MBEDTLS_DES_CRYPT_ECB=1
            CURL_USE_MBEDTLS=ON CURL_USE_OPENSSL=OFF CURL_USE_SCHANNEL=OFF CURL_USE_WOLFSSL=OFF CURL_USE_GNUTLS=OFF
            BUILD_CURL_EXE=OFF BUILD_SHARED_LIBS=OFF BUILD_STATIC_LIBS=ON BUILD_TESTING=OFF
            BUILD_LIBCURL_DOCS=OFF BUILD_MISC_DOCS=OFF ENABLE_CURL_MANUAL=OFF CURL_DISABLE_INSTALL=ON
            CURL_USE_PKGCONFIG=OFF CURL_ZLIB=OFF CURL_BROTLI=OFF CURL_ZSTD=OFF USE_NGHTTP2=OFF USE_LIBIDN2=OFF
            CURL_USE_LIBPSL=OFF CURL_USE_LIBSSH2=OFF CURL_USE_LIBSSH=OFF CURL_USE_GSSAPI=OFF ENABLE_ARES=OFF
            PICKY_COMPILER=OFF CURL_LTO=OFF
            # HTTP and HTTPS only (the transfers allow no others either).
            CURL_DISABLE_DICT=ON CURL_DISABLE_FILE=ON CURL_DISABLE_FTP=ON CURL_DISABLE_GOPHER=ON CURL_DISABLE_IMAP=ON
            CURL_DISABLE_IPFS=ON CURL_DISABLE_LDAP=ON CURL_DISABLE_LDAPS=ON CURL_DISABLE_MQTT=ON CURL_DISABLE_POP3=ON
            CURL_DISABLE_RTSP=ON CURL_DISABLE_SMTP=ON CURL_DISABLE_TELNET=ON CURL_DISABLE_TFTP=ON
            CURL_DISABLE_WEBSOCKETS=ON)
    endif()

    # @std/sqlite: SQLite's amalgamation (one C file), fetched at a release
    # and built here, without loadable extensions.
    option(CLUAU_SQLITE "Build @std/sqlite (fetches SQLite)" ON)
    if(CLUAU_SQLITE)
        include(FetchContent)
        enable_language(C)
        FetchContent_Declare(sqlite
            URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
            URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30)
        FetchContent_MakeAvailable(sqlite)
        add_library(cluau_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
        target_include_directories(cluau_sqlite PUBLIC ${sqlite_SOURCE_DIR})
        target_compile_definitions(cluau_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION
            SQLITE_ENABLE_FTS5 SQLITE_ENABLE_MATH_FUNCTIONS SQLITE_DEFAULT_MEMSTATUS=0)
        find_package(Threads REQUIRED)
        target_link_libraries(cluau_sqlite PUBLIC Threads::Threads)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(cluau_sqlite PRIVATE -w) # its own code, not ours to warn about
        endif()
    endif()

    # @std/compress and @std/zip: miniz (deflate, zlib, zip in one C file),
    # fetched at a release and built here.
    option(CLUAU_COMPRESS "Build @std/compress and @std/zip (fetches miniz)" ON)
    if(CLUAU_COMPRESS)
        include(FetchContent)
        enable_language(C)
        FetchContent_Declare(miniz
            URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
            URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a)
        FetchContent_MakeAvailable(miniz) # the release is the one C file and its header
        add_library(cluau_miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
        target_include_directories(cluau_miniz PUBLIC ${miniz_SOURCE_DIR})
        # Files go through the runtime's own (UTF-8) paths; no zlib names.
        target_compile_definitions(cluau_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(cluau_miniz PRIVATE -w)
        endif()
    endif()

    # @std/regex: PCRE2 (Perl-compatible regular expressions), fetched at a
    # release and linked statically, 8-bit with Unicode, without its JIT.
    option(CLUAU_REGEX "Build @std/regex (fetches PCRE2)" ON)
    if(CLUAU_REGEX)
        include(FetchContent)
        FetchContent_Declare(pcre2
            URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.49/pcre2-10.49.tar.bz2
            URL_HASH SHA256=53c156e1ba416a20da8e65395daa132da0d80e76910424caca3fcdae7831d384)
        cluau_add_dependency(pcre2 SETTINGS
            PCRE2_BUILD_PCRE2_8=ON PCRE2_BUILD_PCRE2_16=OFF PCRE2_BUILD_PCRE2_32=OFF PCRE2_SUPPORT_UNICODE=ON
            PCRE2_SUPPORT_JIT=OFF PCRE2_BUILD_PCRE2GREP=OFF PCRE2_BUILD_TESTS=OFF PCRE2_SHOW_REPORT=OFF
            BUILD_SHARED_LIBS=OFF BUILD_STATIC_LIBS=ON PCRE2_STATIC_PIC=ON)
    endif()

    # The strict rules, over Luau's analysis.
    add_library(cluau_check STATIC src/check/Check.cpp)
    target_link_libraries(cluau_check PUBLIC cluau_translator Luau.Analysis)
    # The standard modules written in cluau, embedded as text.
    file(GLOB CLUAU_STD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/std/*.cluau)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cluau_std_sources.cpp
        COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cluau_std_sources.cpp
            -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/std -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_std.cmake
        DEPENDS ${CLUAU_STD_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_std.cmake
        COMMENT "Embedding the standard modules written in cluau")
    add_library(cluau_runtime STATIC src/runtime/Runtime.cpp src/runtime/Std.cpp src/runtime/Resolve.cpp src/runtime/Spawn.cpp
        src/runtime/Check.cpp src/runtime/Json.cpp src/runtime/LanguageService.cpp src/runtime/LspServer.cpp
        src/runtime/Debugger.cpp src/runtime/DapServer.cpp src/runtime/Profile.cpp src/runtime/Jobs.cpp src/runtime/Net.cpp src/runtime/Encoding.cpp src/runtime/Term.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/cluau_std_sources.cpp)
    target_include_directories(cluau_runtime PUBLIC include)
    target_link_libraries(cluau_runtime PUBLIC cluau_translator PRIVATE cluau_check Luau.VM Luau.Compiler Luau.Analysis Luau.CodeGen)
    if(WIN32)
        target_link_libraries(cluau_runtime PRIVATE ws2_32) # @std/net
    endif()
    if(CLUAU_COMPRESS)
        target_sources(cluau_runtime PRIVATE src/runtime/Compress.cpp)
        target_link_libraries(cluau_runtime PRIVATE cluau_miniz)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_COMPRESS)
    endif()
    if(CLUAU_SQLITE)
        target_sources(cluau_runtime PRIVATE src/runtime/Sqlite.cpp)
        target_link_libraries(cluau_runtime PRIVATE cluau_sqlite)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_SQLITE)
    endif()
    if(CLUAU_REGEX)
        target_sources(cluau_runtime PRIVATE src/runtime/Regex.cpp)
        target_link_libraries(cluau_runtime PRIVATE pcre2-8-static)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_REGEX PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC)
    endif()
    if(CLUAU_HTTP OR CLUAU_CRYPTO)
        target_sources(cluau_runtime PRIVATE src/runtime/Tls.cpp)
        target_link_libraries(cluau_runtime PRIVATE mbedtls mbedx509 mbedcrypto)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_MBEDTLS)
        if(WIN32)
            target_link_libraries(cluau_runtime PRIVATE crypt32 bcrypt) # the system's certificates, its random numbers
        endif()
    endif()
    if(CLUAU_CRYPTO)
        target_sources(cluau_runtime PRIVATE src/runtime/Crypto.cpp)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_CRYPTO)
    endif()
    if(CLUAU_HTTP)
        target_sources(cluau_runtime PRIVATE src/runtime/Http.cpp)
        target_link_libraries(cluau_runtime PRIVATE CURL::libcurl)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_HTTP)
        if(WIN32)
            target_link_libraries(cluau_runtime PRIVATE crypt32) # the system's certificates
        endif()
    endif()
    # The REPL's line editor (history, editing, completion): Luau's copy of
    # isocline, built here so that it switches the terminal's mode without
    # discarding input it has not read yet (TCSANOW, not TCSAFLUSH) and a
    # pasted block arrives whole. A host's own Luau targets: its isocline.
    if(cluau_luau_source AND EXISTS ${cluau_luau_source}/extern/isocline/src/isocline.c)
        enable_language(C)
        set(cluau_isocline ${CMAKE_BINARY_DIR}/cluau-isocline)
        file(COPY ${cluau_luau_source}/extern/isocline/src ${cluau_luau_source}/extern/isocline/include DESTINATION ${cluau_isocline})
        file(READ ${cluau_luau_source}/extern/isocline/src/tty.c cluau_tty)
        string(REPLACE "tcsetattr(tty->fd_in,TCSAFLUSH," "tcsetattr(tty->fd_in,TCSANOW," cluau_tty "${cluau_tty}")
        file(WRITE ${cluau_isocline}/tty.c.patched "${cluau_tty}")
        configure_file(${cluau_isocline}/tty.c.patched ${cluau_isocline}/src/tty.c COPYONLY)
        add_library(cluau_isocline STATIC ${cluau_isocline}/src/isocline.c)
        target_include_directories(cluau_isocline PUBLIC ${cluau_isocline}/include)
        if(NOT MSVC)
            target_compile_options(cluau_isocline PRIVATE -w)
        endif()
        target_link_libraries(cluau_runtime PRIVATE cluau_isocline)
        target_compile_definitions(cluau_runtime PRIVATE CLUAU_HAS_ISOCLINE)
    elseif(TARGET isocline)
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
    # Luau's parser for `cluau convert --verify`.
    target_link_libraries(cluau PRIVATE cluau_runtime Luau.Ast)
    target_compile_definitions(cluau PRIVATE CLUAU_HAS_RUNTIME)
    if(CLUAU_BUILD_TESTS)
        # The runtime's API as a host uses it (host modules, the sandbox).
        add_executable(cluau_runtime_tests tests/main.cpp tests/runtime_tests.cpp)
        target_link_libraries(cluau_runtime_tests PRIVATE cluau_runtime Luau.VM)
        target_include_directories(cluau_runtime_tests PRIVATE src/runtime)
        add_test(NAME cluau_runtime_tests COMMAND cluau_runtime_tests)
        # Fuzz targets (libFuzzer: clang), one per parser of untrusted input,
        # built with the sanitizers; run by hand (hamr's manual tier).
        option(CLUAU_FUZZ "Build the fuzz targets (clang's libFuzzer)" OFF)
        if(CLUAU_FUZZ)
            # Only the targets whose module this build has.
            set(cluau_fuzz_targets translate base64 json csv)
            if(CLUAU_COMPRESS)
                list(APPEND cluau_fuzz_targets gunzip inflate zip)
            endif()
            if(CLUAU_REGEX)
                list(APPEND cluau_fuzz_targets regex)
            endif()
            foreach(target ${cluau_fuzz_targets})
                add_executable(fuzz_${target} tests/fuzz/fuzz.cpp)
                target_compile_definitions(fuzz_${target} PRIVATE CLUAU_FUZZ_TARGET="${target}")
                target_link_libraries(fuzz_${target} PRIVATE cluau_runtime Luau.VM Luau.Compiler)
                target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
            endforeach()
        endif()
        # Each tests/scripts/<name>.cluau runs and its output is compared
        # with <name>.expected, and `cluau check` passes on it (or reports
        # what <name>.checked says); each tests/check/<name>.cluau is checked
        # only (tests/run_script.cmake).
        # A test of a module a build leaves out is left out with it: the
        # options each needs (all of them).
        set(cluau_test_needs_crypto CLUAU_CRYPTO)
        set(cluau_test_needs_web CLUAU_HTTP CLUAU_CRYPTO)
        set(cluau_test_needs_sqlite CLUAU_SQLITE)
        set(cluau_test_needs_regex CLUAU_REGEX)
        set(cluau_test_needs_compress CLUAU_COMPRESS)
        set(cluau_test_needs_term_url_tar CLUAU_COMPRESS)
        set(cluau_test_needs_edges CLUAU_REGEX CLUAU_COMPRESS CLUAU_CRYPTO)
        set(cluau_test_needs_web_edges CLUAU_CRYPTO)
        if(CLUAU_HTTP OR CLUAU_CRYPTO)
            set(cluau_has_tls ON)
        endif()
        set(cluau_test_needs_tls cluau_has_tls)
        file(GLOB CLUAU_SCRIPT_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/*.cluau)
        foreach(script ${CLUAU_SCRIPT_TESTS})
            get_filename_component(name ${script} NAME_WE)
            set(cluau_test_missing "")
            foreach(need IN LISTS cluau_test_needs_${name})
                if(NOT ${need})
                    set(cluau_test_missing ${need})
                endif()
            endforeach()
            if(cluau_test_missing)
                continue()
            endif()
            add_test(NAME script:${name}
                COMMAND ${CMAKE_COMMAND} -DCLUAU=$<TARGET_FILE:cluau> -DSCRIPT=${script}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_script.cmake)
            if(NOT name MATCHES "^repl_")
                add_test(NAME check:scripts/${name}
                    COMMAND ${CMAKE_COMMAND} -DCLUAU=$<TARGET_FILE:cluau> -DSCRIPT=${script} -DMODE=check
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_script.cmake)
            endif()
        endforeach()
        # The prompt in a pseudo-terminal (Ctrl-C, pasting, completion):
        # Linux and macOS.
        if(UNIX AND (TARGET cluau_isocline OR TARGET isocline))
            find_package(Python3 COMPONENTS Interpreter)
            if(Python3_Interpreter_FOUND)
                add_test(NAME repl:terminal
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl_terminal.py $<TARGET_FILE:cluau>)
                set_tests_properties(repl:terminal PROPERTIES TIMEOUT 120)
            endif()
        endif()
        # @std/http against a local HTTP and HTTPS server (tests/http/run.py).
        if(CLUAU_HTTP)
            find_package(Python3 COMPONENTS Interpreter)
            if(Python3_Interpreter_FOUND)
                add_test(NAME http:local
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http/run.py $<TARGET_FILE:cluau>)
                set_tests_properties(http:local PROPERTIES TIMEOUT 180)
            endif()
        endif()
        # THIRD_PARTY_NOTICES.md holds the licences of exactly the pinned
        # releases: a build that fetched all of them checks it.
        if(CLUAU_HTTP AND CLUAU_REGEX AND CLUAU_COMPRESS AND NOT CLUAU_LUAU_DIR AND NOT CLUAU_LUAU_VERSION
                AND DEFINED luau_SOURCE_DIR)
            add_test(NAME notices
                COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/THIRD_PARTY_NOTICES.md
                    -DCHECK=${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md -DLUAU=${luau_SOURCE_DIR}
                    -DCURL=${curl_SOURCE_DIR} -DMBEDTLS=${mbedtls_SOURCE_DIR} -DPCRE2=${pcre2_SOURCE_DIR}
                    -DMINIZ=${miniz_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/third_party_notices.cmake)
        endif()
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
# `#!/usr/bin/env cluau` line finds it), its documentation and the licences
# of the libraries it contains. A host that embeds cluau installs what it
# needs itself.
if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    install(TARGETS cluau RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md docs/syntax.md docs/scripting.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
