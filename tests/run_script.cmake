# Runs one tests/scripts/<name>.cluau with the built tool and compares what
# it prints (stdout and stderr, then `exit: <status>`) with <name>.expected.
#   cmake -DCLUAU=<tool> -DSCRIPT=<file.cluau> [-DMODE=check] -P run_script.cmake
# A <name>.stdin file is the script's standard input; a script named
# repl_*.cluau is fed to the REPL (`cluau -i`) instead of run.
# -DMODE=check runs `cluau check <name>.cluau` instead and compares with
# <name>.checked (no such file: the check must pass with nothing to say); a
# first line `// cluau check --lint` passes the check those flags.
# -DUPDATE=ON rewrites the expected file.
get_filename_component(dir ${SCRIPT} DIRECTORY)
get_filename_component(name ${SCRIPT} NAME_WE)
set(input "")
set(expectedFile ${dir}/${name}.expected)
if(MODE STREQUAL "check")
    # A first line `// cluau check <flags>` gives the check its flags.
    file(STRINGS ${SCRIPT} first LIMIT_COUNT 1)
    set(flags "")
    if(first MATCHES "^// cluau check (.*)$")
        separate_arguments(flags UNIX_COMMAND "${CMAKE_MATCH_1}")
    endif()
    set(command ${CLUAU} check ${flags} ${name}.cluau)
    set(expectedFile ${dir}/${name}.checked)
elseif(name MATCHES "^repl_")
    set(command ${CLUAU} -i)
    set(input ${SCRIPT})
else()
    set(command ${CLUAU} ${name}.cluau alpha beta)
    if(EXISTS ${dir}/${name}.stdin)
        set(input ${dir}/${name}.stdin)
    endif()
endif()
if(input)
    execute_process(COMMAND ${command} WORKING_DIRECTORY ${dir} INPUT_FILE ${input}
        OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE status)
else()
    execute_process(COMMAND ${command} WORKING_DIRECTORY ${dir}
        OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE status)
endif()
string(REPLACE "\r\n" "\n" out "${out}")
set(out "${out}exit: ${status}\n")
set(clean "exit: 0\n")
if(UPDATE)
    if(MODE STREQUAL "check" AND out STREQUAL clean)
        file(REMOVE ${expectedFile})
    else()
        file(WRITE ${expectedFile} "${out}")
    endif()
    return()
endif()
if(MODE STREQUAL "check" AND NOT EXISTS ${expectedFile})
    set(expected "${clean}")
else()
    file(READ ${expectedFile} expected)
endif()
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT out STREQUAL expected)
    get_filename_component(expectedName ${expectedFile} NAME)
    message(FATAL_ERROR "FAIL ${name}.cluau: the output differs from ${expectedName}\n--- got:\n${out}--- expected:\n${expected}")
endif()
