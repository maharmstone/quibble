Quibble
-------

Quibble is the custom Windows bootloader - an open-source reimplementation of the
files bootmgfw.efi and winload.efi, able to boot every version of Windows from XP
to Windows 10 1909. Unlike the official bootloader, it is extensible, allowing you
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

This has been tested with Qemu using the OVMF firmware, with Seabios included as a Compatibility
Support Module (CSM). It may work on other VMs or hardware, it may not. You will need the CSM
version of OVMF, which isn't the usual one bundled with Qemu. Precompiled version are available:
[x86](https://github.com/maharmstone/quibble/blob/fw/OVMF_CODE.fd?raw=true) and [amd64](https://github.com/maharmstone/quibble/blob/fw/OVMF_CODE64.fd?raw=true).

* Set up your VM, and install Windows on an NTFS volume.

* Install [WinBtrfs](https://github.com/maharmstone/btrfs) - you will need version 1.6 at least.

* On modern versions of Windows, turn off Fast Startup in the Control Panel.

* Shutdown your VM, and copy its hard disk to a Btrfs partition.
    * Surprisingly, there isn't an easy way to copy an NTFS volume exactly - you can try using
    robocopy on Windows, but you'll run into files you can't easily access, even as Admin. [RunAsTI](https://github.com/jschicht/RunAsTI)
    helps, but it's still not easy. Robocopy doesn't like symlinks, either.
    * Better, you can mount the volume on Linux and use rsync. This does have the down side that
    you'll lose the DOS attributes and the security descriptors.

* On Windows 8, you will need to use icacls to give C:\Users\[USERNAME]\AppData\LocalLow a
mandatory integrity level of "Low", otherwise it will crash on login.

* On some versions of Windows 10 (1607 to 1803?), you will need to give the pseudo-group
"ALL APPLICATION PACKAGES" access to C:\Windows\Fonts, or you'll have the amusing sight of
Windows not being able to display any text.

* Extract the Quibble package into your EFI System Partition. It's supposed to work in a subdirectory,
but if you have trouble you might need to put it in the root.

* Adjust the file freeldr.ini, if necessary - the default is for it to boot from the first partition
of the first disk. You can also change the SystemPath to e.g. `SystemPath=btrfs(1e10b60a-8e9d-466b-a33a-21760829cf3a)\Windows`,
referring to the partition by UUID rather than number.

* Add quibble.efi to your list of UEFI boot options, and hope that it works...

FAQs
----

* Which versions of Windows does this work on?

With the Btrfs driver, this should work on XP, Vista, Windows 7, Windows 8, Windows 8.1,
and Windows 10 versions 1507 to 1909. XP and Vista work only in 32-bit mode for the time being.
Earlier versions _ought_ to work, as the boot structures were backwards-compatible at that
point, but the Btrfs driver won't work.

* Which filesystems does this support?

The included Btrfs driver, and maybe also the FAT driver that's part of the UEFI specifications.
Windows XP, Vista, and 7 will work fine from a FAT volume, though why you'd want to do that
is anyone's guess.

* How can I extend this?

Drop your EFI driver in the drivers folder, and it'll load it on startup.

* How do I compile this?

Install a cross-compiler version of GCC - either i686-w64-mingw32-gcc or x86_64-w64-mingw32-gcc -
and gnu-efi.

* UWP applications / the Start Menu / Calculator / Edge don't work

I know. This is at least partly due to the NTFS permissions not being transferred. You can use
Win+R to open the Run menu.

* Why do I get a message about the Recycle Bin being corrupted?

Again, permissions. Let Windows fix it and it won't happen again.

* How do I stop Notepad popping up on startup?

Mark the files C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup\desktop.ini and
C:\Users\[USERNAME]\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\desktop.ini.

* Why do I get a message about Sequence1 and Sequence2?

This means that the Registry is unclean, which we can't currently recover from. If you attach
C:\Windows\System32\config\SYSTEM to another machine via Regedit temporarily, it'll fix it.
Make sure you shut Windows down properly to avoid having to do this.

* Can I boot Btrfs from an arbitrary subvolume, like I can on Linux?

Yes - add /SUBVOL=xxx to your Options in freeldr.ini. You can find the number to use on the
Properties page of your subvolume. Bear in mind that unlike Linux this is in hexadecimal.

Licence
-------

This is released under the LGPL. The Mersenne Twister code is by Mutsuo Saito and Makoto Matsumoto -
see the header of tinymt32.c.

Known issues
------------

* Windows can hang while flushing the Registry
* Multiple cores can be a bit ropey

To-do list
----------

* Get compiling with MSVC
* Booting on Windows 8+ without CSM
* Get Windows 8.1 and 10 network debugging working
* Get working with XP and Vista on amd64
* Add NTFS driver
* Parse BCD files
* Get working better on real hardware
* Add Registry recovery
* Add ARM and Aarch64 versions
* Verification of signatures
* Early-launch anti-malware
* ASLR
* Booting 32-bit Windows on 64-bit machine
* Add RAID support for Btrfs
* Add compression support for Btrfs
* Hibernation, etc.
