CC	 = i686-w64-mingw32-gcc
CFLAGS	+= -I/usr/include/efi/ia32 -masm=intel
ENTRY	 = _efi_main
