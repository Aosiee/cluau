# cluau_release_keys(<file> <out>): the minisign public keys in a file, each
# a line of its own (around which spaces and a CR are ignored, so a
# checkout with CRLF line ends reads the same). The build embeds
# installer/release-keys.pub's for `cluau upgrade`; check_release_keys.cmake
# compares the installers' with them.
function(cluau_release_keys file out)
    file(STRINGS ${file} lines)
    set(keys "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "^RW[A-Za-z0-9+/]+=*$")
            list(APPEND keys "${line}")
        endif()
    endforeach()
    set(${out} "${keys}" PARENT_SCOPE)
endfunction()
