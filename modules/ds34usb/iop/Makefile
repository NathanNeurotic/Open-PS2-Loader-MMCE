IOP_BIN = ds34usb.irx
IOP_OBJS = ds34usb.o smsutils.o imports.o

IOP_INCS += -I../../../include/

# Experimental PS5 DualSense (USB) support, off by default (see the top-level DUALSENSE flag).
ifeq ($(DUALSENSE),1)
IOP_CFLAGS += -DDS5_ENABLE
endif

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
