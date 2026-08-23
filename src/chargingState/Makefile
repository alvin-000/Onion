INCLUDE_CJSON=1
include ../common/config.mk

# config.mk sets no optimisation level; these apps scale and rotate a full
# frame on every present, which is not work to do at -O0.
CFLAGS := $(CFLAGS) -O2

TARGET = chargingState
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_image -lSDL_ttf -lSDL_rotozoom -lmi_sys -lmi_gfx

include ../common/commands.mk
include ../common/recipes.mk
