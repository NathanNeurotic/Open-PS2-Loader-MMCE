IOP_BIN  = smb2man.irx
IOP_OBJS = smb2man.o alloc.o udivdi3.o imports.o

# libsmb2 lives in the SDK's IOP ports tree, not the main iop include path.
IOP_INCS += -I$(PS2SDK)/ports_iop/include -I$(PS2SDK)/ports/include
IOP_LDFLAGS += -L$(PS2SDK)/ports_iop/lib
# Deliberately NOT -lgcc. libsmb2 needs __udivdi3/__umoddi3, but linking the toolchain's libgcc.a
# into an IRX makes iopfixup fail with "R_MIPS_LO16 without R_MIPS_HI16" -- libgcc is built with
# explicit relocations, IRX modules must not be. udivdi3.c supplies the two helpers instead, built
# with this module's own flags.
IOP_LIBS += -lsmb2

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
