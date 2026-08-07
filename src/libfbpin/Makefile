INCLUDE_UTILS = 0
SANITIZE = 0
include ../common/config.mk

TARGET = libfbpin.so
# Same reason as libuiscale: config.mk sets no optimisation level. This hook is
# on the ioctl path of every process on the system, games included, so it should
# not be built at -O0.
CFLAGS := $(CFLAGS) -O2 -fpic
LDFLAGS := $(LDFLAGS) -shared -ldl

include ../common/commands.mk
include ../common/recipes.mk
