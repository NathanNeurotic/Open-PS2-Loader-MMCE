# RiptOPL no-IOP-reset child loader (see loader.c). Built like NHDDL's loader:
# a tiny ELF linked into BIOS-unused memory (0x84000) so it can overwrite user
# memory with the target ELF while OPL's IOP state stays untouched.

EE_LINKFILE := linkfile
# NEWLIB_PORT_AWARE: this loader calls the raw fileXio client on purpose (no newlib fds exist
# here); the SDK headers #error on direct fio/fileXio use unless the caller declares awareness.
EE_CFLAGS = -D_EE -DNEWLIB_PORT_AWARE -Os -G0 -Wall
EE_CFLAGS += -fdata-sections -ffunction-sections
EE_LDFLAGS = -Wl,-zmax-page-size=128
EE_LDFLAGS += -s -Wl,--gc-sections

EE_BIN = elfldr.elf

EE_OBJS = loader.o

# fileXio EE client: the child loads the target ELF through the resident fileXio server
# (iomanX-aware -- reaches mmceN:/pfs/smb paths the ROM LOADFILE's plain-ioman lookup cannot).
EE_LIBS = -lfileXio

all: $(EE_BIN)

clean:
	rm -f -r $(EE_OBJS) $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
