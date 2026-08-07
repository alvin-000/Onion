INCLUDE_CJSON=1
CFILES := \
	../common/utils/imageCache.c
include ../common/config.mk

# config.mk sets no optimisation level; these apps scale and rotate a full
# frame on every present, which is not work to do at -O0.
CFLAGS := $(CFLAGS) -O2

TARGET = installUI
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_ttf -lSDL_image -pthread -lSDL_rotozoom

include ../common/commands.mk
include ../common/recipes.mk
