cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_compile_definitions(-D SHELL_THREAD_PRIO=20)
sdk_add_include_directories(.)
sdk_set_main_file(main.c)

# Composite UVC (video) + UAC (audio) device.
# CherryUSB compiles the video/audio class drivers because CONFIG_CHERRYUSB_DEVICE_VIDEO
# and CONFIG_CHERRYUSB_DEVICE_AUDIO are set in defconfig.
#
# InfoNES core: InfoNES_Mapper.c #includes every nes/infones_core/mapper/*.c,
# so DO NOT list the mapper files here (that would duplicate symbols at link).
target_sources(app PRIVATE
    ./usb_composite.c
    ./frame_audio_source.c
    ./nes/infones_port.c
    ./nes/nes_menu.c            # power-up game launcher menu (renders to RGB565)
    ./nes/soft_mjpeg.c          # Phase-1 test-pattern encoder (no ROM present)
    ./nes/hw_mjpeg.c            # BL616 hardware MJPEG encoder (NES frames)
    ./nes/jpeg_head.c           # JPEG header builder for the HW encoder
    ./nes/infones_core/InfoNES.c
    ./nes/infones_core/InfoNES_Mapper.c
    ./nes/infones_core/InfoNES_pAPU.c
    ./nes/infones_core/K6502.c
)

# Hot compute paths: the SDK default is -Os (size), but InfoNES' 6502
# interpreter + PPU renderer and the per-pixel RGB565->YUYV conversion are
# ALU/branch-heavy and run materially faster at -O2. Override the global -Os
# for just these translation units (per-file COMPILE_FLAGS come last on the
# command line, so -O2 wins over -Os).
set_source_files_properties(
    ./nes/infones_core/K6502.c
    ./nes/infones_core/InfoNES.c
    ./nes/infones_core/InfoNES_Mapper.c
    ./nes/infones_core/InfoNES_pAPU.c
    ./nes/infones_port.c
    ./nes/hw_mjpeg.c
    ./nes/nes_menu.c
    ./frame_audio_source.c
    PROPERTIES COMPILE_FLAGS "-O2"
)

# Optimization step (user-selected "仅 -O3 模拟"): the 6502 interpreter
# (K6502.c) and PPU/renderer (InfoNES.c) dominate the per-frame cost
# (~69% per [PROF] instrumentation). Push them to -O3 to shrink the
# simulation slice. This per-file flag is appended after the -O2 block so
# it wins on the command line. If the build misbehaves (-O3 strict-aliasing
# issues), switch to "-O3 -fno-strict-aliasing".
set_source_files_properties(
    ./nes/infones_core/K6502.c
    ./nes/infones_core/InfoNES.c
    PROPERTIES COMPILE_FLAGS "-O3"
)

project(cherryusb)
