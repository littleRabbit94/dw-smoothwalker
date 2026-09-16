set(TARGET DWSmoothCam)
project(${TARGET})

add_library(${TARGET} SHARED "${CMAKE_CURRENT_SOURCE_DIR}/src/dllmain.cpp")
target_link_libraries(${TARGET} PUBLIC UE4SS)

# UE4SS loads Mods/<Name>/dlls/main.dll; the build lands in the repo's mod folder, deployed from there.
set(DW_MOD_DLLS "${CMAKE_CURRENT_SOURCE_DIR}/../../ue4ss/DWSmoothCam/dlls")
add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DW_MOD_DLLS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${TARGET}>" "${DW_MOD_DLLS}/main.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:${TARGET}>" "${DW_MOD_DLLS}/main.pdb"
)
