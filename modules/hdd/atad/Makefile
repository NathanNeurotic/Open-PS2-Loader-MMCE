IOP_BIN := ata_bd.irx
IOP_SRC_DIR := src/
IOP_OBJS_DIR := obj/
IOP_INCS := -Isrc

# ata_bd = SDK atad + the BDM block-device interface (the ps2sdk iop/dev9/ata_bd flag set).
IOP_CFLAGS += -DATA_USE_DEV9=1 -DATA_GAMESTAR_WORKAROUND=1 -DATA_ENABLE_BDM=1

# exports.o MUST come first, and _retonly is defined AFTER the export table in src/exports.tab, so
# ps2dev:latest's iopfixup does not reject _retonly as a value-0 .text symbol (same load-bearing
# ordering as modules/network/udpfs_ministack). Keep on re-vendor.
IOP_OBJS = exports.o ps2atad.o imports.o

all:: $(IOP_BIN)

# Some Makefile.iopglobal vintages only mkdir the obj dir inside the build-imports recipe --
# pre-create it so the first compile step cannot fail on a missing directory.
$(IOP_OBJS_DIR):
	mkdir -p $@
$(IOP_OBJS_DIR)build-exports.c $(IOP_OBJS_DIR)build-imports.c: | $(IOP_OBJS_DIR)

clean::
	rm -f -r $(IOP_BIN) $(IOP_OBJS_DIR)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
