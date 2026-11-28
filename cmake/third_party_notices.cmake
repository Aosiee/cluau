# Writes THIRD_PARTY_NOTICES.md, the licences of the libraries the `cluau`
# tool links in, from their own licence files in the fetched sources:
#
#   cmake -DOUT=THIRD_PARTY_NOTICES.md -DLUAU=<luau source> -DCURL=<curl source>
#         -DMBEDTLS=<mbedtls source> -DPCRE2=<pcre2 source> -DMINIZ=<miniz source>
#         -P cmake/third_party_notices.cmake
#
# (the sources are build/_deps/<name>-src after a default build). With
# -DCHECK=<file>, it writes OUT and then fails when <file> differs from it:
# the test that keeps the repository's copy in step with the pinned releases.

foreach(input OUT LUAU CURL MBEDTLS PCRE2 MINIZ)
    if(NOT ${input})
        message(FATAL_ERROR "third_party_notices.cmake needs -D${input}=...")
    endif()
endforeach()

# A licence file's text, with LF line ends and no trailing spaces.
function(read_licence path out)
    if(NOT EXISTS ${path})
        message(FATAL_ERROR "no licence file at ${path}")
    endif()
    file(READ ${path} text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(REGEX REPLACE "[ \t]+\n" "\n" text "${text}")
    string(STRIP "${text}" text)
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The comment block before a source file's `SPDX-License-Identifier: <id>`
# line, without its comment marks.
function(read_source_notice path id out)
    read_licence(${path} text)
    string(FIND "${text}" "SPDX-License-Identifier: ${id}" end)
    if(end LESS 0)
        message(FATAL_ERROR "${path} has no ${id} notice")
    endif()
    string(SUBSTRING "${text}" 0 ${end} text)
    string(FIND "${text}" "/*" start REVERSE)
    math(EXPR start "${start} + 2")
    string(SUBSTRING "${text}" ${start} -1 text)
    # Twice: a mark on an empty line ends with the newline the next one starts with.
    string(REPLACE "\n *\n" "\n\n" text "${text}")
    string(REPLACE "\n *\n" "\n\n" text "${text}")
    string(REPLACE "\n * " "\n" text "${text}")
    string(STRIP "${text}" text)
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

read_licence(${LUAU}/LICENSE.txt luau)
read_licence(${LUAU}/lua_LICENSE.txt lua)
read_licence(${LUAU}/extern/isocline/LICENSE isocline)
read_licence(${CURL}/COPYING curl)
read_source_notice(${CURL}/lib/curlx/inet_pton.c ISC curl_inet_pton)
read_source_notice(${CURL}/lib/curlx/inet_ntop.c ISC curl_inet_ntop)
read_licence(${MBEDTLS}/LICENSE mbedtls)
read_licence(${PCRE2}/LICENCE.md pcre2)
read_licence(${MINIZ}/LICENSE miniz)

set(fence "```")
file(WRITE ${OUT} "# Third-party notices

The `cluau` command-line tool (the release downloads, the Debian and RPM
packages, and a default build from source) contains the libraries below,
each under its own licence. cluau's own licence is in [`LICENSE`](LICENSE).
A build that leaves out one of the optional modules (README.md, Building)
leaves out its library too. The translator library alone
(`cluau_translator`) contains none of them.

This file is generated from the libraries' own licence files by
`cmake/third_party_notices.cmake`; a test fails when it is out of date.

## Luau

The VM, compiler, type checker and native code generator scripts run on.
<https://github.com/luau-lang/luau>

${fence}text
${luau}
${fence}

Luau is derived from Lua 5.1:

${fence}text
${lua}
${fence}

## isocline

The REPL's line editor (Luau's copy). <https://github.com/daanx/isocline>

${fence}text
${isocline}
${fence}

## libcurl

`@std/http`'s transfers. <https://curl.se>

${fence}text
${curl}
${fence}

libcurl's own `inet_pton` and `inet_ntop`, used on a system that has none,
come from BIND:

${fence}text
${curl_inet_pton}
${fence}

${fence}text
${curl_inet_ntop}
${fence}

## Mbed TLS

TLS for `@std/http` and `@std/net`, and `@std/crypto`.
<https://github.com/Mbed-TLS/mbedtls>. cluau uses it under
the Apache License 2.0, one of the two licences it is offered under.

${fence}text
${mbedtls}
${fence}

## PCRE2

`@std/regex`. <https://github.com/PCRE2Project/pcre2>. cluau builds it
without its just-in-time compiler.

${fence}text
${pcre2}
${fence}

## SQLite

`@std/sqlite`. <https://sqlite.org>. SQLite is in the public domain
(<https://sqlite.org/copyright.html>): it needs no notice, and is named
here so that the list is complete.

## Monocypher

Ed25519 and BLAKE2b, with which `cluau upgrade` checks a release's minisign
signature. <https://monocypher.org>. Monocypher is offered under CC0 1.0 or
the 2-clause BSD licence; cluau takes it under CC0, which dedicates it to
the public domain: it needs no notice, and is named here so that the list
is complete.

## miniz

`@std/compress`, `@std/zip`, and gzip in `@std/tar`.
<https://github.com/richgel999/miniz>

${fence}text
${miniz}
${fence}
")

if(CHECK)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol ${CHECK} ${OUT} RESULT_VARIABLE differs)
    if(differs)
        message(FATAL_ERROR "${CHECK} does not match the licences of the libraries this build fetched. "
            "The up-to-date text is in ${OUT}: copy it over ${CHECK}.")
    endif()
endif()
