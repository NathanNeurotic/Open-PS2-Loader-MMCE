# Our ps2ips.irx, replacing the stock one from ps2sdk.
#
# In ps2sdk's do_recvfrom the reply sockaddr is copied into the shared
# RPC buffer before the request's ee_addr and intr_data fields have been
# read, so both DMA transfers go to EE address 0 and UDP receive on the
# EE never works. The source is a copy of ps2sdk iop/tcpip/tcpips with
# the changes marked "RA fix". Candidate for upstreaming to ps2sdk.
IOP_BIN  = ps2ips.irx
IOP_OBJS = ps2ips.o imports.o

IOP_CFLAGS += -DPS2IP_DNS
IOP_INCS += -I$(PS2SDK)/common/include

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
