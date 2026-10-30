# The Touch 2 build. From the repo root: make daisy
#
# libDaisy and DaisySP are shared with the old project rather than copied, so
# there is one toolchain to keep current. Override on the command line if they
# move: make daisy LIBDAISY_DIR=... DAISYSP_DIR=...

TARGET = lapses

# The delay lines are 1.5 MB, so the app runs from QSPI with SDRAM for them
APP_TYPE = BOOT_QSPI

OPT = -O3

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
