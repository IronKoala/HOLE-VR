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
    src/core/sampler.cpp
    src/core/process_metrics.cpp
    src/core/gpu_share.cpp
    src/ue/names.cpp
    src/ue/objects.cpp
    src/ue/reflection.cpp
    src/ue/dumper.cpp
    src/ue/script_dump.cpp
    src/hooks/hooks.cpp
    src/vr/openxr.cpp
    src/vr/menu_study.cpp
    src/vr/dxgi_hooks.cpp
    src/vr/gpu_timer.cpp
    src/vr/gpu_test.cpp
    src/vr/stereo.cpp
    src/vr/view_lead.cpp
    src/vr/view_lead_math.cpp
    src/vr/gun_walls_math.cpp
    src/vr/wall_shapes.cpp
    src/vr/square_test.cpp
    src/vr/square_watch.cpp
    src/vr/square_watch_math.cpp
    src/vr/spectator.cpp
    src/vr/spectator_math.cpp
    src/vr/input.cpp
    src/vr/controls.cpp
    src/vr/kick.cpp
    src/vr/comfort.cpp
    src/vr/hole_watch.cpp
    src/vr/watch.cpp
    src/vr/watch_math.cpp
    src/vr/watch_model.cpp
    src/vr/comfort_math.cpp
    src/vr/vr_menu_model.cpp
    src/vr/vr_options.cpp
    src/vr/teleport.cpp
    src/vr/teleport_math.cpp
    src/vr/interact/slide.cpp
    src/vr/interact/holster.cpp
    src/vr/interact/holster_visual.cpp
    src/vr/interact/offhand_snap.cpp
    src/vr/interact/holster_feedback.cpp
    src/vr/interact/holster_audio.cpp
    src/vr/interact/holster_audio_data.cpp
    src/vr/interact/holster_model.cpp
    src/vr/interact/grip_points.cpp
    src/vr/interact/rifle_carry.cpp
    src/vr/interact/touch_math.cpp
    src/vr/interact/touch.cpp
    src/vr/interact/magwell_zone.cpp
    src/vr/interact/shell_load.cpp
    src/vr/body/ik_math.cpp
    src/vr/body/arm_pose.cpp
    src/vr/body/arm_posture.cpp
    src/vr/body/body_calibration.cpp
    src/vr/body/elbow_calibration.cpp
    src/vr/body/functional_reach.cpp
    src/vr/body/calibration_guidance.cpp
    src/vr/body/wrist_calibration.cpp
    src/vr/body/torso_pose.cpp
    src/vr/body/leg_pose.cpp
    src/vr/body/leg_world.cpp
    src/vr/body/stair_pose.cpp
    src/vr/body/hand_pose.cpp
    src/vr/body/holster_pose.cpp
    src/vr/body/skeleton.cpp
    src/vr/body/finger_solve.cpp
    src/vr/body/joint_limits.cpp
    src/vr/body/reload_anim.cpp
    src/vr/body/finger_rest.cpp
    src/vr/body/finger_contact.cpp
    src/vr/body/finger_self.cpp
    src/vr/body/finger_world.cpp
    src/vr/body/finger_world_query.cpp
    src/vr/body/thumb_pose.cpp
    src/vr/body/hand_motion.cpp
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
    src/vr/audio_listener.cpp
    src/vr/cvars.cpp
    src/vr/ui_panel.cpp
    src/vr/world_menu.cpp
    src/vr/wrist_hud.cpp
    src/vr/launch.cpp
    src/vr/workbench.cpp
    src/vr/bench_vise.cpp
    src/vr/drinks.cpp
    src/main.cpp
)

target_include_directories(hole_vr PRIVATE src third_party/openxr/include)
target_link_libraries(hole_vr PRIVATE dxgi d3d12 dxguid psapi ole32)
# The game loads dxgi.dll from its own folder first, so we ship as dxgi.dll.
set_target_properties(hole_vr PROPERTIES OUTPUT_NAME "dxgi" PREFIX "")
add_custom_command(TARGET hole_vr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets/watch $<TARGET_FILE_DIR:hole_vr>/hole-vr/watch
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets/holster-sounds $<TARGET_FILE_DIR:hole_vr>/hole-vr/sounds/holster
    VERBATIM)
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

# Portable IK regression cases also run on the Windows build host.
option(HOLEVR_BUILD_TESTS "Build the body calibration and pose regression tests" OFF)
option(HOLEVR_BUILD_HAND_TESTS "Build hand regression tests with the body integration suites" OFF)
if(HOLEVR_BUILD_HAND_TESTS)
    set(HOLEVR_BUILD_TESTS ON)
endif()
if(HOLEVR_BUILD_TESTS)
    enable_testing()
    foreach(case arm_pose arm_posture leg_pose stair_pose functional_reach elbow_calibration body_calibration wrist_calibration calibration_guidance torso_pose)
        add_executable(test_${case} tests/test_${case}.cpp src/vr/body/${case}.cpp
                       src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp)
        target_include_directories(test_${case} PRIVATE src)
        if(MSVC)
            target_compile_options(test_${case} PRIVATE /W4 /EHsc)
        else()
            target_compile_options(test_${case} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME ${case} COMMAND test_${case})
    endforeach()
    target_sources(test_leg_pose PRIVATE src/vr/body/arm_pose.cpp src/vr/body/stair_pose.cpp)
    target_sources(test_arm_posture PRIVATE src/vr/body/arm_pose.cpp)
    target_sources(test_body_calibration PRIVATE src/vr/body/wrist_calibration.cpp src/vr/body/functional_reach.cpp src/vr/body/elbow_calibration.cpp)
    add_executable(test_elbow_rest_posture tests/test_elbow_rest_posture.cpp
                   src/vr/body/arm_posture.cpp src/vr/body/arm_pose.cpp
                   src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp)
    add_executable(test_support_twist tests/test_support_twist.cpp
                   src/vr/body/arm_pose.cpp src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp)
    add_executable(test_calibration_hold tests/test_calibration_hold.cpp
                   src/vr/body/body_calibration.cpp src/vr/body/wrist_calibration.cpp
                   src/vr/body/functional_reach.cpp src/vr/body/elbow_calibration.cpp src/vr/body/ik_math.cpp)
    foreach(case support_twist calibration_hold elbow_rest_posture)
        target_include_directories(test_${case} PRIVATE src)
        if(MSVC)
            target_compile_options(test_${case} PRIVATE /W4 /EHsc)
        else()
            target_compile_options(test_${case} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME ${case} COMMAND test_${case})
    endforeach()
    add_executable(test_body_integration tests/test_body_integration.cpp
                   src/vr/body/body_calibration.cpp src/vr/body/functional_reach.cpp src/vr/body/wrist_calibration.cpp src/vr/body/elbow_calibration.cpp src/vr/body/arm_pose.cpp src/vr/body/arm_posture.cpp
                   src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp)
    target_include_directories(test_body_integration PRIVATE src)
    add_test(NAME body_integration COMMAND test_body_integration)
    add_executable(test_wrist_calibration_integration tests/test_wrist_calibration_integration.cpp
                   src/vr/body/body_calibration.cpp src/vr/body/functional_reach.cpp src/vr/body/wrist_calibration.cpp src/vr/body/elbow_calibration.cpp
                   src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp)
    target_include_directories(test_wrist_calibration_integration PRIVATE src)
    add_test(NAME wrist_calibration_integration COMMAND test_wrist_calibration_integration)
    add_executable(test_body tests/test_body.cpp src/vr/body/ik_math.cpp
                   src/vr/body/hand_pose.cpp src/vr/body/grip_probe.cpp)
    add_executable(test_grip tests/test_grip.cpp src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp
                   src/vr/body/finger_solve.cpp src/vr/body/grip_record.cpp src/vr/body/grip_editor.cpp)
    target_sources(test_grip PRIVATE src/vr/body/holster_pose.cpp)
    add_executable(test_watch tests/test_watch.cpp src/vr/watch_math.cpp
                   src/vr/body/ik_math.cpp src/vr/body/skeleton.cpp)
    add_executable(test_watch_marshal tests/test_watch_marshal.cpp)
    add_executable(test_holster_visual tests/test_holster_visual.cpp src/vr/interact/holster_visual.cpp
                   src/vr/interact/holster_feedback.cpp src/vr/interact/holster.cpp)
    add_executable(test_holster_audio tests/test_holster_audio.cpp src/vr/interact/holster_audio_data.cpp)
    add_executable(test_vr_options tests/test_vr_options.cpp src/vr/vr_menu_model.cpp src/core/ini_edit.cpp)
    add_executable(test_menu_input tests/test_menu_input.cpp src/vr/interact/slide.cpp)
    add_executable(test_hand_ownership tests/test_hand_ownership.cpp)
    add_executable(test_workbench_rules tests/test_workbench_rules.cpp)
    add_executable(test_bench_vise tests/test_bench_vise.cpp)
    add_executable(test_bench_layout tests/test_bench_layout.cpp)
    add_executable(test_rifle_carry tests/test_rifle_carry.cpp src/vr/interact/rifle_carry.cpp)
    add_executable(test_vise_weapons tests/test_vise_weapons.cpp src/vr/interact/rifle_carry.cpp)
    add_executable(test_workbench_release tests/test_workbench_release.cpp src/vr/interact/rifle_carry.cpp src/vr/interact/holster.cpp)
    add_executable(test_attachment_transaction tests/test_attachment_transaction.cpp)
    add_executable(test_ammo_refresh tests/test_ammo_refresh.cpp)
    add_executable(test_workbench_bughunt tests/test_workbench_bughunt.cpp src/vr/interact/rifle_carry.cpp)
    add_executable(test_comfort tests/test_comfort.cpp src/vr/comfort_math.cpp)
    add_executable(test_kick tests/test_kick.cpp src/vr/kick.cpp)
    add_executable(test_turn tests/test_turn.cpp)
    add_executable(test_gun_walls tests/test_gun_walls.cpp src/vr/gun_walls_math.cpp)
    add_executable(test_gpu_share tests/test_gpu_share.cpp)
    add_executable(test_offhand_snap tests/test_offhand_snap.cpp src/vr/interact/offhand_snap.cpp)
    add_executable(test_magwell_zone tests/test_magwell_zone.cpp src/vr/interact/magwell_zone.cpp
                   src/vr/body/ik_math.cpp)
    add_executable(test_shell_load tests/test_shell_load.cpp src/vr/interact/shell_load.cpp)
    foreach(case body grip watch watch_marshal holster_visual holster_audio vr_options menu_input hand_ownership comfort kick turn workbench_rules bench_vise bench_layout rifle_carry vise_weapons workbench_release attachment_transaction ammo_refresh workbench_bughunt magwell_zone shell_load gpu_share offhand_snap gun_walls)
        target_include_directories(test_${case} PRIVATE src)
        if(MSVC)
            target_compile_options(test_${case} PRIVATE /EHsc)
        endif()
        add_test(NAME ${case} COMMAND test_${case})
    endforeach()
    if(MSVC)
        target_compile_options(test_body_integration PRIVATE /EHsc)
        target_compile_options(test_wrist_calibration_integration PRIVATE /EHsc)
    endif()
    if(WIN32)
        add_executable(test_calibration_tracking tests/test_calibration_tracking.cpp src/vr/input.cpp)
        target_include_directories(test_calibration_tracking PRIVATE src third_party/openxr/include)
        target_compile_definitions(test_calibration_tracking PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(test_calibration_tracking PRIVATE /EHsc)
        endif()
        add_test(NAME calibration_tracking COMMAND test_calibration_tracking)
    endif()
endif()

if(HOLEVR_BUILD_TESTS)
    foreach(case finger_rest joint_limits slide_pose)
        add_executable(test_${case} tests/test_${case}.cpp)
        target_include_directories(test_${case} PRIVATE src)
        if(MSVC)
            target_compile_options(test_${case} PRIVATE /W4 /EHsc)
        else()
            target_compile_options(test_${case} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME ${case} COMMAND test_${case})
    endforeach()
    foreach(case finger_rest joint_limits)
        target_sources(test_${case} PRIVATE src/vr/body/${case}.cpp src/vr/body/finger_solve.cpp
                       src/vr/body/skeleton.cpp src/vr/body/ik_math.cpp)
    endforeach()
    foreach(case finger_contact finger_self thumb_pose finger_world hand_motion hand_contact_pipeline)
        add_executable(test_${case} tests/test_${case}.cpp src/vr/body/ik_math.cpp)
        target_include_directories(test_${case} PRIVATE src)
        if(MSVC)
            target_compile_options(test_${case} PRIVATE /W4 /EHsc)
        else()
            target_compile_options(test_${case} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME ${case} COMMAND test_${case})
    endforeach()
    foreach(case finger_contact finger_self thumb_pose finger_world hand_contact_pipeline)
        target_sources(test_${case} PRIVATE src/vr/body/finger_solve.cpp src/vr/body/skeleton.cpp)
    endforeach()
    foreach(case finger_contact finger_self thumb_pose hand_contact_pipeline)
        target_sources(test_${case} PRIVATE src/vr/body/finger_contact.cpp src/vr/body/joint_limits.cpp)
    endforeach()
    foreach(case thumb_pose hand_contact_pipeline)
        target_sources(test_${case} PRIVATE src/vr/body/thumb_pose.cpp)
    endforeach()
    foreach(case finger_world hand_contact_pipeline)
        target_sources(test_${case} PRIVATE src/vr/body/finger_world_query.cpp)
    endforeach()
    target_sources(test_finger_self PRIVATE src/vr/body/finger_self.cpp)
    target_sources(test_hand_contact_pipeline PRIVATE src/vr/body/finger_self.cpp)
    target_sources(test_hand_motion PRIVATE src/vr/body/hand_motion.cpp)
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
    OBJECT_DEPENDS "${DXGI_DLL};${SETUP_MANIFEST};${CMAKE_SOURCE_DIR}/assets/watch/dial.png;${CMAKE_SOURCE_DIR}/assets/watch/timer-atlas.png;${CMAKE_SOURCE_DIR}/assets/holster-sounds/draw.wav;${CMAKE_SOURCE_DIR}/assets/holster-sounds/holster.wav;${CMAKE_SOURCE_DIR}/assets/holster-sounds/hood_open.wav;${CMAKE_SOURCE_DIR}/assets/holster-sounds/hood_close.wav")
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
