# pico-bootLoader - resident .uf2 bootloader frontend for the RP2350 emulators.
# IMPORTANT: Use the latest version of tinyusb and pico pio USB.
# Build/USB structure mirrors pico-infonesPlus (the up-to-date PIO-USB pattern).

cmake_minimum_required(VERSION 3.13)

set(projectname bootLoader)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release
        CACHE STRING "Choose the type of build : None Debug Release RelWithDebInfo MinSizeRel."
        FORCE)
endif()
message("* Current build type is : ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# Hardware / board selection
# ---------------------------------------------------------------------------
if(NOT HW_CONFIG)
    set(HW_CONFIG 8 CACHE STRING "Select the hardware configuration for your board")
endif()
if(NOT USE_HSTX)
    set(USE_HSTX 1 CACHE STRING "Use HSTX for display output when possible")
endif()

include("pico_shared/BoardConfigs.cmake")

if(NOT PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Board type")
    message("PICO_BOARD not set, using default: ${PICO_BOARD}")
endif()

add_definitions(-DNDEBUG)

# A failed malloc must return NULL -- every Frens::f_malloc call site handles
# that and degrades gracefully (image conversion skips, GUI falls back to text
# mode). The SDK default panics inside malloc instead, turning a tight-heap
# situation on no-PSRAM boards into a boot loop. Directory-scoped so it also
# reaches pico_malloc's check_alloc wherever it is compiled (pico_shared).
add_definitions(-DPICO_MALLOC_PANIC=0)

# ---------------------------------------------------------------------------
# TinyUSB family support + Pico SDK init (include family_support.cmake BEFORE
# pico_sdk_init() so TinyUSB picks up the board-specific PIO USB config).
# ---------------------------------------------------------------------------
set(FAMILY rp2040)
include($ENV{PICO_SDK_PATH}/lib/tinyusb/hw/bsp/family_support.cmake)

if(ENABLE_PIO_USB EQUAL 0 AND TARGET tinyusb_common_base)
    get_target_property(_tu_defs tinyusb_common_base INTERFACE_COMPILE_DEFINITIONS)
    if(_tu_defs)
        list(REMOVE_ITEM _tu_defs "CFG_TUH_RPI_PIO_USB=1")
        set_target_properties(tinyusb_common_base PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${_tu_defs}")
    endif()
endif()

project(${projectname} C CXX ASM)

pico_sdk_init()

# Create the tinyusb_pico_pio_usb target (needs pico_generate_pio_header, only
# available after pico_sdk_init). Skip when PIO USB is disabled.
if(ENABLE_PIO_USB EQUAL 1)
    message("Checking for PIO USB support...")
    check_and_add_pico_pio_usb_support()
endif()
message("PIO USB support: ${ENABLE_PIO_USB}, using PIO: ${PIO_USB_USE_PIO}")

# Shared bootloader/app flash map (single source of truth for both this
# bootloader and the emulators relinked with -DBUILD_FOR_BOOTLOADER=ON).
include("pico_shared/BootPartition.cmake")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(${projectname}
    main.cpp
    src/uf2_loader.c
    src/flash_writer.c
    src/app_launch.c
    src/storage_pico.c
    src/program_name.c
    src/emulators_txt.c
    src/sd_boot_ini.c
    src/uf2_crc.cpp
    src/gui.cpp
    src/screensaver.cpp
    src/progress_bar.cpp
    src/image_convert.cpp
    src/third_party/PNGdec/PNGdec.cpp
    src/third_party/PNGdec/adler32.c
    src/third_party/PNGdec/crc32.c
    src/third_party/PNGdec/infback.c
    src/third_party/PNGdec/inffast.c
    src/third_party/PNGdec/inflate.c
    src/third_party/PNGdec/inftrees.c
    src/third_party/PNGdec/zutil.c
    src/third_party/JPEGDEC/JPEGDEC.cpp
)

# PNGdec / JPEGDEC ship with Arduino-style platform gates; steer them onto
# the stdlib path (they otherwise pull in <Arduino.h>). Their zlib sources are
# platform-neutral and don't need this.
set_source_files_properties(
    src/third_party/PNGdec/PNGdec.cpp
    src/third_party/JPEGDEC/JPEGDEC.cpp
    src/image_convert.cpp
    PROPERTIES
    COMPILE_DEFINITIONS "__LINUX__"
)

# Default PNG_MAX_BUFFERED_PIXELS holds only two 320-wide RGBA source lines
# (2562 B). Our source cap is 1280x960; two 1280 RGBA lines want ~10240 B.
# Set project-wide so both PNGdec.cpp (which sizes PNGIMAGE) and image_convert
# (which does placement-new of PNG) agree.
target_compile_definitions(${projectname} PRIVATE
    PNG_MAX_BUFFERED_PIXELS=10752
)

pico_set_program_name(${projectname} "${projectname}")
pico_set_program_version(${projectname} "0.1")

if(UART_ENABLED EQUAL 0)
    message("Disabling UART output for this board.")
    pico_enable_stdio_uart(${projectname} 0)
else()
    pico_enable_stdio_uart(${projectname} 1)
endif()
pico_enable_stdio_usb(${projectname} 0)

# What family_configure_target() used to do for us, inlined.
target_compile_definitions(${projectname} PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)
target_compile_options(${projectname} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-cast-function-type
    -Wno-maybe-uninitialized
)

# ---------------------------------------------------------------------------
# Project compile definitions (board pins, features, flash map)
# ---------------------------------------------------------------------------
target_compile_definitions(${projectname} PRIVATE
    DVICONFIG=${DVICONFIG}
    SDCARD_PIN_CS=${SD_CS}
    SDCARD_PIN_SCK=${SD_SCK}
    SDCARD_PIN_MOSI=${SD_MOSI}
    SDCARD_PIN_MISO=${SD_MISO}
    SDCARD_PIO=pio1
    SDCARD_SPI=${SD_SPI}
    NES_PIN_CLK=${NES_CLK}
    NES_PIN_DATA=${NES_DATA}
    NES_PIN_LAT=${NES_LAT}
    NES_PIO=${NES_PIO}
    NES_PIN_CLK_1=${NES_CLK_1}
    NES_PIN_DATA_1=${NES_DATA_1}
    NES_PIN_LAT_1=${NES_LAT_1}
    NES_PIO_1=${NES_PIO_1}
    WII_PIN_SDA=${WII_SDA}
    WII_PIN_SCL=${WII_SCL}
    LED_GPIO_PIN=${LED_GPIO_PIN}
    HW_CONFIG=${HW_CONFIG}
    WIIPAD_I2C=${WIIPAD_I2C}
    USE_I2S_AUDIO=${USE_I2S_AUDIO}
    PICO_AUDIO_I2S_DATA_PIN=${PICO_AUDIO_I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${PICO_AUDIO_I2S_CLOCK_PIN_BASE}
    PICO_AUDIO_I2S_PIO=${PICO_AUDIO_I2S_PIO}
    PICO_AUDIO_I2S_DMA_IRQ=1
    PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED=${PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED}
    PICO_AUDIO_I2S_RESET_PIN=${PICO_AUDIO_I2S_RESET_PIN}
    PICO_AUDIO_I2S_INTERRUPT_PIN=${PICO_AUDIO_I2S_INTERRUPT_PIN}
    PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON=${PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON}
    CFG_TUH_RPI_PIO_USB=${ENABLE_PIO_USB}
    PIO_USB_USE_PIO=${PIO_USB_USE_PIO}
    PSRAM_CS_PIN=${PSRAM_CS_PIN}
    GPIOHSTXD0=${GPIOHSTXD0}
    GPIOHSTXD1=${GPIOHSTXD1}
    GPIOHSTXD2=${GPIOHSTXD2}
    GPIOHSTXCK=${GPIOHSTXCK}
    GPIOHSTXINVERTED=${GPIOHSTXINVERTED}
    ENABLE_VU_METER=${ENABLE_VU_METER}
    WIIPAD_DELAYED_START=${WIIPAD_DELAYED_START}
    ENABLEDVI=1
    DOUBLEFRAMEBUFFER=0
    # Flash map for src/boot_config.h (kept in sync with BootPartition.cmake).
    BOOTLOADER_SIZE=${FRENS_BOOTLOADER_SIZE}
    FLASH_TOTAL_SIZE=${FRENS_FLASH_TOTAL}
)

if(NOT PICO_PLATFORM STREQUAL "rp2040")
    target_compile_definitions(${projectname} PRIVATE
        PICO_PIO_USE_GPIO_BASE=1
        PICO_STACK_SIZE=3072
    )
endif()

if(DEFINED PICO_DEFAULT_UART_TX_PIN)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_TX_PIN=${PICO_DEFAULT_UART_TX_PIN}
        PICO_DEFAULT_UART=0
    )
endif()
if(DEFINED PICO_DEFAULT_UART_RX_PIN)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_RX_PIN=${PICO_DEFAULT_UART_RX_PIN}
    )
endif()
if(DEFINED PICO_DEFAULT_UART)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART=${PICO_DEFAULT_UART}
    )
endif()

# ---------------------------------------------------------------------------
# PIO USB (optional)
# ---------------------------------------------------------------------------
if(ENABLE_PIO_USB EQUAL 1)
    if(NOT PIO_DP_PLUS_PIN EQUAL -1)
        message(STATUS "Using PIO USB DP pin: ${PIO_DP_PLUS_PIN}")
        target_compile_definitions(${projectname} PRIVATE
            PICO_DEFAULT_PIO_USB_DP_PIN=${PIO_DP_PLUS_PIN}
        )
    endif()
    message("Adding target link libraries for PIO USB support.")
    target_link_libraries(${projectname} PRIVATE tinyusb_pico_pio_usb)
endif()

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
target_link_libraries(${projectname} PRIVATE
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_i2c
    hardware_interp
    hardware_timer
    hardware_clocks
    hardware_pwm
    hardware_flash
    cmsis_core
    dvi
    util
    tinyusb_host
    tinyusb_board
    pico_shared
    xinput_host
)

if(PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    target_link_libraries(${projectname} PRIVATE pico_cyw43_arch_none)
endif()

target_include_directories(${projectname} PRIVATE
    pico_lib
    pico_shared
    tusb_xinput
    src
    .
)

add_subdirectory(pico_lib/dvi)
add_subdirectory(pico_lib/util)
add_subdirectory(pico_shared)
add_subdirectory(tusb_xinput xinput_host)

# Link this image into the resident bootloader region at the start of flash
# (0x10000000, capped to FRENS_BOOTLOADER_SIZE) so it can never overlap the
# application partition and the bootrom always runs it first.
frens_link_as_bootloader(${projectname})

target_link_options(${projectname} PRIVATE -Xlinker --print-memory-usage --data-sections)
pico_add_extra_outputs(${projectname})
