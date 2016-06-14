EXEC = rsa.out

OBJECTS = \
	main.o \

BOARD ?= wisp
CONFIG_EDB ?= 1
CONFIG_PRINTF_LIB ?= libedb
CONFIG_LIBEDB_PRINTF ?= eif

LIBCHAIN_ENABLE_DIAGNOSTICS ?= 0

include $(MAKER_ROOT)/Makefile.gcc
include $(MAKER_ROOT)/Makefile.board
include $(MAKER_ROOT)/Makefile.console
include $(MAKER_ROOT)/Makefile.edb

CFLAGS += \
	-I $(LIBCHAIN_ROOT)/src/include \
	-I $(LIBEDB_ROOT)/src/include \
	-I $(WISP_BASE_ROOT) \
	-I $(WISP_BASE_ROOT)/Sensors \
	-I $(WISP_BASE_ROOT)/wired \

ifeq ($(LIBCHAIN_ENABLE_DIAGNOSTICS),1)
CFLAGS += -DLIBCHAIN_ENABLE_DIAGNOSTICS
endif

CFLAGS += \
	-I$(LIBMSP_ROOT)/src/include \
	-I$(LIBMSPMATH_ROOT)/src/include \

LFLAGS += \
	-L $(LIBCHAIN_ROOT)/bld/gcc \
	-L $(LIBMSPMATH_ROOT)/bld/gcc \
	-L $(WISP_BASE_ROOT)/gcc \

LIBS += \
    -lchain \
	-lwisp-base \
	-lmspmath \

VPATH = ../../src
