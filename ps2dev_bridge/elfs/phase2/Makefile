# Phase 2 ELFs — introspection
PS2SDK  ?= $(PS2DEV)/ps2sdk
PS2DEV  ?= /usr/local/ps2dev

STUB_DIR  = ../../ps2stub
P1_DIR    = ../phase1

EE_CC      = ee-gcc
EE_LD      = ee-gcc

EE_INCS    = -I$(PS2SDK)/ee/include \
              -I$(PS2SDK)/common/include \
              -I../.. \
              -I$(STUB_DIR) \
              -I$(P1_DIR)

EE_CFLAGS  = -O2 -G0 -mno-gpopt -Wall $(EE_INCS)
EE_LIBS    = -lmc -lkernel -lc -lm
EE_LDFLAGS = -T$(PS2SDK)/ee/startup/linkfile \
              -L$(PS2SDK)/ee/lib $(EE_LIBS)

COMMON_OBJS = $(P1_DIR)/dev_io.o

ELFS = regdump.elf threadlist.elf ioppeek.elf

.PHONY: all clean install

all: $(ELFS)

regdump.elf: regdump.o $(COMMON_OBJS)
	$(EE_LD) $(EE_CFLAGS) -o $@ $^ $(EE_LDFLAGS)

threadlist.elf: threadlist.o $(COMMON_OBJS)
	$(EE_LD) $(EE_CFLAGS) -o $@ $^ $(EE_LDFLAGS)

ioppeek.elf: ioppeek.o $(COMMON_OBJS)
	$(EE_LD) $(EE_CFLAGS) -o $@ $^ $(EE_LDFLAGS)

%.o: %.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

$(P1_DIR)/dev_io.o: $(P1_DIR)/dev_io.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

SDCARD ?= /mnt/sdcard/PS2DEV
install: all
	mkdir -p $(SDCARD)/phase2
	cp $(ELFS) $(SDCARD)/phase2/

clean:
	rm -f *.o $(ELFS)
