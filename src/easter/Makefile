INCLUDE_CJSON=1
include ../common/config.mk

TARGET = easter
CFLAGS := $(CFLAGS) -DHAS_AUDIO
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_image -lSDL_ttf -lSDL_mixer -lSDL_rotozoom

include ../common/commands.mk
include ../common/recipes.mk
