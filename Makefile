# default architecture
ARCH	= amd64

include arch/$(ARCH)/Makefile.inc

CFLAGS	+= -ffreestanding -I/usr/include/efi -g -D__MAKEWITH_GNUEFI \
	   -fno-stack-protector -fno-stack-check -mno-stack-arg-probe \
	   -Werror-implicit-function-declaration -Wall -Wextra \
	   -Wno-address-of-packed-member -Wno-packed-not-aligned \

LIBS	+= boot.o reg.o misc.o peload.o hw.o mem.o apiset.o menu.o tinymt32.o

BTRFS_LIBS	+= btrfs.o misc.o crc32c.o


all: quibble.efi btrfs.efi

quibble.efi: $(LIBS)
	$(CC) -nostdlib -Wl,-dll -shared -Wl,--subsystem,10 -e efi_main -o $@ $(LIBS)

boot.o: src/boot.c src/reg.h src/misc.h src/peload.h src/win.h src/x86.h src/quibble.h src/quibbleproto.h
	$(CC) $(CFLAGS) -c -o $@ $<

reg.o: src/reg.c src/reg.h src/misc.h src/winreg.h
	$(CC) $(CFLAGS) -c -o $@ $<

misc.o: src/misc.c src/misc.h
	$(CC) $(CFLAGS) -c -o $@ $<

peload.o: src/peload.c src/peload.h src/misc.h src/peloaddef.h src/tinymt32.h
	$(CC) $(CFLAGS) -c -o $@ $<

hw.o: src/hw.c src/win.h src/x86.h src/misc.h src/quibble.h
	$(CC) $(CFLAGS) -c -o $@ $<

mem.o: src/mem.c src/x86.h src/win.h src/quibble.h
	$(CC) $(CFLAGS) -c -o $@ $<

apiset.o: src/apiset.c src/quibble.h src/win.h src/misc.h src/peload.h src/x86.h
	$(CC) $(CFLAGS) -c -o $@ $<

menu.o: src/menu.c src/quibble.h src/win.h src/misc.h src/x86.h
	$(CC) $(CFLAGS) -c -o $@ $<

tinymt32.o: src/tinymt32.c src/tinymt32.h
	$(CC) $(CFLAGS) -c -o $@ $<

btrfs.efi: $(BTRFS_LIBS)
	$(CC) -nostdlib -Wl,-dll -shared -Wl,--subsystem,11 -e $(ENTRY) -o $@ $(BTRFS_LIBS) -lgcc

btrfs.o: src/btrfs/btrfs.c src/misc.h src/btrfs/btrfs.h src/quibbleproto.h
	$(CC) $(CFLAGS) -c -o $@ $<

crc32c.o: src/btrfs/crc32c.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm *.o quibble.efi btrfs.efi
