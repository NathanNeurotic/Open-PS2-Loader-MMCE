IOP_BIN := udpfs_bd.irx

IOP_SRC_DIR := src/
IOP_OBJS_DIR := obj/
IOP_CFLAGS += -mno-check-zero-division -DFEATURE_UDPFS_BD -DUDPRDMA_MAX_SOCKETS=1
IOP_INCS := -Iinclude -Isrc -I../udpfs_smap/include -I../udpfs_ministack/include
IOP_OBJS = main.o udprdma.o udpfs_core.o udpfs_bd.o imports.o exports.o

all:: $(IOP_BIN)

clean::
	rm -f -r $(IOP_BIN) $(IOP_OBJS_DIR)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
