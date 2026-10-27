cmake_minimum_required(VERSION 3.22)

# VOLTKIT for MPC OS standalone (Open MPC VST2 wrapper). Usually driven by
# mpc/build.sh, which cross-compiles for the MPC Live III (aarch64 Linux) with
# zig and generates params.h and the touchscreen skin with mpc-vst-plugins.
#
#   MPC_VST_DIR         checkout of https://github.com/sd88me/mpc-vst-plugins
#   MPC_PARAMS_DIR      folder holding the params.h gen_vst.py wrote from vst.json
#   MPC_WRAPPER_SOURCE  vst2_wrap.c to build (build.sh passes a copy that also
#                       tells the engine about MPC's STOP)
#
# Two libraries: voltkit-engine (the LXR firmware's sound engine, loaded once
# per instance, see EngineLoader.h) and the plug-in itself.

project(voltkit_mpc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MPC_VST_DIR "" CACHE PATH "mpc-vst-plugins checkout (wrapper/vst2_wrap.c)")
set(MPC_PARAMS_DIR "" CACHE PATH "Folder with the generated params.h")
set(MPC_WRAPPER_SOURCE "" CACHE FILEPATH "vst2_wrap.c to build (default: the checkout's)")

if(NOT MPC_VST_DIR OR NOT EXISTS "${MPC_VST_DIR}/wrapper/engine.h")
    message(FATAL_ERROR "Set MPC_VST_DIR to a mpc-vst-plugins checkout (mpc/build.sh does).")
endif()

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(FW "${ROOT}/firmware/Core")

# ---- the engine: the firmware's sound code, compiled as it is (see engine/)
file(GLOB FW_DSP "${FW}/Src/Soundengine/DSPAudio/*.c"
                 "${FW}/Src/Soundengine/DSPAudio/FX/*.c"
                 "${FW}/Src/Soundengine/DSPAudio/FX/VerbTools/*.c")
add_library(voltkit_engine SHARED
    ${FW_DSP}
    ${FW}/Src/Soundengine/FxBus.c
    ${FW}/Src/Soundengine/MIDI/MidiParser.c
    ${FW}/Src/Soundengine/MIDI/MidiVoiceControl.c
    ${FW}/Src/Soundengine/MIDI/ParameterArray.c
    ${FW}/Src/Soundengine/MIDI/MidiMessages.c
    ${FW}/Src/menu/Cc2Text.c
    ${FW}/Src/Parameters.c
    ${FW}/Src/Preset/initKit.c
    ${ROOT}/engine/volt_engine.c
    ${ROOT}/engine/firmware_stubs.c
)
target_compile_definitions(voltkit_engine PRIVATE VOLTKIT)
# the shim first: it stands in for the STM32 HAL headers
target_include_directories(voltkit_engine PRIVATE ${ROOT}/engine/shim ${FW}/Inc)
# -w: the firmware's own warnings are upstream's; -O3 as the firmware builds.
# -fno-strict-float-cast-overflow: the firmware converts out-of-range floats
# to integers in many places and relies on the Cortex-M7 saturating them;
# this makes the compiler do the same (aarch64's own conversion saturates).
# -fwrapv likewise for its signed shifts and wrap-arounds.
target_compile_options(voltkit_engine PRIVATE -O3 -w -ffp-contract=off -fno-strict-float-cast-overflow -fwrapv)
set_target_properties(voltkit_engine PROPERTIES
    PREFIX ""
    OUTPUT_NAME "voltkit-engine"
    C_VISIBILITY_PRESET hidden)
target_link_libraries(voltkit_engine PRIVATE m)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(voltkit_engine PRIVATE
        -Wl,--no-undefined
        -Wl,-Bsymbolic
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/engine-exports.map
        -s)
elseif(APPLE)
    set_target_properties(voltkit_engine PROPERTIES PREFIX "lib" SUFFIX ".dylib")

endif()

# ---- the adapter
add_library(voltkit_adapter STATIC VoltkitMpc.cpp EngineLoader.cpp)
target_include_directories(voltkit_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${MPC_VST_DIR}/wrapper)
target_compile_options(voltkit_adapter PRIVATE -Wall -Wextra)
set_target_properties(voltkit_adapter PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
find_package(Threads REQUIRED)
target_link_libraries(voltkit_adapter PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_executable(voltkit-mpc-test ../test/EngineTest.cpp)
target_link_libraries(voltkit-mpc-test PRIVATE voltkit_adapter)
add_dependencies(voltkit-mpc-test voltkit_engine)
target_compile_definitions(voltkit-mpc-test PRIVATE VOLTKIT_TEST_ENGINE="$<TARGET_FILE:voltkit_engine>")

add_executable(voltkit-fuzz ../test/Fuzz.c)
target_link_libraries(voltkit-fuzz PRIVATE ${CMAKE_DL_LIBS} m)
add_dependencies(voltkit-fuzz voltkit_engine)
target_compile_definitions(voltkit-fuzz PRIVATE VOLTKIT_TEST_ENGINE="$<TARGET_FILE:voltkit_engine>")

if(MPC_PARAMS_DIR)
    if(NOT MPC_WRAPPER_SOURCE)
        set(MPC_WRAPPER_SOURCE ${MPC_VST_DIR}/wrapper/vst2_wrap.c)
    endif()
    add_library(voltkit_mpc SHARED ${MPC_WRAPPER_SOURCE})
    set_source_files_properties(${MPC_WRAPPER_SOURCE} PROPERTIES COMPILE_OPTIONS "-std=gnu11")
    target_include_directories(voltkit_mpc PRIVATE ${MPC_PARAMS_DIR} ${MPC_VST_DIR}/wrapper)
    target_link_libraries(voltkit_mpc PRIVATE voltkit_adapter)
    set_target_properties(voltkit_mpc PROPERTIES
        PREFIX ""
        OUTPUT_NAME "voltkit"
        SUFFIX ".so"
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Only VSTPluginMain is exported; an unresolved symbol must fail
        # here, not crash MPC when it loads the plug-in.
        target_link_options(voltkit_mpc PRIVATE
            -Wl,--no-undefined
            -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map
            -s)
    endif()
endif()
