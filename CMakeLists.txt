cmake_minimum_required(VERSION 3.22)
project(DWSmoothCam)

# UE4SS C++ mods link against UE4SS.dll, so the source must sit at the commit the installed loader was
# built from: Vercadi rc6 is 97b7e501 (docs/design.md, "Loader: Vercadi rc6"). Config: Game__Shipping__Win64.
# Not named UE4SS_SOURCE_DIR: project(UE4SS) caches that name as <src>/UE4SS, so a re-configure would
# add_subdirectory the UE4SS folder directly and fail with "Unknown CMake command organize_targets".
set(DW_RE_UE4SS_SOURCE_DIR "" CACHE PATH "RE-UE4SS checkout at the installed commit")
if(NOT EXISTS "${DW_RE_UE4SS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set -DDW_RE_UE4SS_SOURCE_DIR=<RE-UE4SS checkout at 97b7e501>")
endif()

add_subdirectory("${DW_RE_UE4SS_SOURCE_DIR}" RE-UE4SS)

add_library(DWSmoothCam SHARED "${CMAKE_CURRENT_SOURCE_DIR}/src/dllmain.cpp")
target_link_libraries(DWSmoothCam PUBLIC UE4SS)

# UE4SS loads Mods/<Name>/dlls/main.dll; the build lands in this repo's mod folder, deployed from there.
set(DW_MOD_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/mod/dlls")
add_custom_command(TARGET DWSmoothCam POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DW_MOD_DLLS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:DWSmoothCam>" "${DW_MOD_DLLS}/main.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:DWSmoothCam>" "${DW_MOD_DLLS}/main.pdb"
)
