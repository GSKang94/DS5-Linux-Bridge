# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Audio pipeline is real-time and crackles under -O0. Default to Release;
# override explicitly with -DCMAKE_BUILD_TYPE=Debug only when debugging.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if (WIN32)
    set(USERHOME $ENV{USERPROFILE})
else ()
    set(USERHOME $ENV{HOME})
endif ()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif ()
# ====================================================================================

if (PICO_W_BUILD)
        set(PICO_BOARD pico_w CACHE STRING "Board type")
        set(PICO_RP2350 OFF CACHE BOOL "Not using RP2350")
else ()
        set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif ()
#set(PICO_CYW43_SUPPORTED 1)
#set(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND 1)

# Diagnostics go over UART0 (GP0 TX, 115200 8N1), always available -- there is
# no USB-CDC serial. USB serial was removed because it perturbed the very USB
# timing it was meant to diagnose (and was unreliable). See README "Debugging".

# Onboard web config UI over a USB CDC-NCM network interface. Default ON.
option(ENABLE_WEBCONFIG "Serve the config web page over a USB network interface" ON)
option(ENABLE_VERBOSE "Enable Log Verbose" OFF)
if(ENABLE_VERBOSE)
    set(ENABLE_VERBOSE_VALUE 1)
else()
    set(ENABLE_VERBOSE_VALUE 0)
endif ()
add_compile_definitions(ENABLE_VERBOSE=${ENABLE_VERBOSE_VALUE})
option(ENABLE_DIAG "Enable HID diagnostic counters and timing probes" OFF)
if(ENABLE_DIAG)
    set(ENABLE_DIAG_VALUE 1)
else()
    set(ENABLE_DIAG_VALUE 0)
endif()
option(ENABLE_BATT_LED "Blink Pico LED when DualSense battery is low (<=10%)" ON)
if (ENABLE_BATT_LED)
        set(ENABLE_BATT_LED_VALUE 1)
else ()
        set(ENABLE_BATT_LED_VALUE 0)
endif ()
option(DISABLE_SPEAKER_PROC "Disable Speaker in controller" OFF)
if (DISABLE_SPEAKER_PROC)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        set(DISABLE_SPEAKER_PROC_VALUE 0)
endif ()
option(PICO_W_BUILD "Build for Rasberry Pi Pico W, audio is missing" OFF)
if (PICO_W_BUILD)
        set(CYW43_PIO_CLOCK_DIV_VALUE 3)
        set(SYS_CLOCK_KHZ_VALUE 200000)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        # Stock RP2350 clock. Selective RAM relocation (relocate_to_ram) keeps the
        # hot paths out of flash XIP, so no overclock is needed for stable audio +
        # 1000Hz polling. PIO div 2 matches the 150 MHz system clock.
        set(CYW43_PIO_CLOCK_DIV_VALUE 2)
        set(SYS_CLOCK_KHZ_VALUE 150000)
endif ()



# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ds5-bridge C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(ds5-bridge
        src/usb_descriptors.cpp
        src/main.cpp
        src/usb.cpp
        src/audio.cpp
        src/bt.cpp
        src/dse.cpp
        src/config.cpp
        src/cmd.cpp
        src/state_mgr.cpp
        src/ram_mem.c
)

# RAM-resident memcpy/memset/memmove (src/ram_mem.c). Must be built with
# -fno-builtin and -fno-tree-loop-distribute-patterns so the compiler does not
# lower the word-copy loops back into calls to memcpy/memset (self-recursion).
set_source_files_properties(src/ram_mem.c PROPERTIES COMPILE_OPTIONS
        "-fno-builtin;-fno-tree-loop-distribute-patterns")

if (ENABLE_WEBCONFIG)
    target_sources(ds5-bridge PRIVATE
            src/usb_net.cpp
            # TinyUSB's tiny DHCP server, used to hand the host an address on
            # the USB NCM link.
            ${PICO_TINYUSB_PATH}/lib/networking/dhserver.c
    )
    target_include_directories(ds5-bridge PRIVATE
            ${PICO_TINYUSB_PATH}/lib/networking
    )
    target_compile_definitions(ds5-bridge PRIVATE ENABLE_WEBCONFIG)
    target_link_libraries(ds5-bridge
            pico_unique_id
            pico_lwip_nosys
            pico_lwip_http
    )
endif ()

if (ENABLE_BATT_LED)
        target_sources(ds5-bridge PRIVATE src/battery_led.cpp)
endif ()


# Opus
#set(OPUS_FIXED_POINT ON)
add_subdirectory(lib/opus)

# Move libopus to RAM via .time_critical sections. libopus is built by
# add_subdirectory(lib/opus); the target `opus` is defined in the subdirectory's
# CMakeLists.txt, so we can't use POST_BUILD here. Instead use a stamp-file
# custom command + custom target that runs after opus is built and before
# ds5-bridge links. Renames .text/.rodata of libopus.a to .time_critical.* which
# the Pico SDK linker places in RAM (loaded from flash at boot, executed from RAM
# at runtime). ~220 KB relocated, ~5-6 ms boot-time impact for the copy.
#
# LOAD-BEARING: this is required for clean duplex audio. Opus's per-frame DSP AND
# its lookup tables (mdct/fft twiddles, CELT_PVQ_U_DATA, SILK codebooks) thrash the
# XIP cache when run from flash, causing audio stutter at stock 150 MHz. Upstream
# relocates the whole library for the same reason (commit 0adca8f). Do NOT drop
# this to free heap -- it trades clean audio for stutter. The web-config/audio RAM
# coexistence must be solved within the remaining heap, not by evicting Opus.
#
# EXCEPTION -- keep the tonality analyzer in flash (recover ~21 KB of RAM):
# analysis.c (run_analysis/tonality_analysis, ~16.5 KB) and its mlp_data.c table
# (~5 KB) are only used for auto-bitrate/complexity>=7 decisions. This firmware
# encodes at COMPLEXITY 0 with VBR OFF and a fixed bitrate (see audio.cpp), so the
# analysis path is NEVER executed at runtime -- it is pure dead weight in RAM.
# After the blanket relocation above, rename ONLY these two member objects' sections
# back to .text/.rodata so the linker leaves them in flash. Zero audio risk (never
# called => never XIP-fetched), ~21 KB of heap reclaimed. (Upstream relocated the
# whole library wholesale via one objcopy for submodule-pristine convenience, not
# because all of it is per-frame hot; this fork has heap pressure they did not.)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
    COMMAND ${CMAKE_OBJCOPY} --rename-section .text=.time_critical.opus_text --rename-section .rodata=.time_critical.opus_rodata --rename-section .rodata.str1.4=.time_critical.opus_strings $<TARGET_FILE:opus>
    # Un-relocate the never-executed analysis path back to flash. Extract the two
    # members, reverse the section renames, and replace them in the archive.
    COMMAND ${CMAKE_AR} x $<TARGET_FILE:opus> analysis.c.obj mlp_data.c.obj
    COMMAND ${CMAKE_OBJCOPY} --rename-section .time_critical.opus_text=.text --rename-section .time_critical.opus_rodata=.rodata --rename-section .time_critical.opus_strings=.rodata.str1.4 analysis.c.obj
    COMMAND ${CMAKE_OBJCOPY} --rename-section .time_critical.opus_text=.text --rename-section .time_critical.opus_rodata=.rodata --rename-section .time_critical.opus_strings=.rodata.str1.4 mlp_data.c.obj
    COMMAND ${CMAKE_AR} r $<TARGET_FILE:opus> analysis.c.obj mlp_data.c.obj
    COMMAND ${CMAKE_COMMAND} -E rm -f analysis.c.obj mlp_data.c.obj
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
    DEPENDS $<TARGET_FILE:opus>
    COMMENT "Relocating libopus to RAM (tonality analyzer kept in flash)"
    VERBATIM
)
add_custom_target(opus_ram_relocate ALL DEPENDS ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp)
add_dependencies(ds5-bridge opus_ram_relocate)

target_compile_options(ds5-bridge PRIVATE -fsigned-char)
target_compile_definitions(ds5-bridge PRIVATE
        CYW43_LWIP=0
        # 0 (not 1): flash_safe_execute() must really park core1 while flash is
        # erased/programmed (config_save). With =1 the lockout is skipped and the
        # flash op races the audio core -> buzzing / crash when saving during
        # playback. core1_entry registers via flash_safe_execute_core_init().
        PICO_FLASH_ASSUME_CORE1_SAFE=0
        CYW43_PIO_CLOCK_DIV_INT=${CYW43_PIO_CLOCK_DIV_VALUE}
        ENABLE_VERBOSE=${ENABLE_VERBOSE_VALUE}
        ENABLE_DIAG=${ENABLE_DIAG_VALUE}
        ENABLE_BATT_LED=${ENABLE_BATT_LED_VALUE}
        SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ_VALUE}
        DISABLE_SPEAKER_PROC=${DISABLE_SPEAKER_PROC_VALUE}
)

option(ENABLE_WAKE_HID "Wake the Windows host on PS-button press" ON)
if(ENABLE_WAKE_HID)
    target_compile_definitions(ds5-bridge PRIVATE
            ENABLE_WAKE_HID
    )
    target_sources(ds5-bridge PRIVATE
            src/wake.cpp
    )
endif()

option(WAKE_DEBUG "Print wake-FSM transitions to UART (developer use)" OFF)
if(WAKE_DEBUG)
    target_compile_definitions(ds5-bridge PRIVATE
            WAKE_DEBUG
    )
endif()

set(VERSION "dev" CACHE STRING "Program version string")

pico_set_program_name(ds5-bridge "ds5-bridge")
pico_set_program_version(ds5-bridge "${VERSION}")

# Diagnostics go over UART0 only (GP0 TX, 115200 8N1). No USB stdio: USB-CDC
# serial perturbed the USB audio timing it was meant to diagnose.
pico_enable_stdio_uart(ds5-bridge 1)
pico_enable_stdio_usb(ds5-bridge 0)

# Add the standard include files to the build
#target_include_directories(ds5-bridge PRIVATE
#        ${CMAKE_CURRENT_LIST_DIR}
#)

target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add any user requested libraries
target_link_libraries(ds5-bridge
        pico_stdlib
        pico_multicore
        hardware_watchdog
        hardware_timer
        hardware_flash
        pico_btstack_classic
#        pico_cyw43_arch_threadsafe_background
        pico_cyw43_arch_poll
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        opus
)

pico_add_extra_outputs(ds5-bridge)

# --- Selective RAM relocation ------------------------------------------------
# Instead of relocating the whole image into RAM (the old memmap_custom.ld
# approach, which needed the overclock to absorb the resulting pressure), keep
# .text in flash (stock linker script) and relocate only the specific hot
# functions that thrash the XIP cache. relocate_to_ram() renames the chosen
# .text.<fn> sections to .time_critical.<name> on the compiled objects in a
# PRE_LINK step; the Pico SDK linker places .time_critical.* in RAM (LMA flash,
# VMA RAM). The helper locates each object by its stable SDK-relative path
# SUFFIX. RENAMES are |-separated old=new pairs; objcopy skips absent sections,
# so this is idempotent across incremental relinks.
function(relocate_to_ram _suffix _renames _comment)
    add_custom_command(TARGET ds5-bridge PRE_LINK
        COMMAND ${CMAKE_COMMAND}
            "-DOBJROOT=${CMAKE_BINARY_DIR}/CMakeFiles/ds5-bridge.dir"
            "-DOBJCOPY=${CMAKE_OBJCOPY}"
            "-DSUFFIX=${_suffix}"
            "-DRENAMES=${_renames}"
            -P "${CMAKE_SOURCE_DIR}/cmake/relocate_to_ram.cmake"
        COMMENT "${_comment}"
        VERBATIM)
endfunction()

# Tier-1: per-packet BT receive + USB FIFO data path
relocate_to_ram(lib/btstack/src/hci.c.o
    ".text.packet_handler=.time_critical.bt_packet_handler"
    "relocate: hci.c packet_handler -> RAM")
relocate_to_ram(lib/btstack/src/l2cap.c.o
    ".text.l2cap_acl_handler=.time_critical.l2cap_acl_handler"
    "relocate: l2cap.c l2cap_acl_handler -> RAM")
relocate_to_ram(lib/tinyusb/src/common/tusb_fifo.c.o
    ".text.tu_fifo_read=.time_critical.tu_fifo_read|.text.tu_fifo_read_n=.time_critical.tu_fifo_read_n|.text.tu_fifo_write=.time_critical.tu_fifo_write|.text.tu_fifo_write_n=.time_critical.tu_fifo_write_n|.text.tu_fifo_count=.time_critical.tu_fifo_count"
    "relocate: tusb_fifo.c data path -> RAM")
relocate_to_ram(src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.c.o
    ".text.cyw43_spi_transfer=.time_critical.cyw43_spi_transfer|.text.cyw43_read_bytes=.time_critical.cyw43_read_bytes|.text.cyw43_write_bytes=.time_critical.cyw43_write_bytes|.text.cyw43_read_reg_u32=.time_critical.cyw43_read_reg_u32|.text.cyw43_read_reg_u16=.time_critical.cyw43_read_reg_u16|.text.cyw43_read_reg_u8=.time_critical.cyw43_read_reg_u8|.text.cyw43_write_reg_u32=.time_critical.cyw43_write_reg_u32|.text.cyw43_write_reg_u16=.time_critical.cyw43_write_reg_u16|.text.cyw43_write_reg_u8=.time_critical.cyw43_write_reg_u8|.text.read_reg_u32_swap=.time_critical.read_reg_u32_swap|.text.write_reg_u32_swap=.time_critical.write_reg_u32_swap|.text.ns_delay.constprop.0=.time_critical.cyw43_ns_delay"
    "relocate: cyw43 PIO-SPI transport -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ll.c.o
    ".text.cyw43_ll_sdpcm_poll_device=.time_critical.cyw43_ll_sdpcm_poll_device|.text.cyw43_ll_process_packets=.time_critical.cyw43_ll_process_packets|.text.cyw43_sdpcm_send_common=.time_critical.cyw43_sdpcm_send_common|.text.cyw43_do_ioctl.part.0=.time_critical.cyw43_do_ioctl|.text.cyw43_ll_bt_has_work=.time_critical.cyw43_ll_bt_has_work|.text.cyw43_ll_parse_async_event=.time_critical.cyw43_ll_parse_async_event"
    "relocate: cyw43 SDPCM pump -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ctrl.c.o
    ".text.cyw43_poll_func=.time_critical.cyw43_poll_func|.text.cyw43_bluetooth_hci_read=.time_critical.cyw43_bluetooth_hci_read|.text.cyw43_bluetooth_hci_write=.time_critical.cyw43_bluetooth_hci_write"
    "relocate: cyw43 poll + BT HCI bus -> RAM")

# Tier-2: run-loop TX pumps + per-transfer submit paths.
# state_update is intentionally NOT moved: it runs on the host->controller
# HID-OUT (rumble/LED) path, not the audio frame path, so it stays in flash.
relocate_to_ram(lib/btstack/src/hci.c.o
    ".text.hci_run=.time_critical.hci_run|.text.hci_send_acl_packet_fragments=.time_critical.hci_send_acl_packet_fragments|.text.hci_send_acl_packet_buffer=.time_critical.hci_send_acl_packet_buffer"
    "relocate: hci.c TX pump + ACL out -> RAM")
relocate_to_ram(lib/btstack/src/l2cap.c.o
    ".text.l2cap_run=.time_critical.l2cap_run|.text.l2cap_send=.time_critical.l2cap_send|.text.l2cap_send_prepared=.time_critical.l2cap_send_prepared|.text.l2cap_notify_channel_can_send=.time_critical.l2cap_notify_channel_can_send"
    "relocate: l2cap.c TX pump + sends -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ll.c.o
    ".text.cyw43_ll_write_backplane_mem=.time_critical.cyw43_ll_write_backplane_mem|.text.cyw43_ll_read_backplane_mem=.time_critical.cyw43_ll_read_backplane_mem|.text.cyw43_write_backplane=.time_critical.cyw43_write_backplane|.text.cyw43_read_backplane.constprop.0=.time_critical.cyw43_read_backplane"
    "relocate: cyw43 backplane accessors -> RAM")
relocate_to_ram(lib/tinyusb/src/device/usbd.c.o
    ".text.tud_task_ext=.time_critical.tud_task_ext|.text.usbd_edpt_xfer=.time_critical.usbd_edpt_xfer|.text.usbd_edpt_xfer_fifo=.time_critical.usbd_edpt_xfer_fifo"
    "relocate: usbd.c task pump + xfer -> RAM")
relocate_to_ram(lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c.o
    ".text.dcd_edpt_xfer=.time_critical.dcd_edpt_xfer|.text.dcd_edpt_iso_activate=.time_critical.dcd_edpt_iso_activate"
    "relocate: dcd_rp2040.c submit + iso -> RAM")
relocate_to_ram(lib/tinyusb/src/class/audio/audio_device.c.o
    ".text.tud_audio_n_read=.time_critical.tud_audio_n_read|.text.tud_audio_n_write=.time_critical.tud_audio_n_write|.text.audiod_calc_tx_packet_sz.isra.0=.time_critical.audiod_calc_tx_packet_sz|.text.audiod_xfer_isr=.time_critical.audiod_xfer_isr"
    "relocate: audio_device.c data path -> RAM")

# Tier-3: input-report path tail (controller -> host).
# queue_try_* also back the audio inter-core FIFOs, so this helps both paths.
relocate_to_ram(lib/tinyusb/src/class/hid/hid_device.c.o
    ".text.tud_hid_n_report=.time_critical.tud_hid_n_report|.text.tud_hid_n_ready=.time_critical.tud_hid_n_ready|.text.hidd_xfer_cb=.time_critical.hidd_xfer_cb"
    "relocate: hid_device.c report path -> RAM")
relocate_to_ram(src/common/pico_util/queue.c.o
    ".text.queue_try_add=.time_critical.queue_try_add|.text.queue_try_remove=.time_critical.queue_try_remove"
    "relocate: pico_util queue add/remove -> RAM")
relocate_to_ram(src/rp2_common/pico_cyw43_arch/cyw43_arch.c.o
    ".text.cyw43_arch_poll=.time_critical.cyw43_arch_poll"
    "relocate: cyw43_arch_poll -> RAM")
relocate_to_ram(lib/btstack/src/btstack_run_loop.c.o
    ".text.btstack_run_loop_base_poll_data_sources=.time_critical.btstack_run_loop_base_poll_data_sources|.text.btstack_run_loop_poll_data_sources_from_irq=.time_critical.btstack_run_loop_poll_data_sources_from_irq"
    "relocate: btstack run-loop poll -> RAM")
