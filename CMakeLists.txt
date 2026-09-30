cmake_minimum_required(VERSION 3.22)
project(DWSmoothwalker)

option(DW_BUILD_MOD "the UE4SS mod DLL" ON)
option(DW_BUILD_TESTS "unit tests" OFF)

if(DW_BUILD_MOD)
    # UE4SS C++ mods link against UE4SS.dll, so the source must sit at the commit the installed loader was
    # built from: Vercadi rc6 is 97b7e501 (docs/design.md, "Loader: Vercadi rc6"). Config: Game__Shipping__Win64.
    # Not named UE4SS_SOURCE_DIR: project(UE4SS) caches that name as <src>/UE4SS, so a re-configure would
    # add_subdirectory the UE4SS folder directly and fail with "Unknown CMake command organize_targets".
    set(DW_RE_UE4SS_SOURCE_DIR "" CACHE PATH "RE-UE4SS checkout at the installed commit")
    if(NOT EXISTS "${DW_RE_UE4SS_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Set -DDW_RE_UE4SS_SOURCE_DIR=<RE-UE4SS checkout at 97b7e501>")
    endif()

    add_subdirectory("${DW_RE_UE4SS_SOURCE_DIR}" RE-UE4SS)

    # The camera core (src/camera) and Smoothwalker (src/smoothwalker) as separate translation units: Smoothwalker reaches
    # the core only through camera/api.hpp, and mod.cpp owns both (docs/design.md, "Core and processors").
    add_library(DWSmoothwalker SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/src/mod.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/camera/authority.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/camera/core.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/camera/guarded.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/camera/hook.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/camera/pipeline.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smoothwalker/smoothwalker.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smoothwalker/settings/ini.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smoothwalker/settings/presets.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smoothwalker/settings/store.cpp")
    target_link_libraries(DWSmoothwalker PUBLIC UE4SS)
    # Record the PDB by file name rather than absolute path. The name is literal because the Visual Studio
    # generator escapes the percent signs in %_PDB%.
    target_link_options(DWSmoothwalker PRIVATE "/PDBALTPATH:main.pdb")

    # UE4SS loads Mods/<Name>/dlls/main.dll; the build lands in this repo's mod folder, deployed from there.
    set(DW_MOD_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/mod/dlls")
    add_custom_command(TARGET DWSmoothwalker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DW_MOD_DLLS}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:DWSmoothwalker>" "${DW_MOD_DLLS}/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:DWSmoothwalker>" "${DW_MOD_DLLS}/main.pdb"
    )
endif()

# tests/unit: dw_unit over the UE4SS-free sources, one CTest per suite. Needs no RE-UE4SS tree.
if(DW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
