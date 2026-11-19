# The Touch 2 build: make, from this directory.
#
# libDaisy and DaisySP are not vendored. Override if they move:
# make LIBDAISY_DIR=... DAISYSP_DIR=...

TARGET = lapses

# Internal flash, no bootloader on the board. -O3 overflows the 128 KB; -O2 is 86%.
APP_TYPE = BOOT_NONE

OPT = -O2

CPP_SOURCES = src/main.cpp src/panel.cpp src/synth.cpp src/voices.cpp \
              src/wfc.cpp

C_INCLUDES += -Isrc

# Lets sqrtf be VSQRT instead of a libm call
C_DEFS += -fno-math-errno

LIBDAISY_DIR ?= ../../synthux/firmware/libDaisy
DAISYSP_DIR  ?= ../../synthux/firmware/DaisySP
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

include $(SYSTEM_FILES_DIR)/Makefile

# Relink when libDaisy changes: the app does not otherwise depend on it
$(BUILD_DIR)/$(TARGET).elf: $(LIBDAISY_DIR)/build/libdaisy.a
