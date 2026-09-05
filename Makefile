VERSION = 1
SUBVERSION = 2
PATCHLEVEL = 0
EXTRAVERSION = Beta

# How to DEBUG?
# Simply type "make <debug mode>" to build OPL with the necessary debugging functionality.
# Debug modes:
#	debug		    	 -	UI-side debug mode (UDPTTY)
#	iopcore_debug		 -	UI-side + iopcore debug mode (UDPTTY).
#	ingame_debug		 -	UI-side + in-game debug mode. IOP core modules will not be built as debug versions (UDPTTY).
#	debug_ppctty		 -	UI-side debug mode (PowerPC UART)
#	iopcore_ppctty_debug -	UI-side + iopcore debug mode (PowerPC UART).
#	ingame_ppctty_debug	 -	UI-side + in-game debug mode. IOP core modules will not be built as debug versions (PowerPC UART).
#	eesio_debug			 -	UI-side + eecore debug mode (EE SIO)
#	deci2_debug			 -	UI-side + in-game DECI2 debug mode (EE-side only).

# I want to put my name in my custom build! How can I do it?
# Type "make LOCALVERSION=-foobar"

# ======== START OF CONFIGURABLE SECTION ========
# You can adjust the variables in this section to meet your needs.
# To enable a feature, set its variable's value to 1. To disable, change it to 0.
# Do not COMMENT out the variables!!
# You can also specify variables when executing make: "make RTL=1 IGS=1 PADEMU=1"

# Check if EXTRA_FEATURES is set, default to 0
EXTRA_FEATURES ?= 0

# Set RTL and IGS based on EXTRA_FEATURES, but allow user overrides
#Enables/disables Right-To-Left (RTL) language support
RTL ?= $(EXTRA_FEATURES)
#Enables/disables In Game Screenshot (IGS). NB: It depends on GSM and IGR to work
IGS ?= $(EXTRA_FEATURES)

#Enables/disables pad emulator
PADEMU ?= 1

#Enables/disables PS5 DualSense (USB) support in the pad emulator. HW-VALIDATED on a real DS5
#(maintainer, 2026-07-06) but kept OFF in the default build by deliberate choice -- the DS5 code
#is entirely #ifdef DS5_ENABLE, so with this at 0 ds34usb.irx is behaviourally identical to the
#build that is already hardware-proven. Named DUALSENSE=1 release assets return with item 58.
DUALSENSE ?= 0

#GSM 1080p video mode (a GSM-synthetic progressive raster; see ee_core DTV_1080P). ON in every
#build since it was confirmed working on real hardware (RT4K reported 1080p and displayed it
#correctly, 2026-08-09) -- it is no longer shipped as an isolated variant.
#
#DEFAULTING THIS TO 1 IS LOAD-BEARING, not just convenience: $GSMVMode is stored as a RAW INDEX
#into gsmvmodeNames[]/predef_vmode[], and the 1080p entry is APPENDED LAST. With the flag
#optional the list was 29 entries in one build and 30 in another, so a per-game video mode saved
#by one flavour meant a DIFFERENT mode when read by the other -- and both flavours ship in the
#same release. One list length everywhere removes that entire class of bug.
#Kept as an escape hatch (build with GSM1080P=0) if a console ever regresses; nothing we PUBLISH
#should set it to 0, or the index divergence comes back.
#The mode itself stays gated behind the triple-confirm in the GUI.
GSM1080P ?= 1

#Enables/disables RetroAchievements support: the menu-side "is this game tracked?" check and the
#in-game memory telemetry that feeds a PC client running rcheevos. Builds its OWN release flavour
#(see .github/scripts/build_rolling_extras.sh); the default ELF must stay byte-for-byte unaffected,
#so every RA source file is wrapped in #ifdef RETROACHIEVEMENTS and every call site in a shared
#file sits inside one too.
#
#A FLAG IS NOT A DEPENDENCY: make cannot see that flipping this changes the meaning of an object
#file, so run "make clean" when switching it. This bites hardest on EECoreConfig_t and
#OPL_MODULE_ID, where a stale .o silently loads the wrong module blob.
#
#Only the four launch legs that go through sysLaunchLoaderElf (BDM/ETH/HDD/MMCE) can ever carry
#telemetry -- Neutrino-core games, UDPFS and PS1/VCD hand off to an external ELF and never load
#ee_core at all. See docs/RETROACHIEVEMENTS-INTEGRATION-PLAN.md.
RETROACHIEVEMENTS ?= 0

#Enables/disables building of an edition of OPL that will support the DTL-T10000 (SDK v2.3+)
DTL_T10000 ?= 0

#Nor stripping neither compressing binary ELF after compiling.
NOT_PACKED ?= 0

# ======== END OF CONFIGURABLE SECTION. DO NOT MODIFY VARIABLES AFTER THIS POINT!! ========
DEBUG ?= 0
EESIO_DEBUG ?= 0
INGAME_DEBUG ?= 0
DECI2_DEBUG ?= 0
# Field diagnostics. Unlike DEBUG (which needs a TTY link and is never what a tester runs), this
# only turns on the on-screen boot-stage labels and the error-code detail suffix, so a normal
# release ELF can be handed to a tester and still report WHERE it wedged. The strings it emits are
# untranslated developer text by design -- that is why they must never be in a plain release.
OPLDIAG ?= 0
#How the TTY will reach developer: 'UDP', 'PPC_UART'.
TTY_APPROACH ?= UDP

# ======== DO NOT MODIFY VALUES AFTER THIS POINT! UNLESS YOU KNOW WHAT YOU ARE DOING ========
# The two version inputs have different jobs, and step-178 proved they cannot share one
# anchor:
#   REVISION is the ORDINAL testers compare ("am I up to date?"). It must be monotonic --
#   a newer build must never print a lower number, and every report on file quotes a
#   HEAD-derived count. Code-anchoring it (step-178, caught in review before it
#   published) made the number go BACKWARDS across publishes and broke comparability
#   with every existing report. So REVISION stays HEAD-based: a republish of identical
#   code prints a new NUMBER, which is mildly confusing at worst, never misleading.
#   GIT_HASH carries the reproducibility contract instead: it anchors to CODE_ANCHOR,
#   the last commit touching anything the build can consume (tree minus the exclusion
#   list below). A commit confined to those NAMED paths -- CI, the agent scratch
#   areas, notes/, obj/ -- does not move it, so the hash names the exact code a
#   tester ran and "check out what the tester ran" is a checkout of this hash.
#   Note that docs/ is deliberately NOT on the list, so a docs-only commit does
#   move the hash: the list errs toward caution, and anything not named still
#   moves it, so a missed code path can never silently collide two different
#   binaries under one string.
CODE_ANCHOR = $(shell git log -1 --format=%H -- . ':(exclude).github' ':(exclude)agent-file-drop' ':(exclude)frame_builds' ':(exclude)notes' ':(exclude)obj' ':(exclude).claude' ':(exclude).agents' ':(exclude).codex' ':(exclude).codex-tmp-nhddl' ':(exclude).codex-tmp-wle-r3z' 2>/dev/null)
REVISION = $(shell expr $(shell git rev-list --count HEAD) + 2)

GIT_HASH = $(shell git rev-parse --short=7 $(if $(CODE_ANCHOR),$(CODE_ANCHOR),HEAD) 2>/dev/null)
ifeq ($(shell git diff --quiet; echo $$?),1)
  DIRTY = -dirty
endif
ifneq ($(shell test -d .git; echo $$?),0)
  DIRTY = -dirty
endif

# Only v* release tags name a build; checkpoint tags (rebuild-NN etc.) must not hijack the
# version string or drop the toolchain-flavour suffix.
GIT_TAG = $(shell git describe --exact-match --tags --match "v*" 2>/dev/null)
OPL_VERSION = v$(VERSION).$(SUBVERSION).$(PATCHLEVEL)$(if $(EXTRAVERSION),-$(EXTRAVERSION))-$(REVISION)$(if $(GIT_HASH),-$(GIT_HASH))$(if $(DIRTY),$(DIRTY))$(if $(LOCALVERSION),-$(LOCALVERSION))

ifneq ($(GIT_TAG),)
ifneq ($(GIT_TAG),latest)
	# git revision is tagged
	OPL_VERSION = $(GIT_TAG)$(if $(DIRTY),$(DIRTY))$(if $(LOCALVERSION),-$(LOCALVERSION))
endif
endif

FRONTEND_OBJS = pad.o xparam.o fntsys.o renderman.o menusys.o OSDHistory.o system.o elfldr_noreset.o elfldr.o lang.o lang_internal.o config.o hdd.o dialogs.o favsupport.o \
		dia.o ioman.o texcache.o themes.o supportbase.o bdmsupport.o netsupport.o ethsupport.o udpfssupport.o hddsupport.o zso.o lz4.o \
		appsupport.o mmcesupport.o artindex.o vcdsupport.o cuesupport.o libview.o retrogem.o folderbrowse.o gui.o guigame.o vmc_groups.o textures.o opl.o atlas.o nbns.o httpclient.o gsm.o cheatman.o sound.o ps2cnf.o tar.o

IOP_OBJS =	iomanx.o filexio.o ps2fs.o usbd.o bdmevent.o \
		bdm.o bdmfs_fatfs.o usbmass_bd.o iLinkman.o IEEE1394_bd.o mx4sio_bd.o \
		ps2atad.o hdpro_atad.o poweroff.o ps2hdd.o xhdd.o genvmc.o lwnbdsvr.o \
		ps2dev9.o smsutils.o ps2ip.o smap.o smap_udpbd.o udpfs_smap.o udpfs_ministack.o udpfs_bd.o udpfs_ioman.o isofs.o nbns-iop.o \
		sio2man.o padman.o mcman.o mcserv.o mmceman.o mmcedrv.o mmceigr.o \
		httpclient-iop.o netman.o ps2ips.o \
		bdm_mcemu.o mmce_mcemu.o hdd_mcemu.o smb_mcemu.o \
		iremsndpatch.o apemodpatch.o f2techioppatch.o cleareffects.o resetspu.o \
		libsd.o audsrv.o

EECORE_OBJS = ee_core.o ioprp.o util.o \
		udnl.o imgdrv.o eesync.o \
		bdm_cdvdman.o bdm_ata_cdvdman.o IOPRP_img.o smb_cdvdman.o \
		hdd_cdvdman.o mmce_cdvdman.o hdd_hdpro_cdvdman.o cdvdfsv.o \
		ingame_smstcpip.o smap_ingame.o smbman.o smbinit.o

PNG_ASSETS = load0 load1 load2 load3 load4 load5 load6 load7 usb usb_bd ilk_bd \
	m4s_bd hdd_bd mmce hdd eth udp_bd udp_fs app cross triangle circle square select start left right \
	cover disc screen incebtion ip coverapp missing screens \
	ELF HDL ISO VCD ZSO UL APPS CD DVD Aspect_s Aspect_w Aspect_w1 \
	Aspect_w2 Rating_0 \
	Rating_1 Rating_2 Rating_3 Rating_4 Rating_5 Scan_240p Scan_240p1 Scan_480i Scan_480p \
	Scan_480p1 Scan_480p2 Scan_480p3 Scan_480p4 Scan_480p5 Scan_576i Scan_576p Scan_720p \
	Scan_1080i Scan_1080i2 Scan_1080p Vmode_multi Vmode_ntsc Vmode_pal logo \
	logo0 logo1 logo2 logo3 case\
	Index_0 Index_1 Index_2 Index_3 Index_4 fav fav_mark R3 L3 L1 R1 PS1 PS2 case_overlay
	# unused icons - up down l2 r2

# The RA cover mark is embedded ONLY in the RA flavour: the default build has no
# RA_MARK enum member and no table row for it, so embedding the bitmap there would
# be dead weight and would break the "default ELF is unaffected" acceptance gate.
# Must stay ahead of GFX_OBJS, which expands PNG_ASSETS.
ifeq ($(RETROACHIEVEMENTS),1)
PNG_ASSETS += ra_mark
endif

GFX_OBJS = $(PNG_ASSETS:%=%_png.o) poeveticanew.o icon_sys.o icon_icn.o

# NOTE: audio/bgm.ogg is intentionally NOT compiled into the ELF (saves ~324 KB).
# It is kept in the repo only as a reference/default track. BGM is loaded at
# runtime from a theme's sound/bgm.ogg, a configured BGM path, or the built-in
# theme's <OPL boot/CWD home>/bgm.ogg on non-APA boots and <gHDDPrefix>THM/bgm.ogg
# on APA boots (see sound.c bgmLoad), never embedded
# -- do not add bgm.o back here.
AUDIO_OBJS =	boot.o cancel.o confirm.o cursor.o message.o transition.o bd_connect.o bd_disconnect.o

MISC_OBJS =	icon_sys_A.o icon_sys_J.o icon_sys_C.o conf_theme_OPL.o theme_coverflow.o

TRANSLATIONS = Albanian Arabic Bulgarian Cebuano Croatian Czech Danish Dutch Filipino French \
	German Greek Hungarian Indonesian Italian Japanese Korean Laotian Persian Polish Portuguese \
	Portuguese_BR Romana Russian Ryukyuan SChinese Spanish Swedish TChinese Turkish Vietnamese

EE_BIN = opl.elf
EE_BIN_STRIPPED = opl_stripped.elf
EE_BIN_PACKED = RIPTOPL.ELF
EE_VPKD = RIPTOPL-$(OPL_VERSION)
EE_SRC_DIR = src/
EE_OBJS_DIR = obj/
EE_ASM_DIR = asm/
LNG_SRC_DIR = lng_src/
# Fork-maintained translation overlay (item 57). Fills gaps in the upstream language files for
# labels this fork added, without ever touching lng_src/ (which tools/download_lng.sh rewrites).
LNG_FORK_DIR = lng_fork/
LNG_TMPL_DIR = lng_tmpl/
LNG_DIR = lng/
PNG_ASSETS_DIR = gfx/

# Source the MMCE driver from the PS2SDK (as wOPL does) instead of downloading the rolling
# ps2-mmce/mmceman "latest". The SDK's mmceman is versioned in lockstep with the SDK's sio2man,
# so it never drifts out of sync -- the independent rolling release refactored its sio2man hook
# on 2026-06-14 and froze native MMCE launches on the pinned SDK. wOPL never hit this because
# it always took the coordinated SDK-bundled driver.
MMCE_ASSETS_DIR = $(PS2SDK)/iop/irx

MAPFILE = opl.map
EE_LDFLAGS += -Wl,-Map,$(MAPFILE)

EE_LIBS = -L$(PS2SDK)/ports/lib -L$(GSKIT)/lib -L./lib -lgskit -ldmakit -lpoweroff -lfileXio -lpatches -lpng -lz -lmc -lfreetype -lvux -lcdvd -lnetman -lps2ips -laudsrv -lvorbisfile -lvorbis -logg -lpadx -lelf-loader-nocolour
EE_INCS += -I$(PS2SDK)/ports/include -I$(PS2SDK)/ports/include/freetype2 -I$(GSKIT)/include -I$(GSKIT)/ee/dma/include -I$(GSKIT)/ee/gs/include -Imodules/iopcore/common -Imodules/network/common -Imodules/hdd/common -Iinclude
BIN2C = $(PS2SDK)/bin/bin2c

# WARNING: Only extra spaces are allowed and ignored at the beginning of the conditional directives (ifeq, ifneq, ifdef, ifndef, else and endif)
# but a tab is not allowed; if the line begins with a tab, it will be considered part of a recipe for a rule!

ifeq ($(RTL),1)
  EE_CFLAGS += -D__RTL
endif

ifeq ($(DTL_T10000),1)
  EE_CFLAGS += -D_DTL_T10000
  EECORE_EXTRA_FLAGS += DTL_T10000=1
  UDNL_OUT = $(PS2SDK)/iop/irx/udnl-t300.irx
else
  UDNL_OUT = $(PS2SDK)/iop/irx/udnl.irx
endif

# Experimental 1080p GSM mode: compile the table/GUI/gate (main EE build) and the ee_core raster
# handler (via EECORE_EXTRA_FLAGS -> ee_core/Makefile) only when explicitly requested.
ifeq ($(GSM1080P),1)
  EE_CFLAGS += -DGSM_1080P
  EECORE_EXTRA_FLAGS += GSM1080P=1
endif

ifeq ($(IGS),1)
  EE_CFLAGS += -DIGS
  IGS_FLAGS = IGS=1
else
  IGS_FLAGS = IGS=0
endif

ifeq ($(PADEMU),1)
  IOP_OBJS += bt_pademu.o usb_pademu.o ds34usb.o ds34bt.o libds34usb.a libds34bt.a
  EE_CFLAGS += -DPADEMU
  EE_INCS += -Imodules/ds34bt/ee -Imodules/ds34usb/ee
  PADEMU_FLAGS = PADEMU=1
else
  PADEMU_FLAGS = PADEMU=0
endif

# RetroAchievements flavour. Everything the feature adds hangs off this one switch, on BOTH sides
# of the EE/ee_core split -- the define reaches the menu build directly and ee_core through
# EECORE_EXTRA_FLAGS, because ee_core is a separate $(MAKE) invocation that inherits nothing.
# Grows one entry at a time as each phase lands its files, so the flavour always builds.
ifeq ($(RETROACHIEVEMENTS),1)
  EE_CFLAGS += -DRETROACHIEVEMENTS
  EECORE_EXTRA_FLAGS += RETROACHIEVEMENTS=1
  # md5 (vendored, zlib licence): the game hash the PC client keys achievements on.
  # rawatch: the watch list the menu hands to ee_core, loaded from <device>RA/.
  # rahash:  the image hash RetroAchievements keys a game on, taken by walking
  #          ISO9660 directly -- mounting hangs on USB past the 2 GB mark.
  # ranet:   the menu-side PC client exchange (hash -> watch list over UDP).
  # rabadge: the "tracked game" badge cache behind the list mark and cover mark.
  FRONTEND_OBJS += md5.o rawatch.o rahash.o ranet.o rabadge.o
  # The in-game telemetry sender. Hand-builds Ethernet/UDP frames and calls
  # SMAPSendPacket directly, so it deliberately bypasses the menu network stack.
  IOP_OBJS += raudp.o
endif

# A DEBUG build implies the field diagnostics: it already has a TTY, so withholding the on-screen
# labels from it would only make the two build types disagree about what they report.
ifeq ($(DEBUG),1)
  OPLDIAG = 1
endif

ifeq ($(OPLDIAG),1)
  EE_CFLAGS += -D__OPLDIAG
endif

ifeq ($(DEBUG),1)
  EE_CFLAGS += -D__DEBUG -g
  ifeq ($(DECI2_DEBUG),1)
    EE_OBJS += debug.o drvtif_irx.o tifinet_irx.o deci2_img.o
    EE_LDFLAGS += -liopreboot
  else ifeq ($(TTY_APPROACH),UDP)
    EE_OBJS += debug.o udptty.o ioptrap.o ps2link.o
    EE_CFLAGS += -DTTY_UDP
  else ifeq ($(TTY_APPROACH),PPC_UART)
    EE_OBJS += debug.o ppctty.o ioptrap.o
    EE_CFLAGS += -DTTY_PPC_UART
  else
	$(error Unknown value for TTY_APPROACH: '$(TTY_APPROACH)')
  endif
  MOD_DEBUG_FLAGS = DEBUG=1
  ifeq ($(IOPCORE_DEBUG),1)
    EE_CFLAGS += -D__INGAME_DEBUG
    EECORE_EXTRA_FLAGS += LOAD_DEBUG_MODULES=1
    CDVDMAN_DEBUG_FLAGS = IOPCORE_DEBUG=1
    MCEMU_DEBUG_FLAGS = IOPCORE_DEBUG=1
    SMSTCPIP_INGAME_CFLAGS =
    ifeq ($(TTY_APPROACH),UDP)
      IOP_OBJS += udptty-ingame.o
    endif
  else ifeq ($(EESIO_DEBUG),1)
    EE_CFLAGS += -D__EESIO_DEBUG
    EE_LIBS += -lsiocookie
  else ifeq ($(INGAME_DEBUG),1)
    EE_CFLAGS += -D__INGAME_DEBUG
    EECORE_EXTRA_FLAGS += LOAD_DEBUG_MODULES=1
    CDVDMAN_DEBUG_FLAGS = IOPCORE_DEBUG=1
    SMSTCPIP_INGAME_CFLAGS =
    ifeq ($(DECI2_DEBUG),1)
      EE_CFLAGS += -D__DECI2_DEBUG
      EECORE_EXTRA_FLAGS += DECI2_DEBUG=1
      IOP_OBJS += drvtif_ingame_irx.o tifinet_ingame_irx.o
      DECI2_DEBUG=1
      CDVDMAN_DEBUG_FLAGS = USE_DEV9=1 #(clear IOPCORE_DEBUG) dsidb cannot be used to handle exceptions or set breakpoints, so disable output to save resources.
    else ifeq ($(TTY_APPROACH),UDP)
      IOP_OBJS += udptty-ingame.o
      EECORE_EXTRA_FLAGS += "TTY_APPROACH=$(TTY_APPROACH)"
    else ifeq ($(TTY_APPROACH),PPC_UART)
      EECORE_EXTRA_FLAGS += "TTY_APPROACH=$(TTY_APPROACH)"
    endif
  endif
else
  EE_CFLAGS += -O2
  SMSTCPIP_INGAME_CFLAGS = INGAME_DRIVER=1
endif

# RetroAchievements: turn UDP on in the in-game lwIP build. Set here rather than
# in the RA block above because SMSTCPIP_INGAME_CFLAGS is assigned (not appended)
# by every branch of the debug block, so anything set earlier would be discarded.
# The bumped heap and pool sizes in SMSTCPIP/include/lwipopts.h hang off the same
# INGAME_UDP define -- MEM_SIZE 0x400 was a single 536-byte packet away from
# starving the game's own SMB stream and rebooting the console.
ifeq ($(RETROACHIEVEMENTS),1)
  SMSTCPIP_INGAME_CFLAGS += INGAME_UDP=1
endif

EE_CFLAGS += -fsingle-precision-constant -DOPL_VERSION=\"$(OPL_VERSION)\"

# There are a few places where the config key/value are truncated, so disable these warnings
EE_CFLAGS += -Wno-format-truncation -Wno-stringop-truncation
# Generate .d files to track header file dependencies of each object file
EE_CFLAGS += -MMD -MP
EE_OBJS += $(FRONTEND_OBJS) $(GFX_OBJS) $(AUDIO_OBJS) $(MISC_OBJS) $(EECORE_OBJS) $(IOP_OBJS)
EE_OBJS := $(EE_OBJS:%=$(EE_OBJS_DIR)%)
EE_DEPS = $($(filter %.o,$(EE_OBJS)):%.o=%.d)

# To help linking getting rid off unused functions and data
EE_CFLAGS += -fdata-sections -ffunction-sections
EE_LDFLAGS += -fdata-sections -ffunction-sections -Wl,--gc-sections

.SILENT:

.PHONY: all release debug iopcore_debug eesio_debug ingame_debug deci2_debug debug_ppctty iopcore_ppctty_debug ingame_ppctty_debug clean rebuild pc_tools pc_tools_win32 oplversion format format-check ps2sdk-not-setup download_lng download_lwNBD languages

ifdef PS2SDK

all: download_lng download_lwNBD languages
	echo "Building Open PS2 Loader $(OPL_VERSION)..."
	echo "-Interface"
ifneq ($(NOT_PACKED),1)
	$(MAKE) $(EE_BIN_PACKED)
else
	$(MAKE) $(EE_BIN)
endif

release: download_lng download_lwNBD languages $(EE_VPKD).ZIP

debug:
	$(MAKE) DEBUG=1 all

iopcore_debug:
	$(MAKE) DEBUG=1 IOPCORE_DEBUG=1 all

eesio_debug:
	$(MAKE) DEBUG=1 EESIO_DEBUG=1 all

ingame_debug:
	$(MAKE) DEBUG=1 INGAME_DEBUG=1 all

deci2_debug:
	$(MAKE) DEBUG=1 INGAME_DEBUG=1 DECI2_DEBUG=1 all

debug_ppctty:
	$(MAKE) DEBUG=1 TTY_APPROACH=PPC_UART all

iopcore_ppctty_debug:
	$(MAKE) DEBUG=1 IOPCORE_DEBUG=1 TTY_APPROACH=PPC_UART all

ingame_ppctty_debug:
	$(MAKE) DEBUG=1 INGAME_DEBUG=1 TTY_APPROACH=PPC_UART all

clean:	download_lwNBD
	echo "Cleaning..."
	echo "-Interface"
	rm -fr $(MAPFILE) $(EE_BIN) $(EE_BIN_PACKED) $(EE_BIN_STRIPPED) $(EE_VPKD).* $(EE_OBJS_DIR) $(EE_ASM_DIR)
	echo "-EE core"
	$(MAKE) -C ee_core clean
	echo "-ELF loader"
	$(MAKE) -C elfldr clean
	echo "-IOP core"
	echo " -imgdrv"
	$(MAKE) -C modules/iopcore/imgdrv clean
	echo " -cdvdman"
	$(MAKE) -C modules/iopcore/cdvdman USE_BDM=1 clean
	$(MAKE) -C modules/iopcore/cdvdman USE_BDM_ATA=1 clean
	$(MAKE) -C modules/iopcore/cdvdman USE_MMCE=1 clean
	$(MAKE) -C modules/iopcore/cdvdman USE_SMB=1 clean
	$(MAKE) -C modules/iopcore/cdvdman USE_HDD=1 clean
	$(MAKE) -C modules/iopcore/cdvdman USE_HDPRO=1 clean
	echo " -cdvdfsv"
	$(MAKE) -C modules/iopcore/cdvdfsv clean
	echo " -resetspu"
	$(MAKE) -C modules/iopcore/resetspu clean
	echo "  -patches"
	echo "   -iremsnd"
	$(MAKE) -C modules/iopcore/patches/iremsndpatch clean
	echo "   -apemod"
	$(MAKE) -C modules/iopcore/patches/apemodpatch clean
	echo "   -f2techiop"
	$(MAKE) -C modules/iopcore/patches/f2techioppatch clean
	echo "   -cleareffects"
	$(MAKE) -C modules/iopcore/patches/cleareffects clean
	echo " -isofs"
	$(MAKE) -C modules/isofs clean
	echo " -bdmevent"
	$(MAKE) -C modules/bdmevent clean
	echo " -SMSUTILS"
	$(MAKE) -C modules/network/SMSUTILS clean
	echo " -SMSTCPIP"
	$(MAKE) -C modules/network/SMSTCPIP clean
	echo " -in-game SMAP"
	$(MAKE) -C modules/network/smap-ingame clean
	echo " -smap_udpbd"
	$(MAKE) -C modules/network/smap_udpbd clean
	echo " -udpfs (smap/ministack/bd)"
	$(MAKE) -C modules/network/udpfs_smap clean
	$(MAKE) -C modules/network/udpfs_ministack clean
	$(MAKE) -C modules/network/udpfs_bd clean
	$(MAKE) -C modules/network/udpfs_ioman clean
	echo " -smbinit"
	$(MAKE) -C modules/network/smbinit clean
	echo " -nbns"
	$(MAKE) -C modules/network/nbns clean
	echo " -httpclient"
	$(MAKE) -C modules/network/httpclient clean
	echo " -atad (ata_bd)"
	$(MAKE) -C modules/hdd/atad clean
	echo " -xhdd"
	$(MAKE) -C modules/hdd/xhdd clean
	echo " -mcemu"
	$(MAKE) -C modules/mcemu USE_BDM=1 clean
	$(MAKE) -C modules/mcemu USE_MMCE=1 clean
	$(MAKE) -C modules/mcemu USE_HDD=1 clean
	$(MAKE) -C modules/mcemu USE_SMB=1 clean
	echo " -genvmc"
	$(MAKE) -C modules/vmc/genvmc clean
	echo " -lwnbdsvr"
	$(MAKE) -C modules/network/lwNBD/ TARGET=iop clean
	echo " -udptty-ingame"
	$(MAKE) -C modules/debug/udptty-ingame clean
	echo " -ps2link"
	$(MAKE) -C modules/debug/ps2link clean
	echo " -ds34usb"
	$(MAKE) -C modules/ds34usb clean
	echo " -ds34bt"
	$(MAKE) -C modules/ds34bt clean
	echo " -pademu"
	$(MAKE) -C modules/pademu USE_BT=1 clean
	$(MAKE) -C modules/pademu USE_USB=1 clean
	echo " -ps2ips"
	$(MAKE) -C modules/network/ps2ips clean
	echo " -raudp"
	$(MAKE) -C modules/network/raudp clean
	echo "-pc tools"
	$(MAKE) -C pc clean

realclean: clean
	echo "-Language"
	rm -fr $(LNG_SRC_DIR) $(LNG_DIR)lang_*.lng $(INTERNAL_LANGUAGE_C) $(INTERNAL_LANGUAGE_H)

rebuild: clean all

run: $(EE_BIN_PACKED)
	ps2client -h 192.168.1.10 execee host:$<

sim: $(EE_BIN_PACKED)
	PCSX2 --elf=$(PWD)/$< --nodisc --nogui

pc_tools:
	echo "Building iso2opl, opl2iso and genvmc..."
	$(MAKE) _WIN32=0 -C pc

pc_tools_win32:
	echo "Building WIN32 iso2opl, opl2iso and genvmc..."
	$(MAKE) _WIN32=1 -C pc

cfla = "thirdparty/clang-format-lint-action"
format-check: download_cfla
	@python3 $(cfla)/run-clang-format.py --clang-format-executable $(cfla)/clang-format/clang-format12 -r .

format: download_cfla
	@python3 $(cfla)/run-clang-format.py --clang-format-executable $(cfla)/clang-format/clang-format12 -r . -i true

$(EE_ASM_DIR):
	@mkdir -p $@

$(EE_OBJS_DIR):
	@mkdir -p $@

.PHONY: DETAILED_CHANGELOG
DETAILED_CHANGELOG:
	sh tools/make_changelog.sh

$(EE_BIN_STRIPPED): $(EE_BIN)
	echo "Stripping..."
	$(EE_STRIP) -o $@ $<

$(EE_BIN_PACKED): $(EE_BIN_STRIPPED)
	echo "Compressing..."
	ps2-packer $< $@ > /dev/null

$(EE_VPKD).ELF: $(EE_BIN_PACKED)
	cp -f $< $@

$(EE_VPKD).ZIP: $(EE_VPKD).ELF DETAILED_CHANGELOG CREDITS LICENSE README.md
	zip -r $@ $^
	echo "Package Complete: $@"

ee_core/ee_core.elf: ee_core
	echo "-EE core"
	$(MAKE) $(IGS_FLAGS) $(PADEMU_FLAGS) $(EECORE_EXTRA_FLAGS) -C $<

$(EE_ASM_DIR)ee_core.c: ee_core/ee_core.elf | $(EE_ASM_DIR)
	$(BIN2C) $< $@ eecore_elf

elfldr/elfldr.elf: elfldr
	echo "-ELF loader (no-IOP-reset handoff)"
	$(MAKE) -C $<

$(EE_ASM_DIR)elfldr.c: elfldr/elfldr.elf | $(EE_ASM_DIR)
	$(BIN2C) $< $@ elfldr_elf

$(EE_ASM_DIR)udnl.c: $(UDNL_OUT) | $(EE_ASM_DIR)
	$(BIN2C) $(UDNL_OUT) $@ udnl_irx

$(EE_ASM_DIR)mmceman.c: $(MMCE_ASSETS_DIR)/mmceman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)mmcedrv.c: $(MMCE_ASSETS_DIR)/mmcedrv.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)mmceigr.c: $(MMCE_ASSETS_DIR)/mmceigr.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/imgdrv/imgdrv.irx: modules/iopcore/imgdrv
	$(MAKE) -C $<

$(EE_ASM_DIR)imgdrv.c: modules/iopcore/imgdrv/imgdrv.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)eesync.c: $(PS2SDK)/iop/irx/eesync-nano.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

# RetroAchievements needs the network even when the game runs from USB, and the
# in-game DEV9 driver lives inside cdvdman -- upstream enables it for the BDM
# variant only under IOPCORE_DEBUG (modules/iopcore/cdvdman/Makefile). Without
# DEV9 SMAP cannot start and raudp fails to load with -200 (E_IOP_DEPENDANCY).
# Conditional so the default build's bdm_cdvdman.irx does not grow a driver it
# has no use for.
ifeq ($(RETROACHIEVEMENTS),1)
CDVDMAN_RA_FLAGS = USE_DEV9=1
endif

modules/iopcore/cdvdman/bdm_cdvdman.irx: modules/iopcore/cdvdman
	$(MAKE) $(CDVDMAN_PS2LOGO_FLAGS) $(CDVDMAN_DEBUG_FLAGS) $(CDVDMAN_RA_FLAGS) USE_BDM=1 -C $< all

$(EE_ASM_DIR)bdm_cdvdman.c: modules/iopcore/cdvdman/bdm_cdvdman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/cdvdman/bdm_ata_cdvdman.irx: modules/iopcore/cdvdman
	$(MAKE) $(CDVDMAN_PS2LOGO_FLAGS) $(CDVDMAN_DEBUG_FLAGS) USE_BDM_ATA=1 -C $< all

$(EE_ASM_DIR)bdm_ata_cdvdman.c: modules/iopcore/cdvdman/bdm_ata_cdvdman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ bdm_ata_cdvdman_irx

modules/iopcore/cdvdman/mmce_cdvdman.irx: modules/iopcore/cdvdman
	$(MAKE) $(CDVDMAN_PS2LOGO_FLAGS) $(CDVDMAN_DEBUG_FLAGS) USE_MMCE=1 -C $< all

$(EE_ASM_DIR)mmce_cdvdman.c: modules/iopcore/cdvdman/mmce_cdvdman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/cdvdman/smb_cdvdman.irx: modules/iopcore/cdvdman
	$(MAKE) $(CDVDMAN_PS2LOGO_FLAGS) $(CDVDMAN_DEBUG_FLAGS) USE_SMB=1 -C $< all

$(EE_ASM_DIR)smb_cdvdman.c: modules/iopcore/cdvdman/smb_cdvdman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/cdvdman/hdd_cdvdman.irx: modules/iopcore/cdvdman
	$(MAKE) $(CDVDMAN_PS2LOGO_FLAGS) $(CDVDMAN_DEBUG_FLAGS) USE_HDD=1 -C $< all

$(EE_ASM_DIR)hdd_cdvdman.c: modules/iopcore/cdvdman/hdd_cdvdman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/cdvdman/hdd_hdpro_cdvdman.irx: modules/iopcore/cdvdman
	$(MAKE) $(CDVDMAN_PS2LOGO_FLAGS) $(CDVDMAN_DEBUG_FLAGS) USE_HDPRO=1 -C $< all

$(EE_ASM_DIR)hdd_hdpro_cdvdman.c: modules/iopcore/cdvdman/hdd_hdpro_cdvdman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/cdvdfsv/cdvdfsv.irx: modules/iopcore/cdvdfsv
	$(MAKE) -C $<

$(EE_ASM_DIR)cdvdfsv.c: modules/iopcore/cdvdfsv/cdvdfsv.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/patches/iremsndpatch/iremsndpatch.irx: modules/iopcore/patches/iremsndpatch
	$(MAKE) -C $<

$(EE_ASM_DIR)iremsndpatch.c: modules/iopcore/patches/iremsndpatch/iremsndpatch.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/patches/apemodpatch/apemodpatch.irx: modules/iopcore/patches/apemodpatch
	$(MAKE) -C $<

$(EE_ASM_DIR)apemodpatch.c: modules/iopcore/patches/apemodpatch/apemodpatch.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/patches/f2techioppatch/f2techioppatch.irx: modules/iopcore/patches/f2techioppatch
	$(MAKE) -C $<

$(EE_ASM_DIR)f2techioppatch.c: modules/iopcore/patches/f2techioppatch/f2techioppatch.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/patches/cleareffects/cleareffects.irx: modules/iopcore/patches/cleareffects
	$(MAKE) -C $<

$(EE_ASM_DIR)cleareffects.c: modules/iopcore/patches/cleareffects/cleareffects.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/iopcore/resetspu/resetspu.irx: modules/iopcore/resetspu
	$(MAKE) -C $<

$(EE_ASM_DIR)resetspu.c: modules/iopcore/resetspu/resetspu.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/mcemu/bdm_mcemu.irx: modules/mcemu
	$(MAKE) $(MCEMU_DEBUG_FLAGS) $(PADEMU_FLAGS) USE_BDM=1 -C $< all

$(EE_ASM_DIR)bdm_mcemu.c: modules/mcemu/bdm_mcemu.irx
	$(BIN2C) $< $@ $(*F)_irx

modules/mcemu/mmce_mcemu.irx: modules/mcemu
	$(MAKE) $(MCEMU_DEBUG_FLAGS) $(PADEMU_FLAGS) USE_MMCE=1 -C $< all

$(EE_ASM_DIR)mmce_mcemu.c: modules/mcemu/mmce_mcemu.irx
	$(BIN2C) $< $@ $(*F)_irx

modules/mcemu/hdd_mcemu.irx: modules/mcemu
	$(MAKE) $(MCEMU_DEBUG_FLAGS) $(PADEMU_FLAGS) USE_HDD=1 -C $< all

$(EE_ASM_DIR)hdd_mcemu.c: modules/mcemu/hdd_mcemu.irx
	$(BIN2C) $< $@ $(*F)_irx

modules/mcemu/smb_mcemu.irx: modules/mcemu
	$(MAKE) $(MCEMU_DEBUG_FLAGS) $(PADEMU_FLAGS) USE_SMB=1 -C $< all

$(EE_ASM_DIR)smb_mcemu.c: modules/mcemu/smb_mcemu.irx
	$(BIN2C) $< $@ $(*F)_irx

modules/isofs/isofs.irx: modules/isofs
	$(MAKE) -C $<

$(EE_ASM_DIR)isofs.c: modules/isofs/isofs.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)usbd.c: $(PS2SDK)/iop/irx/usbd_mini.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)libsd.c: $(PS2SDK)/iop/irx/libsd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)audsrv.c: $(PS2SDK)/iop/irx/audsrv.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_OBJS_DIR)libds34bt.a: modules/ds34bt/ee/libds34bt.a
	cp $< $@

modules/ds34bt/ee/libds34bt.a: modules/ds34bt/ee
	$(MAKE) -C $<

modules/ds34bt/iop/ds34bt.irx: modules/ds34bt/iop
	$(MAKE) -C $<

$(EE_ASM_DIR)ds34bt.c: modules/ds34bt/iop/ds34bt.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_OBJS_DIR)libds34usb.a: modules/ds34usb/ee/libds34usb.a
	cp $< $@

modules/ds34usb/ee/libds34usb.a: modules/ds34usb/ee
	$(MAKE) -C $<

modules/ds34usb/iop/ds34usb.irx: modules/ds34usb/iop
	$(MAKE) -C $< DUALSENSE=$(DUALSENSE)

$(EE_ASM_DIR)ds34usb.c: modules/ds34usb/iop/ds34usb.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/pademu/bt_pademu.irx: modules/pademu
	$(MAKE) -C $< USE_BT=1

$(EE_ASM_DIR)bt_pademu.c: modules/pademu/bt_pademu.irx
	$(BIN2C) $< $@ $(*F)_irx

modules/pademu/usb_pademu.irx: modules/pademu
	$(MAKE) -C $< USE_USB=1

$(EE_ASM_DIR)usb_pademu.c: modules/pademu/usb_pademu.irx
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)bdm.c: $(PS2SDK)/iop/irx/bdm.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)bdmfs_fatfs.c: $(PS2SDK)/iop/irx/bdmfs_fatfs.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)iLinkman.c: $(PS2SDK)/iop/irx/iLinkman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

ifeq ($(DEBUG),1)
# block device drivers with printf's
$(EE_ASM_DIR)usbmass_bd.c: $(PS2SDK)/iop/irx/usbmass_bd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)IEEE1394_bd.c: $(PS2SDK)/iop/irx/IEEE1394_bd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)mx4sio_bd.c: $(PS2SDK)/iop/irx/mx4sio_bd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx
else
# block device drivers without printf's
$(EE_ASM_DIR)usbmass_bd.c: $(PS2SDK)/iop/irx/usbmass_bd_mini.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)IEEE1394_bd.c: $(PS2SDK)/iop/irx/IEEE1394_bd_mini.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)mx4sio_bd.c: $(PS2SDK)/iop/irx/mx4sio_bd_mini.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx
endif

modules/bdmevent/bdmevent.irx: modules/bdmevent
	$(MAKE) -C $<

$(EE_ASM_DIR)bdmevent.c: modules/bdmevent/bdmevent.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)ps2dev9.c: $(PS2SDK)/iop/irx/ps2dev9.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/SMSUTILS/SMSUTILS.irx: modules/network/SMSUTILS
	$(MAKE) -C $<

$(EE_ASM_DIR)smsutils.c: modules/network/SMSUTILS/SMSUTILS.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)ps2ip.c: $(PS2SDK)/iop/irx/ps2ip-nm.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/SMSTCPIP/SMSTCPIP.irx: modules/network/SMSTCPIP
	$(MAKE) $(SMSTCPIP_INGAME_CFLAGS) -C $< rebuild

$(EE_ASM_DIR)ingame_smstcpip.c: modules/network/SMSTCPIP/SMSTCPIP.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/smap-ingame/smap.irx: modules/network/smap-ingame
	$(MAKE) -C $<

$(EE_ASM_DIR)smap_ingame.c: modules/network/smap-ingame/smap.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/smap_udpbd/smap_udpbd.irx: modules/network/smap_udpbd
	$(MAKE) -C $<

$(EE_ASM_DIR)smap_udpbd.c: modules/network/smap_udpbd/smap_udpbd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

# RetroAchievements telemetry sender. Listed dependencies rather than a bare
# directory prerequisite so editing raudp.c actually rebuilds the blob: the .irx
# is embedded into the ELF, and a stale one is invisible until it misbehaves on
# hardware. `rebuild` (clean+all) because this module's objects are shared with
# nothing and a partial rebuild here is not worth the risk.
RAUDP_DEPS := $(wildcard modules/network/raudp/*.c) $(wildcard modules/network/raudp/*.h) \
              modules/network/raudp/imports.lst modules/network/raudp/exports.tab \
              modules/network/raudp/Makefile

modules/network/raudp/raudp.irx: $(RAUDP_DEPS) | modules/network/raudp
	$(MAKE) -C modules/network/raudp rebuild

$(EE_ASM_DIR)raudp.c: modules/network/raudp/raudp.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ raudp_irx

modules/network/udpfs_smap/udpfs_smap.irx: modules/network/udpfs_smap
	$(MAKE) -C $<

$(EE_ASM_DIR)udpfs_smap.c: modules/network/udpfs_smap/udpfs_smap.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/udpfs_ministack/udpfs_ministack.irx: modules/network/udpfs_ministack
	$(MAKE) -C $<

$(EE_ASM_DIR)udpfs_ministack.c: modules/network/udpfs_ministack/udpfs_ministack.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/udpfs_bd/udpfs_bd.irx: modules/network/udpfs_bd
	$(MAKE) -C $<

$(EE_ASM_DIR)udpfs_bd.c: modules/network/udpfs_bd/udpfs_bd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/udpfs_ioman/udpfs_ioman.irx: modules/network/udpfs_ioman
	$(MAKE) -C $<

$(EE_ASM_DIR)udpfs_ioman.c: modules/network/udpfs_ioman/udpfs_ioman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)smap.c: $(PS2SDK)/iop/irx/smap.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)netman.c: $(PS2SDK)/iop/irx/netman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

# FORK-VENDORED ps2ips (modules/network/ps2ips): the ps2sdk prebuilt has four defects in the
# EE<->IOP socket bridge, and the first one corrupts EE memory in any build that opens a socket.
#
#   * do_recvfrom DMAs the whole 144-byte rests_pkt into the EE's _intr_data[32], which is 128
#     bytes. In the LINKED OPL the next object in .bss is __ps2ipc_fdman_ops_socket -- the table of
#     function pointers behind every EE socket call -- and bytes 128..143 are exactly its getfd /
#     getfilename / close / read entries. Overwriting them makes close() stop calling the IOP, so
#     the lwIP socket leaks and the UDP PCB pool runs dry: roughly ONE network operation per boot.
#   * the reply sockaddr is written into the shared RPC buffer BEFORE the request's ee_addr and
#     intr_data are read, so both DMA transfers target EE address 0 -- EE-side UDP receive has
#     never worked.
#   * fromlen is passed uninitialised, so the sender address comes back zeroed.
#   * lwip_buffer is BUFF_SIZE + 32 where a receive at offset 64 needs BUFF_SIZE + 64.
#
# The source is current ps2sdk master with the four spots marked "RA fix" -- verified by diffing
# against ps2dev/ps2sdk master: 6 hunks, and every removed line is one of the defects above, so
# vendoring drops no upstream behaviour. It keeps the ps2ips.irx name and the same export table, so
# nbns, httpclient and everything else on the EE socket API bind exactly as before.
#
# Kept out of .clang-format-ignore's reach by that same reasoning: do NOT reformat it, or the diff
# against ps2sdk (and the chance of upstreaming this) is lost.
modules/network/ps2ips/ps2ips.irx: $(wildcard modules/network/ps2ips/*.c) $(wildcard modules/network/ps2ips/*.h) modules/network/ps2ips/imports.lst modules/network/ps2ips/Makefile
	$(MAKE) -C modules/network/ps2ips rebuild

$(EE_ASM_DIR)ps2ips.c: modules/network/ps2ips/ps2ips.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)smbman.c: $(PS2SDK)/iop/irx/smbman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/smbinit/smbinit.irx: modules/network/smbinit
	$(MAKE) -C $<

$(EE_ASM_DIR)smbinit.c: modules/network/smbinit/smbinit.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

# FORK-VENDORED atad (modules/hdd/atad, see its ORIGIN.txt): the SDK prebuilt latches a failed
# first probe forever (the "40x: HardDisk Drive not detected" all-session APA death); ours makes
# sceAtaInit retryable so the EE retry loop + xhdd's re-probe devctl genuinely heal. Generated file
# keeps the ps2atad.c name -> symbol ps2atad_irx -> zero EE-side changes.
modules/hdd/atad/ata_bd.irx: modules/hdd/atad
	$(MAKE) -C $<

$(EE_ASM_DIR)ps2atad.c: modules/hdd/atad/ata_bd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)hdpro_atad.c: $(PS2SDK)/iop/irx/hdproatad.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)poweroff.c: $(PS2SDK)/iop/irx/poweroff.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/hdd/xhdd/xhdd.irx: modules/hdd/xhdd
	$(MAKE) -C $<

$(EE_ASM_DIR)xhdd.c: modules/hdd/xhdd/xhdd.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)ps2hdd.c: $(PS2SDK)/iop/irx/ps2hdd-osd.irx
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)ps2fs.c: $(PS2SDK)/iop/irx/ps2fs-osd.irx
	$(BIN2C) $< $@ $(*F)_irx

modules/vmc/genvmc/genvmc.irx: modules/vmc/genvmc
	$(MAKE) $(MOD_DEBUG_FLAGS) -C $<

$(EE_ASM_DIR)genvmc.c: modules/vmc/genvmc/genvmc.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/lwNBD/lwnbdsvr.irx: modules/network/lwNBD
	$(MAKE) TARGET=iop -C $<

$(EE_ASM_DIR)lwnbdsvr.c: modules/network/lwNBD/lwnbdsvr.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)udptty.c: $(PS2SDK)/iop/irx/udptty.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)ppctty.c: $(PS2SDK)/iop/irx/ppctty.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/debug/udptty-ingame/udptty.irx: modules/debug/udptty-ingame
	$(MAKE) -C $<

$(EE_ASM_DIR)udptty-ingame.c: modules/debug/udptty-ingame/udptty.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ udptty_ingame_irx

$(EE_ASM_DIR)ioptrap.c: $(PS2SDK)/iop/irx/ioptrap.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/debug/ps2link/ps2link.irx: modules/debug/ps2link
	$(MAKE) -C $<

$(EE_ASM_DIR)ps2link.c: modules/debug/ps2link/ps2link.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

modules/network/nbns/nbns.irx: modules/network/nbns
	$(MAKE) -C $<

$(EE_ASM_DIR)nbns-iop.c: modules/network/nbns/nbns.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ nbns_irx

modules/network/httpclient/httpclient.irx: modules/network/httpclient
	$(MAKE) -C $<

$(EE_ASM_DIR)httpclient-iop.c: modules/network/httpclient/httpclient.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ httpclient_irx

$(EE_ASM_DIR)iomanx.c: $(PS2SDK)/iop/irx/iomanX.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)filexio.c: $(PS2SDK)/iop/irx/fileXio.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)sio2man.c: $(PS2SDK)/iop/irx/freesio2.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)padman.c: $(PS2SDK)/iop/irx/freepad.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)mcman.c: $(PS2SDK)/iop/irx/mcman.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)mcserv.c: $(PS2SDK)/iop/irx/mcserv.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_irx

$(EE_ASM_DIR)poeveticanew.c: thirdparty/PoeVeticaNew.ttf | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_raw

$(EE_ASM_DIR)icon_sys.c: gfx/icon.sys | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)icon_icn.c: gfx/opl.icn | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)icon_sys_A.c: misc/icon_A.sys | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)icon_sys_J.c: misc/icon_J.sys | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)icon_sys_C.c: misc/icon_C.sys | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)conf_theme_OPL.c: misc/conf_theme_OPL.cfg | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_cfg

$(EE_ASM_DIR)theme_coverflow.c: misc/theme_coverflow.cfg | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_cfg

$(EE_ASM_DIR)boot.c: audio/boot.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)cancel.c: audio/cancel.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)confirm.c: audio/confirm.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)cursor.c: audio/cursor.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)message.c: audio/message.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)transition.c: audio/transition.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)bd_connect.c: audio/bd_connect.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)bd_disconnect.c: audio/bd_disconnect.adp | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)_adp

$(EE_ASM_DIR)IOPRP_img.c: modules/iopcore/IOPRP.img | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)drvtif_ingame_irx.c: modules/debug/drvtif-ingame.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)tifinet_ingame_irx.c: modules/debug/tifinet-ingame.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)drvtif_irx.c: modules/debug/drvtif.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)tifinet_irx.c: modules/debug/tifinet.irx | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_ASM_DIR)deci2_img.c: modules/debug/deci2.img | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(*F)

$(EE_OBJS_DIR)%.o: $(EE_SRC_DIR)%.c | $(EE_OBJS_DIR)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(EE_OBJS_DIR)%.o: $(EE_ASM_DIR)%.c | $(EE_OBJS_DIR)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(PNG_ASSETS:%=$(EE_ASM_DIR)%_png.c): $(EE_ASM_DIR)%_png.c: $(PNG_ASSETS_DIR)%.png | $(EE_ASM_DIR)
	$(BIN2C) $< $@ $(@:$(EE_ASM_DIR)%.c=%)

endif

TRANSLATIONS_LNG = $(TRANSLATIONS:%=$(LNG_DIR)lang_%.lng)
TRANSLATIONS_YML = $(TRANSLATIONS:%=$(LNG_SRC_DIR)%.yml)
ENGLISH_TEMPLATE_YML = $(LNG_SRC_DIR)English.yml
ENGLISH_LNG = $(LNG_SRC_DIR)lang_English.lng
BASE_LANGUAGE = $(LNG_TMPL_DIR)_base.yml
INTERNAL_LANGUAGE_C = src/lang_internal.c
INTERNAL_LANGUAGE_H = include/lang_autogen.h
LANG_COMPILER = tools/lang_compiler.py

languages: $(ENGLISH_TEMPLATE_YML) $(TRANSLATIONS_YML) $(ENGLISH_LNG) $(TRANSLATIONS_LNG) $(INTERNAL_LANGUAGE_C) $(INTERNAL_LANGUAGE_H)

download_lng:
	./tools/download_lng.sh

download_lwNBD:
	./tools/download_lwNBD.sh

download_cfla:
	./tools/download_cfla.sh

$(TRANSLATIONS_LNG): $(LNG_DIR)lang_%.lng: $(LNG_SRC_DIR)%.yml $(BASE_LANGUAGE) $(LANG_COMPILER)
	@if [ -f $(LNG_FORK_DIR)$*.yml ]; then python3 $(LANG_COMPILER) --overlay_translation_yml --base $(BASE_LANGUAGE) --translation $< --overlay $(LNG_FORK_DIR)$*.yml; fi
	python3 $(LANG_COMPILER) --make_lng --base $(BASE_LANGUAGE) --translation $< $@

$(TRANSLATIONS_YML): %.yml: $(BASE_LANGUAGE) $(LANG_COMPILER)
	python3 $(LANG_COMPILER) --update_translation_yml --base $(BASE_LANGUAGE) --translation $@

$(ENGLISH_TEMPLATE_YML): $(BASE_LANGUAGE) $(LANG_COMPILER)
	python3 $(LANG_COMPILER) --make_template_yml --base $< $@

$(ENGLISH_LNG): $(ENGLISH_TEMPLATE_YML) $(BASE_LANGUAGE) $(LANG_COMPILER)
	python3 $(LANG_COMPILER) --make_lng --base $(BASE_LANGUAGE) --translation $< $@

$(INTERNAL_LANGUAGE_C): $(BASE_LANGUAGE) $(LANG_COMPILER)
	python3 $(LANG_COMPILER) --make_source --base $< $@

$(INTERNAL_LANGUAGE_H): $(BASE_LANGUAGE) $(LANG_COMPILER)
	python3 $(LANG_COMPILER) --make_header --base $< $@

ifndef PS2SDK
ps2sdk-not-setup:
	@echo "PS2SDK is not setup. Please setup PS2SDK before building this project"
endif

oplversion:
	@echo $(OPL_VERSION)

ifdef PS2SDK
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
endif

-include $(EE_DEPS)
