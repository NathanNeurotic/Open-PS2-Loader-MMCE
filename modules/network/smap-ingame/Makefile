IOP_BIN = smap.irx
IOP_OBJS = main.o smap.o xfer.o imports.o

# RetroAchievements: publish SMAPSendPacket as a library so raudp can hand the
# NIC a finished frame without going through the stack. Conditional because
# adding exports.o changes this module's link layout (the SDK then drops
# --allow-zero-text and pins the export table at .text offset 0), and the
# default build has no reason to carry either the table or its imports.
ifeq ($(RETROACHIEVEMENTS),1)
IOP_OBJS += exports.o
IOP_CFLAGS += -DRETROACHIEVEMENTS
endif

IOP_INCS += -I../../iopcore/common -I../common

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
