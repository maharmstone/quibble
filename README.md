Quibble
-------

Quibble is the custom Windows bootloader - an open-source reimplementation of the
files bootmgfw.efi and winload.efi, able to boot every version of Windows from XP
to Windows 10 2009. Unlike the official bootloader, it is extensible, allowing you
to boot from other filesystems than just NTFS.

This is only a proof of concept at this stage - don't use this for anything serious.

Screenshot of Windows 10 1909 running on Btrfs:

<img src="https://raw.githubusercontent.com/maharmstone/quibble/fw/1909.png" width="400" />

Donations
---------

I'm doing this for kicks and giggles, but if you want to donate it'd be appreciated:

* [Paypal](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=3XQVCQ6YB55L2&lc=GB&item_name=Quibble%20donation&currency_code=GBP&bn=PP%2dDonationsBF%3abtn_donate_LG%2egif%3aNonHosted)

Installation
------------

If you're booting Windows 7 or earlier in a VM, you will need the OVMF firmware with Seabios compiled
in as the Compatibility Support Module (CSM), which isn't normally included. Precompiled version are
available: [x86](https://github.com/maharmstone/quibble/blob/fw/OVMF_CODE.fd?raw=true) and [amd64](https://github.com/maharmstone/quibble/blob/fw/OVMF_CODE64.fd?raw=true).

This has been tested successfully in Qemu v5.0 and on EFI version F50 of a Gigabyte motherboard. The quality of
EFI implementations varies significantly, so if you're testing on real hardware it may or may not work
for you.

* Install Windows on an NTFS volume.

* Install [WinBtrfs](https://github.com/maharmstone/btrfs) - you will need version 1.6 at least, but the later the better.

* On modern versions of Windows, turn off Fast Startup in the Control Panel.

* Shutdown your PC or VM, and copy its hard disk to a Btrfs partition. The best way is to use [Ntfs2btrfs](https://github.com/maharmstone/ntfs2btrfs)
to do in-place conversion, which will also preserve your metadata.

* Extract the Quibble package into your EFI System Partition. It's supposed to work in a subdirectory,
but if you have trouble you might need to put it in the root.

* Adjust the file freeldr.ini, if necessary - the default is for it to boot from the first partition
of the first disk. You can also change the SystemPath to e.g. `SystemPath=btrfs(1e10b60a-8e9d-466b-a33a-21760829cf3a)\Windows`,
referring to the partition by UUID rather than number.

* Add quibble.efi to your list of UEFI boot options, and hope that it works...

Changelog
---------

* 20201108
  * Added support for Windows 10 2004 and 2009
  * KDNET now works with Realtek devices
  * Added support for booting Windows 8 and up without CSM
  * Added workarounds for issues with real EFI implementations
  * Fixed issues with multiple CPU cores

* 20200405
  * Fixed bug involving case-insensitivity
  * Changed build system to cmake
  * Included local copy of gnu-efi, to make things easier
  * Added support for compiling on MSVC
  * Added support for kdnet on Windows 10

* 20200213
  * Initial release

Compiling
---------

On Linux:

* Install a cross-compiler, x86_64-w64-mingw32-gcc, and cmake.
* Run the following:
  * `git clone https://github.com/maharmstone/quibble`
  * `cd quibble`
  * `mkdir build`
  * `cd build`
  * `cmake -DCMAKE_TOOLCHAIN_FILE=../mingw-amd64.cmake ..` or `cmake -DCMAKE_TOOLCHAIN_FILE=../mingw-x86.cmake ..`
  * `make`

On Windows:

* Install a recent version of Visual C++ - I used the free Visual Studio Community 2019
* Clone the repository, and open it as a folder
* Wait for it to finish generating its cmake cache
* Right-click on CMakeLists.txt and choose "Build"

FAQs
----

* Which versions of Windows does this work on?

With the Btrfs driver, this should work on XP, Vista, Windows 7, Windows 8, Windows 8.1,
and Windows 10 versions 1507 to 2009. XP and Vista work only in 32-bit mode for the time being.
Earlier versions _ought_ to work, as the boot structures were backwards-compatible at that
point, but the Btrfs driver won't work.

* Which filesystems does this support?

The included Btrfs driver, and maybe also the FAT driver that's part of the UEFI specifications.
Windows XP, Vista, and 7 will work fine from a FAT volume, anything after that won't.

* How can I extend this?

Drop your EFI driver in the drivers folder, and it'll load it on startup.

* Why do I get a message about Sequence1 and Sequence2?

This means that the Registry is unclean, which we can't currently recover from. If you attach
C:\Windows\System32\config\SYSTEM to another machine via Regedit temporarily, it'll fix it.
Make sure you shut Windows down properly to avoid having to do this.

* Can I boot Btrfs from an arbitrary subvolume, like I can on Linux?

Yes - add /SUBVOL=xxx to your Options in freeldr.ini. You can find the number to use on the
Properties page of your subvolume. On Linux you can use `btrfs subvol list`, but bear in mind
that you will need to translate the number to hexadecimal.

* Why can't I access any NTFS volumes in Windows?

Because Windows only loads ntfs.sys when it's booting from NTFS. To start it as a one-off, run
`sc start ntfs` from an elevated command prompt. To get it to start every time, open regedit and
change HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ntfs\Start to 1.

* Why don't I see the Windows logo on startup?

The boot graphics code isn't completed yet - you won't see either the Windows logo or the progress
indicator, just a few seconds of blackness.

* Why does it hang on startup?

There's a race condition in the latest version of WinBtrfs, which manifests itself on some hardware.
Try adding /ONECPU to your boot options, to see if that makes a difference.

Licence
-------

This is released under the LGPL. The Mersenne Twister code is by Mutsuo Saito and Makoto Matsumoto -
see the header of tinymt32.c. The GNU-EFI headers are under the BSD licence.

Thanks to Daniel Hepper for his [public-domain bitmap font](https://github.com/dhepper/font8x8).

To-do list
----------

* Get working with XP and Vista on amd64 (done?)
* Add Registry recovery
* Add NTFS driver
* Parse BCD files
* Get tested on more hardware
* Slipstream into Windows ISO(?)
* Add ARM and Aarch64 versions
* Verification of signatures
* Early-launch anti-malware
* ASLR
* Booting 32-bit Windows on 64-bit machine
* Add RAID support for Btrfs
* Add compression support for Btrfs
* Hibernation, etc.
* Get kdnet working with Windows 8.1
