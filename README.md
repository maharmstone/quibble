Quibble
-------

Quibble is the custom Windows bootloader - an open-source reimplementation of the
files bootmgfw.efi and winload.efi, able to boot versions of Windows from XP
to Windows 10 22H2. Unlike the official bootloader, it is extensible, allowing you
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

This has been tested successfully in Qemu v7.2, VirtualBox v7, and on EFI version F50 of a Gigabyte motherboard.
The quality of EFI implementations varies significantly, so if you're testing on real hardware it may or may not work
for you.

* Install Windows on an NTFS volume.

* On modern versions of Windows, turn off Fast Startup in the Control Panel, or run `powercfg /h off` from an admin command prompt.

* For Btrfs:

  * Install [WinBtrfs](https://github.com/maharmstone/btrfs) - you will need version 1.6 at least, but the later the better.

  * Shutdown your PC or VM, and copy its hard disk to a Btrfs partition. The best way is to use [Ntfs2btrfs](https://github.com/maharmstone/ntfs2btrfs) to do in-place conversion, which will also preserve your metadata.

* Extract the Quibble package into your EFI System Partition. It's supposed to work in a subdirectory,
but if you have trouble you might need to put it in the root.

* Adjust the file freeldr.ini, if necessary - the default is for it to boot from the third partition
of the first disk, which is where Windows normally installs itself. You can also change the SystemPath to e.g. `SystemPath=btrfs(1e10b60a-8e9d-466b-a33a-21760829cf3a)\Windows`,
referring to the partition by UUID rather than number. This is the Btrfs UUID, i.e. what shows up in
the drive properties box on WinBtrfs, or what shows in `btrfs check` on Linux.

* Add quibble.efi to your list of UEFI boot options, and hope that it works...

Changelog
---------

* 20230328
  * Added NTFS driver
  * Added support for versions of Windows 10 up to 22H2
  * CD drives no longer get allocated ARC names
  * Switched to C++
  * Lots of miscellaneous bug fixes

* 20210111
  * Added support for GOP graphics
  * Added support for TTF fonts
  * Added rudimentary recovery of unclean Registry hives
  * Added support for Btrfs compression

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
  * `git clone https://github.com/maharmstone/quibble --recurse-submodules`
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

* Why don't I get pretty graphics?

Disable CSM in your BIOS. Bear in mind that this will also stop Windows 7 and earlier
from booting.

* Which versions of Windows does this work on?

With the Btrfs driver or NTFS drivers, this should work on XP, Vista, Windows 7, Windows 8,
Windows 8.1, and Windows 10 versions 1507 to 22H2.

* Which filesystems does this support?

The included Btrfs driver, and maybe also the FAT driver that's part of the UEFI specifications.
Windows XP, Vista, and 7 will work fine from a FAT volume, anything after that won't.

* How can I extend this?

Drop your EFI driver in the drivers folder, and it'll load it on startup.

* Why do I get a BAD_SYSTEM_CONFIG_INFO BSOD?

This means that the Registry is unclean, and our rudimentary recovery wasn't good enough. If you attach
C:\Windows\System32\config\SYSTEM to another machine via Regedit temporarily, it'll fix it.
Make sure you shut Windows down properly to avoid having to do this.

* Can I boot Btrfs from an arbitrary subvolume, like I can on Linux?

Yes - add /SUBVOL=xxx to your Options in freeldr.ini. You can find the number to use on the
Properties page of your subvolume. On Linux you can use `btrfs subvol list`, but bear in mind
that you will need to translate the number to hexadecimal.

* Why can't I access any NTFS volumes in Windows when booting from Btrfs?

Because Windows only loads ntfs.sys when it's booting from NTFS. To start it as a one-off, run
`sc start ntfs` from an elevated command prompt. To get it to start every time, open regedit and
change HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\ntfs\Start to 1.

* Why can't I access FAT partitions on Windows?

If Windows doesn't load the FAT driver automatically, try running `sc start fastfat`.

* Why don't I see the Windows logo on startup?

The boot graphics code isn't completed yet - you won't see either the Windows logo or the progress
indicator, just a few seconds of blackness.

Licences and Thanks
-------------------

This is released under the LGPL. The Mersenne Twister code is by Mutsuo Saito and Makoto Matsumoto -
see the header of tinymt32.c. The GNU-EFI headers are under the BSD licence.

This code incorporates FreeType, licensed under the [FreeType Licence](https://github.com/freetype/freetype/blob/master/LICENSE.TXT).

The included font is [Jost*](https://indestructibletype.com/Jost.html), from Indestructible Type.

The Btrfs EFI driver contains portions of the following software:

### Zlib

  Copyright (C) 1995-2017 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

### LZO

btrfs-efi contains portions of an early version of lzo, which is copyright 1996
Markus Oberhumer. Modern versions are licensed under the GPL, but this was
licensed under the LGPL, so I believe it is okay to use.

### Zstd

Copyright (c) 2016-present, Facebook, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific
   prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### Harfbuzz

HarfBuzz is licensed under the so-called "Old MIT" license.  Details follow.
For parts of HarfBuzz that are licensed under different licenses see individual
files names COPYING in subdirectories where applicable.

Copyright © 2010-2022  Google, Inc.
Copyright © 2015-2020  Ebrahim Byagowi
Copyright © 2019,2020  Facebook, Inc.
Copyright © 2012,2015  Mozilla Foundation
Copyright © 2011  Codethink Limited
Copyright © 2008,2010  Nokia Corporation and/or its subsidiary(-ies)
Copyright © 2009  Keith Stribley
Copyright © 2011  Martin Hosken and SIL International
Copyright © 2007  Chris Wilson
Copyright © 2005,2006,2020,2021,2022,2023  Behdad Esfahbod
Copyright © 2004,2007,2008,2009,2010,2013,2021,2022,2023  Red Hat, Inc.
Copyright © 1998-2005  David Turner and Werner Lemberg
Copyright © 2016  Igalia S.L.
Copyright © 2022  Matthias Clasen
Copyright © 2018,2021  Khaled Hosny
Copyright © 2018,2019,2020  Adobe, Inc
Copyright © 2013-2015  Alexei Podtelezhnikov

For full copyright notices consult the individual files in the package.

Permission is hereby granted, without written agreement and without
license or royalty fees, to use, copy, modify, and distribute this
software and its documentation for any purpose, provided that the
above copyright notice and the following two paragraphs appear in
all copies of this software.

IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

To-do list
----------

* Get working with XP and Vista on amd64 (done?)
* Add proper Registry recovery
* Parse BCD files
* Get tested on more hardware
* Slipstream into Windows ISO(?)
* Add ARM and Aarch64 versions
* Verification of signatures
* Early-launch anti-malware
* ASLR
* Booting 32-bit Windows on 64-bit machine
* BIOS emulation on machines without CSM
* Add RAID support for Btrfs
* Hibernation, etc.
* Get kdnet working with Windows 8.1
