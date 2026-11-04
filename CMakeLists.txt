cmake_minimum_required(VERSION 3.20)
project(hole_vr CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hole_vr SHARED
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi.def
    src/proxy/dxgi.rc
    src/core/log.cpp
    src/core/config.cpp
    src/core/ini_edit.cpp
    src/core/builtin_files.cpp
    src/core/memory.cpp
    src/core/perf.cpp
    src/core/process_metrics.cpp
    src/ue/names.cpp
    src/ue/objects.cpp
    src/ue/reflection.cpp
    src/ue/dumper.cpp
    src/ue/script_dump.cpp
    src/hooks/hooks.cpp
    src/vr/openxr.cpp
    src/vr/menu_study.cpp
    src/vr/dxgi_hooks.cpp
    src/vr/stereo.cpp
    src/vr/view_lead.cpp
    src/vr/view_lead_math.cpp
    src/vr/square_test.cpp
    src/vr/square_watch.cpp
    src/vr/square_watch_math.cpp
    src/vr/spectator.cpp
    src/vr/spectator_math.cpp
    src/vr/input.cpp
    src/vr/controls.cpp
    src/vr/comfort.cpp
    src/vr/hole_watch.cpp
    src/vr/comfort_math.cpp
    src/vr/vr_menu_model.cpp
    src/vr/vr_options.cpp
    src/vr/teleport.cpp
    src/vr/teleport_math.cpp
    src/vr/interact/slide.cpp
    src/vr/interact/holster.cpp
    src/vr/interact/grip_points.cpp
    src/vr/interact/rifle_carry.cpp
    src/vr/interact/touch_math.cpp
    src/vr/interact/touch.cpp
    src/vr/body/ik_math.cpp
    src/vr/body/hand_pose.cpp
    src/vr/body/skeleton.cpp
    src/vr/body/finger_solve.cpp
    src/vr/body/finger_rest.cpp
    src/vr/body/grip_record.cpp
    src/vr/body/grip_editor.cpp
    src/vr/body/body.cpp
    src/vr/body/hand_walls.cpp
    src/ue/process_event.cpp
    src/ue/call.cpp
    src/game/game.cpp
    src/game/gun.cpp
    src/game/weapon_custom.cpp
    src/game/weapon_lab.cpp
    src/vr/render_fixes.cpp
    src/vr/cvars.cpp
    src/vr/ui_panel.cpp
    src/vr/world_menu.cpp
    src/vr/wrist_hud.cpp
    src/vr/launch.cpp
    src/vr/workbench.cpp
    src/vr/drinks.cpp
    src/main.cpp
)

target_include_directories(hole_vr PRIVATE src third_party/openxr/include)
target_link_libraries(hole_vr PRIVATE dxgi d3d12 dxguid psapi)
# The game loads dxgi.dll from its own folder first, so we ship as dxgi.dll.
set_target_properties(hole_vr PROPERTIES OUTPUT_NAME "dxgi" PREFIX "")
target_compile_definitions(hole_vr PRIVATE __USE_MINGW_ANSI_STDIO=1 WIDL_EXPLICIT_AGGREGATE_RETURNS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# The build id (src/build_id.h): the git revision, regenerated at every build.
# -DHOLEVR_BUILD_ID=... overrides it (letters, digits, dot, underscore, + or -).
set(HOLEVR_BUILD_ID "" CACHE STRING "Build id override (default: the git revision)")
set(HOLEVR_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(hole_vr_build_id
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${HOLEVR_GENERATED_DIR}/generated_build_id.h
            -DOVERRIDE=${HOLEVR_BUILD_ID} -P ${CMAKE_SOURCE_DIR}/cmake/build_id.cmake
    BYPRODUCTS ${HOLEVR_GENERATED_DIR}/generated_build_id.h
    VERBATIM)
add_dependencies(hole_vr hole_vr_build_id)
target_include_directories(hole_vr PRIVATE ${HOLEVR_GENERATED_DIR})

# Built-in files (defaults/README.md): every file under defaults/ goes into the
# DLL and is written to a player's hole-vr folder when missing there.
file(GLOB_RECURSE HOLEVR_DEFAULT_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/defaults/*)
set(HOLEVR_BUILTIN_CPP ${HOLEVR_GENERATED_DIR}/generated_builtin_files.cpp)
add_custom_command(OUTPUT ${HOLEVR_BUILTIN_CPP}
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR}/defaults -DOUT=${HOLEVR_BUILTIN_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
    DEPENDS ${HOLEVR_DEFAULT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
    VERBATIM)
target_sources(hole_vr PRIVATE ${HOLEVR_BUILTIN_CPP})

if(MINGW)
    target_compile_options(hole_vr PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
    # Static runtime so players need no extra DLLs next to the game.
    target_link_options(hole_vr PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
else()
    target_compile_options(hole_vr PRIVATE /O2 /W4 /permissive- /EHsc)
    set_property(TARGET hole_vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# ---- Release dxgi.dll: the same DLL without its debug data ----
# build/dxgi.dll keeps its debug data (function names, line tables) so crash
# addresses can be read. Players get build/release/dxgi.dll, about 2 MB
# smaller; the part taken out is kept beside it as build/release/dxgi.debug
# (addr2line -e dxgi.debug reads crash addresses from it).
set(HOLEVR_RELEASE_DIR ${CMAKE_BINARY_DIR}/release)
set(HOLEVR_RELEASE_DLL ${HOLEVR_RELEASE_DIR}/dxgi.dll)
if(MINGW)
    if(NOT CMAKE_OBJCOPY)
        find_program(CMAKE_OBJCOPY NAMES x86_64-w64-mingw32-objcopy objcopy REQUIRED)
    endif()
    if(NOT CMAKE_STRIP)
        find_program(CMAKE_STRIP NAMES x86_64-w64-mingw32-strip strip REQUIRED)
    endif()
    add_custom_command(OUTPUT ${HOLEVR_RELEASE_DLL} ${HOLEVR_RELEASE_DIR}/dxgi.debug
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HOLEVR_RELEASE_DIR}
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:hole_vr> ${HOLEVR_RELEASE_DIR}/dxgi.debug
        COMMAND ${CMAKE_STRIP} --strip-all -o ${HOLEVR_RELEASE_DLL} $<TARGET_FILE:hole_vr>
        DEPENDS hole_vr
        VERBATIM)
else()
    # MSVC keeps debug data in a separate .pdb already.
    add_custom_command(OUTPUT ${HOLEVR_RELEASE_DLL}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HOLEVR_RELEASE_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hole_vr> ${HOLEVR_RELEASE_DLL}
        DEPENDS hole_vr
        VERBATIM)
endif()
add_custom_target(hole_vr_release ALL DEPENDS ${HOLEVR_RELEASE_DLL})

# ---- Installer: HOLE-VR-Setup.exe with the release dxgi.dll embedded ----
set(DXGI_DLL "${HOLEVR_RELEASE_DLL}")
set(SETUP_MANIFEST "${CMAKE_SOURCE_DIR}/installer/setup.manifest")
configure_file(installer/setup.rc.in ${CMAKE_BINARY_DIR}/setup.rc @ONLY)
add_executable(hole_vr_setup WIN32 installer/setup.cpp ${CMAKE_BINARY_DIR}/setup.rc)
add_dependencies(hole_vr_setup hole_vr_release)
set_source_files_properties(${CMAKE_BINARY_DIR}/setup.rc PROPERTIES
    OBJECT_DEPENDS "${DXGI_DLL};${SETUP_MANIFEST}")
target_include_directories(hole_vr_setup PRIVATE src)
set_target_properties(hole_vr_setup PROPERTIES OUTPUT_NAME "HOLE-VR-Setup")
target_compile_definitions(hole_vr_setup PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_link_libraries(hole_vr_setup PRIVATE comctl32 shell32 ole32 version advapi32 user32)
if(MINGW)
    target_compile_options(hole_vr_setup PRIVATE -O2 -Wall -Wextra -Wno-missing-field-initializers -municode)
    # -s: the installer ships to players, so no debug data in it either.
    target_link_options(hole_vr_setup PRIVATE -municode -static -static-libgcc -static-libstdc++ -s)
else()
    set_property(TARGET hole_vr_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    # setup.rc already embeds the supplied manifest, including Common Controls
    # and DPI settings; an additional linker-generated manifest has the same ID.
    target_link_options(hole_vr_setup PRIVATE /MANIFEST:NO)
endif()
