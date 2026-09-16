cmake_minimum_required(VERSION 3.22)
project(DWCppMods)

# UE4SS C++ mods link against UE4SS.dll, so the source must sit at the commit the installed loader was
# built from: Vercadi rc6 is 97b7e501 (docs/design.md, "The loader: Vercadi rc6"). Config: Game__Shipping__Win64.
# Not named UE4SS_SOURCE_DIR: project(UE4SS) caches that name as <src>/UE4SS, so a re-configure would
# add_subdirectory the UE4SS folder directly and fail with "Unknown CMake command organize_targets".
set(DW_RE_UE4SS_SOURCE_DIR "C:/path/to/RE-UE4SS-src" CACHE PATH "RE-UE4SS checkout at the installed commit")

add_subdirectory("${DW_RE_UE4SS_SOURCE_DIR}" RE-UE4SS)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/DWSmoothCam" DWSmoothCam)
