# PS2 Dev Channel Stub
# Requires ps2sdk and ee toolchain (ee-gcc, ee-ld, ps2-packer)
#
# Set PS2SDK to your sdk root, e.g.:
#   export PS2SDK=/usr/local/ps2dev/ps2sdk
#
# Build:
#   make
#
# Output:
#   ps2dev_stub.elf   — load this onto the SD2PSX SD card
#   ps2dev_stub_packed.elf — compressed version (smaller, faster FMCB boot)

PS2SDK  ?= $(PS2DEV)/ps2sdk
PS2DEV  ?= /usr/local/ps2dev

EE_BIN   = ps2dev_stub.elf
EE_OBJS  = stub_main.o mc_access.o stdout_ring.o

EE_INCS  = -I$(PS2SDK)/ee/include \
            -I$(PS2SDK)/common/include \
            -I.

EE_LIBS  = -lmc -lkernel -lc -lm

EE_CFLAGS  = -O2 -G0 -mno-gpopt -Wall -Wextra \
              -fno-common \
              $(EE_INCS)

EE_LDFLAGS = -T$(PS2SDK)/ee/startup/linkfile \
              -L$(PS2SDK)/ee/lib \
              $(EE_LIBS)

EE_CC  = ee-gcc
EE_LD  = ee-gcc
EE_STRIP = ee-strip

.PHONY: all clean pack

all: $(EE_BIN)

$(EE_BIN): $(EE_OBJS)
	$(EE_LD) $(EE_CFLAGS) -o $@ $(EE_OBJS) $(EE_LDFLAGS)

%.o: %.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

# Optional: pack with ps2-packer for faster FMCB load
pack: $(EE_BIN)
	ps2-packer $(EE_BIN) ps2dev_stub_packed.elf

clean:
	rm -f $(EE_OBJS) $(EE_BIN) ps2dev_stub_packed.elf
