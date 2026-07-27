IOP_BIN  = smb2man.irx

# libsmb2 is vendored (libsmb2/) rather than linked from $(PS2SDK)/ports_iop/lib/libsmb2.a.
# The SDK's prebuilt copy is NOT linkable into an IRX: readelf shows R_MIPS_GPREL16 relocations and
# unbalanced HI16/LO16, i.e. it was built without -G0 ($gp-relative small data). iopfixup rejects
# that with "R_MIPS_LO16 without R_MIPS_HI16". Building the sources here means they inherit this
# module's -G0 -mno-explicit-relocs and fix up correctly.
LIBSMB2_OBJS = $(patsubst libsmb2/lib/%.c,%.o,$(wildcard libsmb2/lib/*.c))

IOP_OBJS = smb2man.o alloc.o udivdi3.o posix.o $(LIBSMB2_OBJS) imports.o

IOP_INCS += -I. -Ishim -Ilibsmb2/include -Ilibsmb2/include/smb2 -Ilibsmb2/lib -I$(PS2SDK)/common/include
# libsmb2 is portable C written for hosted platforms; silence the diagnostics that are pure noise
# on a freestanding IOP build rather than editing vendored code.
# -include stdint.h -include posix-compat.h: libsmb2 is written against C99 fixed-width types, which the IOP headers do
# not pull in on their own.
IOP_CFLAGS += -DHAVE_CONFIG_H=1 -include stdint.h -include posix-compat.h -D_U_= -Wno-unused-parameter -Wno-sign-compare

# No -lgcc: it has the same explicit-relocation problem as the prebuilt library above.
# udivdi3.c supplies the 64-bit helpers instead.

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal

$(IOP_OBJS_DIR)%.o: libsmb2/lib/%.c
	$(DIR_GUARD)
	$(IOP_CC) $(IOP_CFLAGS) $(IOP_INCS) -c $< -o $@
