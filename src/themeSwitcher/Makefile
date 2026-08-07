INCLUDE_CJSON=1
include ../common/config.mk

# config.mk sets no optimisation level. themeSwitcher scales more than any
# other app - a zoomSurface pass per theme preview - so -O0 hurts it most.
CFLAGS := $(CFLAGS) -O2

TARGET = themeSwitcher
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_image -lSDL_ttf -lSDL_rotozoom

include ../common/commands.mk
include ../common/recipes.mk
