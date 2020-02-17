# default architecture
ARCH	= amd64

include arch/$(ARCH)/Makefile.inc

CFLAGS	+= -ffreestanding -I/usr/include/efi -D__MAKEWITH_GNUEFI -pipe \
	   -fno-stack-protector -fno-stack-check -mno-stack-arg-probe \
	   -Werror-implicit-function-declaration -Wall -Wextra \
	   -Wno-address-of-packed-member -Wno-packed-not-aligned

EFI_CFLAGS += -nostdlib -Wl,-dll -shared -e $(ENTRY)

LIBS	+= boot.o reg.o misc.o peload.o hw.o mem.o apiset.o menu.o tinymt32.o

BTRFS_LIBS	+= btrfs.o misc.o crc32c.o

all: CFLAGS += -O2
all: quibble.efi btrfs.efi

debug: CFLAGS += -g -Og
debug: quibble.efi btrfs.efi

quibble.efi: $(LIBS)
	$(CC) $(EFI_CFLAGS) -Wl,--subsystem,10 -o $@ $(LIBS)

btrfs.efi: $(BTRFS_LIBS)
	$(CC) $(EFI_CFLAGS) -Wl,--subsystem,11 -o $@ $(BTRFS_LIBS) -lgcc

clean:
	$(RM) *.o quibble.efi btrfs.efi

%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: src/btrfs/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# specify header dependencies
boot.o: src/reg.h src/misc.h src/peload.h src/win.h src/x86.h src/quibble.h src/quibbleproto.h
reg.o: src/reg.h src/misc.h src/winreg.h
misc.o: src/misc.h
peload.o: src/peload.h src/misc.h src/peloaddef.h src/tinymt32.h
hw.o: src/win.h src/x86.h src/misc.h src/quibble.h
mem.o: src/x86.h src/win.h src/quibble.h
apiset.o: src/quibble.h src/win.h src/misc.h src/peload.h src/x86.h
menu.o: src/quibble.h src/win.h src/misc.h src/x86.h
tinymt32.o: src/tinymt32.h
btrfs.o: src/misc.h src/btrfs/btrfs.h src/quibbleproto.h
