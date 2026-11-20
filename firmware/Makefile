TARGET = lapses

# Internal flash
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

$(BUILD_DIR)/$(TARGET).elf: $(LIBDAISY_DIR)/build/libdaisy.a

all: ../lapses.bin
../lapses.bin: $(BUILD_DIR)/$(TARGET).bin
	cp $< $@
