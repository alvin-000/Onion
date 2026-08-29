INCLUDE_UTILS = 0
SANITIZE = 0
include ../common/config.mk

TARGET = libuiscale.so
# config.mk sets no optimisation level, so without this the scaler's per-pixel
# loop compiles at -O0. Measured on a Mini Flip: 191ms per present at -O0, for
# work that is a few dozen cycles a pixel. This is not optional here.
CFLAGS := $(CFLAGS) -O2 -fpic
# Deliberately not -lSDL. This shim is in the system-wide LD_PRELOAD, and an
# LD_PRELOADed object's dependencies join the global symbol scope of every
# process on the system - which is searched ahead of a dlopen'd library's own
# dependencies. Linking SDL 1.2 here therefore force-loaded it into processes
# that use SDL2 and hijacked the 142 symbol names the two versions share
# (SDL_Init, SDL_VideoInit, SDL_GetError, ...), leaving SDL2's video subsystem
# uninitialised. Measured on a Flip: a pygame app died with "No available video
# device" with this shim preloaded and started cleanly without it.
#
# Nothing is lost. The shim only has reach in a program that has an undefined
# SDL_SetVideoMode for the preload to capture, which means that program links
# SDL 1.2 itself - so SDL 1.2 is in the process either way and the dlsym lookups
# in uiscale.c find it. Checked against every binary on a populated card.
LDFLAGS := $(LDFLAGS) -shared -ldl -lmi_sys -lmi_gfx

include ../common/commands.mk
include ../common/recipes.mk
