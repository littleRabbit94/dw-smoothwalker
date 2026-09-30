# dw_unit: the tests and the UE4SS-free product sources, built without the RE-UE4SS tree. One CTest per suite
# (`dw_unit <suite>`). Run from the repo root:
#   cmake -S . -B build-tests -DDW_BUILD_MOD=OFF -DDW_BUILD_TESTS=ON
#   cmake --build build-tests --config Debug
#   ctest --test-dir build-tests -C Debug --output-on-failure
set(DW_SRC "${PROJECT_SOURCE_DIR}/src")

add_executable(dw_unit
    main.cpp
    math_test.cpp
    curves_test.cpp
    wall_test.cpp
    follow_test.cpp
    pipeline_test.cpp
    authority_test.cpp
    ini_test.cpp
    presets_test.cpp
    store_test.cpp
    "${DW_SRC}/camera/authority.cpp"
    "${DW_SRC}/camera/guarded.cpp"
    "${DW_SRC}/camera/pipeline.cpp"
    "${DW_SRC}/smoothwalker/settings/ini.cpp"
    "${DW_SRC}/smoothwalker/settings/presets.cpp"
    "${DW_SRC}/smoothwalker/settings/store.cpp")
target_compile_features(dw_unit PRIVATE cxx_std_23)
target_include_directories(dw_unit PRIVATE "${DW_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}")
# store_test reads the shipped smoothwalker.ini and mod_settings.ini as fixtures.
target_compile_definitions(dw_unit PRIVATE DW_REPO="${PROJECT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(dw_unit PRIVATE /W4 /utf-8 /permissive-)
endif()

foreach(suite math curves wall follow pipeline authority ini presets store)
    add_test(NAME ${suite} COMMAND dw_unit ${suite})
endforeach()
