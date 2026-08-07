ifeq (,$(BUILD_DIR))
BUILD_DIR=$(shell pwd -P)
endif

ifeq (,$(VERSION))
VERSION="4.x.x-dev-test"
endif

PLATFORM ?= $(UNION_PLATFORM)
ifeq (,$(PLATFORM))
PLATFORM=linux
endif

LIB = /mnt/SDCARD/.tmp_update/lib

CC 		= $(CROSS_COMPILE)gcc
CXX 	= $(CROSS_COMPILE)g++
STRIP 	= $(CROSS_COMPILE)strip

SOURCES := $(SOURCES) .

# Sources from outside the component directory, compiled once per component
# rather than once per tree.
#
# ../common/utils/str.o and ../../include/cjson/cJSON.o used to be single files
# that every component linked. Whichever component compiled one first fixed its
# flags for all the rest, so on a tree where individual components opt into -O2,
# an optimised component could silently link unoptimised helpers - decided by
# nothing more than build order. Measured before the fix: packageManager came
# out 78008 bytes in a full build and 65720 built on its own.
#
# Naming each object after the target gives every component its own copy, built
# with its own flags.
ifeq ($(INCLUDE_CJSON),1)
EXT_CFILES := $(EXT_CFILES) $(wildcard ../../include/cjson/*.c)
endif
ifneq ($(INCLUDE_UTILS),0)
EXT_CFILES := $(EXT_CFILES) \
	../common/utils/str.c \
	../common/utils/log.c \
	../common/utils/file.c
endif

CFILES := $(CFILES) $(foreach dir, $(SOURCES), $(wildcard $(dir)/*.c))
CPPFILES := $(CPPFILES) $(foreach dir, $(SOURCES), $(wildcard $(dir)/*.cpp))

# Recursively expanded, all of them, and that is the point: TARGET is not set
# until after this file is included, and several components (keymon,
# gameSwitcher, sendUDP) append to CFILES *after* including it too. Deferring
# the split to expansion time means it does not matter what order any of that
# happens in.
#
# Anything reached by a relative path is outside this component by definition,
# so it is routed to a per-component object whether it was listed above or added
# by hand in a component Makefile.
ext_obj = $(dir $(1))$(TARGET)-$(notdir $(1:.c=.o))
LOCAL_CFILES = $(filter-out ../%,$(CFILES))
ALL_EXT_CFILES = $(EXT_CFILES) $(filter ../%,$(CFILES))

ifeq ($(TEST),1)
# The gtest build reaches its sources at a different depth (../src/...), which
# has no matching rule, and shares nothing with a component build anyway.
OFILES = $(CFILES:.c=.o) $(CPPFILES:.cpp=.o)
else
OFILES = $(LOCAL_CFILES:.c=.o) $(CPPFILES:.cpp=.o) \
	$(foreach f,$(ALL_EXT_CFILES),$(call ext_obj,$(f)))
endif

CFLAGS := -I../../include -I../common -DPLATFORM_$(shell echo $(PLATFORM) | tr a-z A-Z) -DONION_VERSION="\"$(VERSION)\"" -Wall

ifeq ($(DEBUG),1)
CFLAGS := $(CFLAGS) -DLOG_DEBUG -g3
endif

ifeq ($(TEST),1)
CFLAGS := $(CFLAGS) -I../include -I../src/common -I$(GTEST_INCLUDE_DIR)
endif

ifeq ($(SANITIZE),1)
CFILES := $(CFILES) ../common/utils/asan.c
CFLAGS := $(CFLAGS) -fno-omit-frame-pointer -fsanitize=address -static-libasan
LDFLAGS := -fsanitize=address -static-libasan $(LDFLAGS)
endif

CXXFLAGS := $(CFLAGS)
LDFLAGS := $(LDFLAGS) -L../../lib -L/usr/local/lib

ifeq ($(PLATFORM),miyoomini)
CFLAGS := $(CFLAGS) -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve -Wl,-rpath=$(LIB)

ifdef INCLUDE_SHMVAR
LDFLAGS := $(LDFLAGS) -lshmvar
endif

endif
