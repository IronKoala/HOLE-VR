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
    src/core/memory.cpp
    src/ue/names.cpp
    src/ue/objects.cpp
    src/ue/reflection.cpp
    src/ue/dumper.cpp
    src/hooks/hooks.cpp
    src/vr/openxr.cpp
    src/vr/dxgi_hooks.cpp
    src/vr/stereo.cpp
    src/vr/input.cpp
    src/ue/process_event.cpp
    src/ue/call.cpp
    src/game/game.cpp
    src/game/gun.cpp
    src/game/weapon_lab.cpp
    src/vr/render_fixes.cpp
    src/main.cpp
)

target_include_directories(hole_vr PRIVATE src third_party/openxr/include)
target_link_libraries(hole_vr PRIVATE dxgi d3d12 dxguid)
# The game loads dxgi.dll from its own folder first, so we ship as dxgi.dll.
set_target_properties(hole_vr PROPERTIES OUTPUT_NAME "dxgi" PREFIX "")
target_compile_definitions(hole_vr PRIVATE __USE_MINGW_ANSI_STDIO=1 WIDL_EXPLICIT_AGGREGATE_RETURNS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if(MINGW)
    target_compile_options(hole_vr PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
    # Static runtime so players need no extra DLLs next to the game.
    target_link_options(hole_vr PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
else()
    target_compile_options(hole_vr PRIVATE /O2 /W4 /permissive-)
    set_property(TARGET hole_vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# ---- Installer: HOLE-VR-Setup.exe with dxgi.dll embedded ----
set(DXGI_DLL "${CMAKE_BINARY_DIR}/dxgi.dll")
set(SETUP_MANIFEST "${CMAKE_SOURCE_DIR}/installer/setup.manifest")
configure_file(installer/setup.rc.in ${CMAKE_BINARY_DIR}/setup.rc @ONLY)
add_executable(hole_vr_setup WIN32 installer/setup.cpp ${CMAKE_BINARY_DIR}/setup.rc)
add_dependencies(hole_vr_setup hole_vr)
set_source_files_properties(${CMAKE_BINARY_DIR}/setup.rc PROPERTIES
    OBJECT_DEPENDS "${DXGI_DLL};${SETUP_MANIFEST}")
target_include_directories(hole_vr_setup PRIVATE src)
set_target_properties(hole_vr_setup PROPERTIES OUTPUT_NAME "HOLE-VR-Setup")
target_compile_definitions(hole_vr_setup PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_link_libraries(hole_vr_setup PRIVATE comctl32 shell32 ole32 version advapi32 user32)
if(MINGW)
    target_compile_options(hole_vr_setup PRIVATE -O2 -Wall -Wextra -Wno-missing-field-initializers -municode)
    target_link_options(hole_vr_setup PRIVATE -municode -static -static-libgcc -static-libstdc++)
else()
    set_property(TARGET hole_vr_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
