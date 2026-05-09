EE_BIN = dist/ps2-usbhdl.elf

EE_SRC_DIR   = src
EE_BUILD_DIR = build

# IOP modules embedded into the EE ELF as binary blobs (via bin2c).
# These need to be loaded from RAM because we can't read from USB
# until usbd/bdm/bdmfs is loaded — which themselves need to come
# from somewhere; bundling them in the ELF avoids the bootstrap
# problem entirely.
# Standard PS2SDK-shipped IRXes.
PS2SDK_IRX_NAMES = iomanX fileXio poweroff ps2dev9 ps2atad \
                   usbd bdm bdmfs_fatfs usbmass_bd \
                   sio2man padman

# IRXes from HDLGameInstaller, vendored under vendor/irx/.
# We use ps2hdd_hdl.irx (the HDL-aware fork) instead of the standard
# ps2hdd.irx, plus hdlfs.irx for the hdl0: device.
VENDOR_IRX_NAMES = ps2hdd_hdl hdlfs

IRX_NAMES = $(PS2SDK_IRX_NAMES) $(VENDOR_IRX_NAMES)
IRX_OBJS  = $(addprefix $(EE_BUILD_DIR)/, $(addsuffix _irx.o, $(IRX_NAMES)))

EE_OBJS = $(EE_BUILD_DIR)/main.o \
          $(EE_BUILD_DIR)/iop.o \
          $(EE_BUILD_DIR)/iso.o \
          $(EE_BUILD_DIR)/hdl.o \
          $(EE_BUILD_DIR)/ui.o \
          $(IRX_OBJS)

EE_LIBS = -ldebug -lhdd -lpoweroff -lfileXio -lpatches -lpad

all: $(EE_BIN)

# bin2c turns a .irx into a C file with `unsigned char <name>[]`
# and `unsigned int size_<name>` symbols. We have two source paths
# (PS2SDK's installed IRX dir, and our vendored HDLGameInstaller
# IRXes); make picks the one with an existing prerequisite.
$(EE_BUILD_DIR)/%_irx.c: $(PS2SDK)/iop/irx/%.irx
	@mkdir -p $(@D)
	bin2c $< $@ $*_irx

$(EE_BUILD_DIR)/%_irx.c: vendor/irx/%.irx
	@mkdir -p $(@D)
	bin2c $< $@ $*_irx

# Compile our own sources from src/ into build/.
$(EE_BUILD_DIR)/%.o: $(EE_SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# Compile generated sources (bin2c output) from build/ into build/.
$(EE_BUILD_DIR)/%.o: $(EE_BUILD_DIR)/%.c
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

clean:
	rm -rf $(EE_BIN) $(EE_BUILD_DIR)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
