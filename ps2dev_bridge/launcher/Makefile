# PS2 Dev Channel Launcher
# Requires ps2sdk (ee-gcc, ps2-packer)
#
#   export PS2SDK=/usr/local/ps2dev/ps2sdk
#   make

PS2SDK  ?= $(PS2DEV)/ps2sdk
PS2DEV  ?= /usr/local/ps2dev

EE_BIN  = ps2dev_launcher.elf

EE_OBJS = launcher_main.o \
          ../ps2stub/mc_access.o

EE_INCS = -I$(PS2SDK)/ee/include \
           -I$(PS2SDK)/common/include \
           -I$(PS2SDK)/ee/include/graph \
           -I$(PS2SDK)/ee/include/draw \
           -I..

EE_LIBS = -lmc -lpad -lgraph -ldraw -ldma -lpacket \
           -lkernel -lc -lm

EE_CFLAGS  = -O2 -G0 -mno-gpopt -Wall $(EE_INCS)

EE_LDFLAGS = -T$(PS2SDK)/ee/startup/linkfile \
              -L$(PS2SDK)/ee/lib \
              $(EE_LIBS)

EE_CC  = ee-gcc
EE_LD  = ee-gcc

.PHONY: all clean pack install

all: $(EE_BIN)

$(EE_BIN): $(EE_OBJS)
	$(EE_LD) $(EE_CFLAGS) -o $@ $(EE_OBJS) $(EE_LDFLAGS)

%.o: %.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

# Pack with ps2-packer for faster FMCB boot
pack: $(EE_BIN)
	ps2-packer $(EE_BIN) ps2dev_launcher_packed.elf

# Copy both ELFs to a mounted SD card (adjust path as needed)
SDCARD ?= /mnt/sdcard/PS2DEV
install: all
	mkdir -p $(SDCARD)
	cp $(EE_BIN) $(SDCARD)/
	cp ../ps2stub/ps2dev_stub.elf $(SDCARD)/

clean:
	rm -f $(EE_OBJS) $(EE_BIN) ps2dev_launcher_packed.elf
