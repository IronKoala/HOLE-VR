cmake_minimum_required(VERSION 3.20)
project(hole_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hole_vr SHARED
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi.def
    src/core/log.cpp
    src/core/config.cpp
    src/core/memory.cpp
    src/ue/names.cpp
    src/ue/objects.cpp
    src/ue/reflection.cpp
    src/ue/dumper.cpp
    src/main.cpp
)

target_include_directories(hole_vr PRIVATE src)
# The game loads dxgi.dll from its own folder first, so we ship as dxgi.dll.
set_target_properties(hole_vr PROPERTIES OUTPUT_NAME "dxgi" PREFIX "")
target_compile_definitions(hole_vr PRIVATE __USE_MINGW_ANSI_STDIO=1 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if(MINGW)
    target_compile_options(hole_vr PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    # Static runtime so players need no extra DLLs next to the game.
    target_link_options(hole_vr PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
else()
    target_compile_options(hole_vr PRIVATE /O2 /W4 /permissive-)
    set_property(TARGET hole_vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
