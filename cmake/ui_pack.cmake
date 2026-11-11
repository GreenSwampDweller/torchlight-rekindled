# The port's own interface pack, made by every build.
#
# `scripts/build_ui_pack.py` draws the art and writes the layouts (Pillow, no
# file of the game's): the interface a PC Pak.zip is drawn with when no
# console_ui/ is beside it. Every target carries it - beside the program on the
# desktop (`ui_pack/`), in the .nro's RomFS (`romfs:/ui_pack`), in the APK's
# assets (scripts/build_apk.py packs the build tree's `ui_pack/`) - so the build
# needs a Python 3 with Pillow and stops at configure without one.
#
# The Python is found once and cached as TL_PYTHON: the given one, else the
# first with Pillow among those on PATH and Windows' usual install folders
# (devkitPro's shell has no Python of its own, and no LOCALAPPDATA).

function(tl_find_ui_pack_python)
    set(_candidates "")
    if(TL_PYTHON)
        list(APPEND _candidates "${TL_PYTHON}")
    else()
        foreach(_name python3 python)
            find_program(_tl_found NAMES ${_name} NO_CACHE)
            if(_tl_found)
                list(APPEND _candidates "${_tl_found}")
            endif()
            unset(_tl_found)
        endforeach()
        # Both spellings of the drive: devkitPro's CMake is an MSYS2 program (/c/...).
        foreach(_root "C:" "/c")
            file(GLOB _tl_windows
                "${_root}/Users/*/AppData/Local/Programs/Python/Python3*/python.exe"
                "${_root}/Program Files/Python3*/python.exe"
                "${_root}/Python3*/python.exe")
            list(SORT _tl_windows ORDER DESCENDING)
            list(APPEND _candidates ${_tl_windows})
        endforeach()
    endif()
    foreach(_python IN LISTS _candidates)
        execute_process(COMMAND "${_python}" -c "import PIL"
            RESULT_VARIABLE _tl_result OUTPUT_QUIET ERROR_QUIET)
        if(_tl_result EQUAL 0)
            set(TL_PYTHON "${_python}" CACHE FILEPATH
                "Python 3 with Pillow, for scripts/build_ui_pack.py" FORCE)
            message(STATUS "Torchlight: the interface pack is made with ${_python}")
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "Torchlight: every build carries the port's interface pack, made by "
        "scripts/build_ui_pack.py, which needs Python 3 with Pillow. None was found"
        " (tried: ${_candidates}). Install it (python -m pip install pillow) or "
        "name one with -DTL_PYTHON=<path to python>.")
endfunction()

# Makes the pack in `out_dir` whenever the scripts change, from an empty folder
# (Windows keeps an older pack's folder case, and the devices match case), and
# makes `target` wait for it. Sets `<target>_UI_PACK_STAMP` in the caller: the
# file a packaging step depends on.
function(tl_add_ui_pack target out_dir)
    set(_stamp "${CMAKE_BINARY_DIR}/${target}_ui_pack.stamp")
    set(_scripts
        "${CMAKE_SOURCE_DIR}/scripts/build_ui_pack.py"
        "${CMAKE_SOURCE_DIR}/scripts/ui_pack_art.py"
        "${CMAKE_SOURCE_DIR}/scripts/ui_pack_front.py"
        "${CMAKE_SOURCE_DIR}/scripts/ui_pack_menu.py")
    add_custom_command(OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${out_dir}"
        COMMAND "${TL_PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/build_ui_pack.py" --out "${out_dir}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS ${_scripts}
        COMMENT "Making the interface pack in ${out_dir}"
        VERBATIM)
    add_custom_target(${target}_ui_pack ALL DEPENDS "${_stamp}")
    add_dependencies(${target} ${target}_ui_pack)
    set(${target}_UI_PACK_STAMP "${_stamp}" PARENT_SCOPE)
endfunction()
