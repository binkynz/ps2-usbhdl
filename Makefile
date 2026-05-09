EE_BIN = ps2-usbhdl.elf

EE_SRC_DIR   = src
EE_BUILD_DIR = build

# IOP modules embedded into the EE ELF as binary blobs (via bin2c).
# These need to be loaded from RAM because we can't read from USB
# until usbd/bdm/bdmfs is loaded — which themselves need to come
# from somewhere; bundling them in the ELF avoids the bootstrap
# problem entirely.
IRX_NAMES = iomanX fileXio poweroff ps2dev9 ps2atad ps2hdd
IRX_OBJS  = $(addprefix $(EE_BUILD_DIR)/, $(addsuffix _irx.o, $(IRX_NAMES)))

EE_OBJS = $(EE_BUILD_DIR)/main.o $(IRX_OBJS)

EE_LIBS = -ldebug -lhdd -lpoweroff -lfileXio

all: $(EE_BIN)

# bin2c turns a .irx into a C file with `unsigned char <name>[]`
# and `unsigned int size_<name>` symbols.
$(EE_BUILD_DIR)/%_irx.c: $(PS2SDK)/iop/irx/%.irx
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
