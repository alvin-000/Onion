INCLUDE_UTILS = 0
SANITIZE = 0
include ../common/config.mk

TARGET = libuiscale.so
# config.mk sets no optimisation level, so without this the scaler's per-pixel
# loop compiles at -O0. Measured on a Mini Flip: 191ms per present at -O0, for
# work that is a few dozen cycles a pixel. This is not optional here.
CFLAGS := $(CFLAGS) -O2 -fpic
LDFLAGS := $(LDFLAGS) -shared -ldl -lSDL -lmi_sys -lmi_gfx

include ../common/commands.mk
include ../common/recipes.mk
