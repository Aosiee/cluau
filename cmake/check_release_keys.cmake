# Checks that the installers carry exactly the release keys in
# installer/release-keys.pub (which the build embeds for `cluau upgrade`):
#
#   cmake -DDIR=<source dir> -P cmake/check_release_keys.cmake

include(${CMAKE_CURRENT_LIST_DIR}/release_keys.cmake)

cluau_release_keys(${DIR}/installer/release-keys.pub expected)
foreach(installer install.sh install.ps1)
    cluau_release_keys(${DIR}/installer/${installer} actual)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "installer/${installer} carries the release keys [${actual}], "
            "but installer/release-keys.pub has [${expected}]: give it the same ones, in the same order")
    endif()
endforeach()
