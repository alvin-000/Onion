include ../common/config.mk

# config.mk sets no optimisation level; these apps scale and rotate a full
# frame on every present, which is not work to do at -O0.
CFLAGS := $(CFLAGS) -O2

TARGET = playActivityUI
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_image -lSDL_ttf -lsqlite3 -lSDL_rotozoom

include ../common/commands.mk
include ../common/recipes.mk
