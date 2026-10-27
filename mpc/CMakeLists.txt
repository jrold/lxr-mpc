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
# Two libraries: voltkit-engine (the whole LXR-02 firmware on its virtual
# board, loaded once per instance, see EngineLoader.h) and the plug-in itself.

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

# ---- the engine: the whole LXR V2 firmware on VOLTKIT's virtual board
# (engine/volt_machine.h). The firmware's own sources, compiled unmodified but
# for the VOLTKIT changes; board/ is the STM32, its HAL and the panel.
set(FWR "${ROOT}/firmware")
set(BOARD "${ROOT}/board")
set(FW_SOURCES
    Core/Src/main.c Core/Src/stm32f7xx_it.c Core/Src/stm32f7xx_hal_msp.c Core/Src/system_stm32f7xx.c Core/Src/flash_if.c
    Core/Src/IO/adcPots.c Core/Src/IO/DIO.c Core/Src/IO/uart.c
    Core/Src/delay.c Core/Src/lcd.c Core/Src/encoder.c Core/Src/buttonHandler.c Core/Src/ledHandler.c Core/Src/graphic.c
    Core/Src/frontPanelParser.c Core/Src/Parameters.c
    Core/Src/menu/Cc2Text.c Core/Src/menu/copyClearTools.c Core/Src/menu/menu.c Core/Src/menu/screensaver.c
    Core/Src/Preset/initKit.c Core/Src/Preset/presetManager.c
    Core/Src/Soundengine/AudioCodecManager.c Core/Src/Soundengine/cs4344_cs5343.c Core/Src/Soundengine/FIFO.c
    Core/Src/Soundengine/FxBus.c Core/Src/Soundengine/Soundengine.c Core/Src/Soundengine/TriggerOut.c
    Core/Src/Soundengine/USB/usb_manager.c
    Middlewares/Third_Party/FatFs/src/diskio.c Middlewares/Third_Party/FatFs/src/ff.c
    Middlewares/Third_Party/FatFs/src/ff_gen_drv.c Middlewares/Third_Party/FatFs/src/option/syscall.c
    FATFS/App/fatfs.c FATFS/Target/bsp_driver_sd.c FATFS/Target/fatfs_platform.c FATFS/Target/sd_diskio.c
    Middlewares/JS/STM32_USB_Device_Library/Core/Src/usbd_core.c
    Middlewares/JS/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c
    Middlewares/JS/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c
    Middlewares/JS/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c
    USB_DEVICE_MIDI/App/usb_device.c USB_DEVICE_MIDI/App/usbd_cdc_if.c USB_DEVICE_MIDI/App/usbd_desc.c
    USB_DEVICE_MIDI/Target/usbd_conf.c)
file(GLOB FW_GLOBBED RELATIVE "${FWR}"
    "${FWR}/Core/Src/Soundengine/DSPAudio/*.c" "${FWR}/Core/Src/Soundengine/DSPAudio/FX/*.c"
    "${FWR}/Core/Src/Soundengine/DSPAudio/FX/VerbTools/*.c" "${FWR}/Core/Src/Soundengine/MIDI/*.c"
    "${FWR}/Core/Src/Soundengine/Sequencer/*.c")
list(APPEND FW_SOURCES ${FW_GLOBBED})
list(TRANSFORM FW_SOURCES PREPEND "${FWR}/")

add_library(voltkit_engine SHARED ${FW_SOURCES} ${BOARD}/board.c ${BOARD}/hal.c ${ROOT}/engine/volt_panel.c)
target_compile_definitions(voltkit_engine PRIVATE VOLTKIT STM32F765xx USE_HAL_DRIVER)
set_source_files_properties(${FWR}/Core/Src/main.c PROPERTIES COMPILE_DEFINITIONS
    "main=lxr_firmware_main;_itcm_text_end=_itcm_text_start;FIRMWARE_VERSION=\" 1.7\"")
# board/include first: its stm32f7xx.h and core_cm7.h stand in for ST's and ARM's
target_include_directories(voltkit_engine PRIVATE
    "${BOARD}/include" "${BOARD}" "${ROOT}/engine" "${FWR}/Core/Inc"
    "${FWR}/Drivers/STM32F7xx_HAL_Driver/Inc" "${FWR}/Drivers/STM32F7xx_HAL_Driver/Inc/Legacy"
    "${FWR}/Drivers/CMSIS/Device/ST/STM32F7xx/Include"
    "${FWR}/Middlewares/Third_Party/FatFs/src" "${FWR}/FATFS/App" "${FWR}/FATFS/Target"
    "${FWR}/Middlewares/JS/STM32_USB_Device_Library/Core/Inc" "${FWR}/Middlewares/JS/STM32_USB_Device_Library/Class/CDC/Inc"
    "${FWR}/USB_DEVICE_MIDI/App" "${FWR}/USB_DEVICE_MIDI/Target")
# -w: the firmware's own warnings are upstream's. Its flags (-O3, fast math),
# the ARM GCC ABI's unsigned char and short enums, and, as the STM32 does,
# saturating float-to-integer conversions (-fno-strict-float-cast-overflow)
# and wrapping signed arithmetic (-fwrapv).
target_compile_options(voltkit_engine PRIVATE -std=gnu11 -O3 -w -ffast-math -freciprocal-math -fomit-frame-pointer
    -funsigned-char -fshort-enums -fno-strict-float-cast-overflow -fwrapv)
if(APPLE)
    # Apple's clang defines __weak (Objective-C); the HAL's own __weak must win.
    target_compile_options(voltkit_engine PRIVATE -U__weak -Wno-deprecated-declarations
        -include "${BOARD}/include/volt_no_sections.h")
endif()
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

# the machine test links the engine's objects directly (no copy per instance)
add_executable(voltkit-machine-test ../test/MachineTest.c)
target_include_directories(voltkit-machine-test PRIVATE "${ROOT}/engine")
target_link_libraries(voltkit-machine-test PRIVATE voltkit_engine m)

# ---- the adapter
add_library(voltkit_adapter STATIC VoltkitMpc.cpp EngineLoader.cpp)
target_include_directories(voltkit_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${MPC_VST_DIR}/wrapper)
target_compile_options(voltkit_adapter PRIVATE -Wall -Wextra)
set_target_properties(voltkit_adapter PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
find_package(Threads REQUIRED)
target_link_libraries(voltkit_adapter PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# the adapter as the wrapper drives it (VOLTKIT_ENGINE, VOLTKIT_CARD)
add_executable(voltkit-adapter-test ../test/AdapterTest.cpp)
target_link_libraries(voltkit-adapter-test PRIVATE voltkit_adapter)

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
