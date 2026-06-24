# Phase 1 ELFs — chain verification
# Requires ps2sdk ee-gcc toolchain
#
#   export PS2SDK=/usr/local/ps2dev/ps2sdk
#   make

PS2SDK  ?= $(PS2DEV)/ps2sdk
PS2DEV  ?= /usr/local/ps2dev

STUB_DIR  = ../../ps2stub

EE_CC      = ee-gcc
EE_LD      = ee-gcc

EE_INCS    = -I$(PS2SDK)/ee/include \
              -I$(PS2SDK)/common/include \
              -I../.. \
              -I$(STUB_DIR)

EE_CFLAGS  = -O2 -G0 -mno-gpopt -Wall $(EE_INCS)

EE_LIBS    = -lmc -lkernel -lc -lm

EE_LDFLAGS = -T$(PS2SDK)/ee/startup/linkfile \
              -L$(PS2SDK)/ee/lib \
              $(EE_LIBS)

# Common objects shared by all ELFs
COMMON_OBJS = dev_io.o $(STUB_DIR)/mc_access.o

# Per-ELF targets
ELFS = hello.elf \
       ramdump.elf \
       ramdump_interactive.elf

.PHONY: all clean install

all: $(ELFS)

# ── hello ─────────────────────────────────────────────────────────────────

hello.elf: hello.o dev_io.o
	$(EE_LD) $(EE_CFLAGS) -o $@ $^ $(EE_LDFLAGS)

# ── ramdump ───────────────────────────────────────────────────────────────

DUMP_ADDR ?= 0x00100000
DUMP_LEN  ?= 512

ramdump.elf: ramdump.o dev_io.o
	$(EE_LD) $(EE_CFLAGS) -o $@ $^ $(EE_LDFLAGS)

ramdump.o: ramdump.c
	$(EE_CC) $(EE_CFLAGS) \
	    -DDUMP_ADDR=$(DUMP_ADDR) \
	    -DDUMP_LEN=$(DUMP_LEN) \
	    -c $< -o $@

# ── ramdump_interactive ───────────────────────────────────────────────────

ramdump_interactive.elf: ramdump_interactive.o dev_io.o $(STUB_DIR)/mc_access.o
	$(EE_LD) $(EE_CFLAGS) -o $@ $^ $(EE_LDFLAGS)

# ── Generic rules ─────────────────────────────────────────────────────────

%.o: %.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

$(STUB_DIR)/mc_access.o: $(STUB_DIR)/mc_access.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

# ── Helpers ───────────────────────────────────────────────────────────────

SDCARD ?= /mnt/sdcard/PS2DEV

install: all
	mkdir -p $(SDCARD)/phase1
	cp $(ELFS) $(SDCARD)/phase1/

clean:
	rm -f *.o $(STUB_DIR)/mc_access.o $(ELFS)
