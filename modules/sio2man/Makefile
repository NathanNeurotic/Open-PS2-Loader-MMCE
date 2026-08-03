# Locally-built sio2man with the RiptOPL priority-ceiling patch (issue #340).
# Source provenance, patch rationale, and the revert path: PROVENANCE.md in this directory.
IOP_BIN  = sio2man.irx
IOP_OBJS = sio2man.o imports.o exports.o

# iop_mmio_hwport.h lives in the SDK's common includes, not the IOP include set.
IOP_INCS += -I$(PS2SDK)/common/include

include $(PS2SDK)/Defs.make
include ../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
