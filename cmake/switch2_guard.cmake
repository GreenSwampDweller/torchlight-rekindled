# Switch 2: route every service request, mount and SD-card write of an executable through
# src/platform/switch/platform_guard.cpp, which refuses what a homebrew must never do on the
# console (its NAND, settings, installs, reports). --require-defined makes the link fail if the
# guard is not in the executable, so a build cannot quietly go without it.
#
#   tl_switch2_guard(<target>)                 the guard is already in a library the target links
#   tl_switch2_guard(<target> ADD_SOURCE)      compile the guard into the target itself
set(TL_SWITCH2_GUARD_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../src/platform/switch/platform_guard.cpp")
set(TL_SWITCH2_GUARD_WRAPPED
    smGetServiceWrapper smGetServiceOriginal fsdevMountDevice
    fsFsCreateFile fsFsDeleteFile fsFsCreateDirectory fsFsDeleteDirectory
    fsFsDeleteDirectoryRecursively fsFsCleanDirectoryRecursively
    fsFsRenameFile fsFsRenameDirectory fsFsOpenFile fsFsSetConcatenationFileAttribute)

function(tl_switch2_guard target)
    if(NOT NINTENDO_SWITCH2)
        return()
    endif()
    if("ADD_SOURCE" IN_LIST ARGN)
        target_sources(${target} PRIVATE "${TL_SWITCH2_GUARD_SOURCE}")
    endif()
    foreach(_tl_fn IN LISTS TL_SWITCH2_GUARD_WRAPPED)
        target_link_options(${target} PRIVATE "-Wl,--wrap=${_tl_fn}" "-Wl,--require-defined=__wrap_${_tl_fn}")
    endforeach()
endfunction()
