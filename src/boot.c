/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of Quibble.
 *
 * Quibble is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * Quibble is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with Quibble.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <intrin.h>
#include "quibble.h"
#include "reg.h"
#include "peload.h"
#include "misc.h"
#include "win.h"
#include "x86.h"
#include "tinymt32.h"
#include "quibbleproto.h"
#include "font8x8_basic.h"

// #define DEBUG_EARLY_FAULTS

typedef struct {
    LIST_ENTRY list_entry;
    WCHAR* name;
    WCHAR* file;
    WCHAR* dir;
    WCHAR* group;
    uint32_t tag;
} driver;

typedef struct {
    union {
        uint8_t loader_block;
        LOADER_PARAMETER_BLOCK_WS03 loader_block_ws03;
        LOADER_PARAMETER_BLOCK_VISTA loader_block_vista;
        LOADER_PARAMETER_BLOCK_WIN7 loader_block_win7;
        LOADER_PARAMETER_BLOCK_WIN8 loader_block_win8;
        LOADER_PARAMETER_BLOCK_WIN81 loader_block_win81;
        LOADER_PARAMETER_BLOCK_WIN10 loader_block_win10;
    };

    union {
        uint8_t extension;
        LOADER_PARAMETER_EXTENSION_WS03 extension_ws03;
        LOADER_PARAMETER_EXTENSION_VISTA extension_vista;
        LOADER_PARAMETER_EXTENSION_VISTA_SP2 extension_vista_sp2;
        LOADER_PARAMETER_EXTENSION_WIN7 extension_win7;
        LOADER_PARAMETER_EXTENSION_WIN8 extension_win8;
        LOADER_PARAMETER_EXTENSION_WIN81 extension_win81;
        LOADER_PARAMETER_EXTENSION_WIN10 extension_win10;
        LOADER_PARAMETER_EXTENSION_WIN10_1607 extension_win10_1607;
        LOADER_PARAMETER_EXTENSION_WIN10_1703 extension_win10_1703;
        LOADER_PARAMETER_EXTENSION_WIN10_1809 extension_win10_1809;
        LOADER_PARAMETER_EXTENSION_WIN10_1903 extension_win10_1903;
        LOADER_PARAMETER_EXTENSION_WIN10_2004 extension_win10_2004;
    };

    char strings[1024];
    NLS_DATA_BLOCK nls;
    ARC_DISK_INFORMATION arc_disk_information;
    LOADER_PERFORMANCE_DATA loader_performance_data;
    DEBUG_DEVICE_DESCRIPTOR debug_device_descriptor;

    union {
        uint8_t bgc;
        BOOT_GRAPHICS_CONTEXT_V1 bgc_v1;
        BOOT_GRAPHICS_CONTEXT_V2 bgc_v2;
        BOOT_GRAPHICS_CONTEXT_V3 bgc_v3;
        BOOT_GRAPHICS_CONTEXT_V4 bgc_v4;
    };
} loader_store;

typedef struct _command_line {
    char* debug_type;
    WCHAR* hal;
    WCHAR* kernel;
    uint64_t subvol;
#ifdef _X86_
    unsigned int pae;
    unsigned int nx;
#endif
} command_line;

typedef struct {
    unsigned int x;
    unsigned int y;
} text_pos;

EFI_SYSTEM_TABLE* systable;
NLS_DATA_BLOCK nls;
size_t acp_size, oemcp_size, lang_size;
void* errata_inf = NULL;
size_t errata_inf_size = 0;
LIST_ENTRY images;
void* stack;
EFI_HANDLE image_handle;
bool kdnet_loaded = false;
static DEBUG_DEVICE_DESCRIPTOR debug_device_descriptor;
image* kdstub = NULL;
uint64_t cpu_frequency;
void* apic = NULL;
void* system_font = NULL;
size_t system_font_size = 0;
void* console_font = NULL;
size_t console_font_size = 0;
loader_store* store2;

typedef void (EFIAPI* change_stack_cb) (
    EFI_BOOT_SERVICES* bs,
    EFI_HANDLE image_handle
);

static const WCHAR system_root[] = L"\\SystemRoot\\";

// FIXME - calls to protocols should include pointer to callback to display any errors (and also TRACE etc.?)

EFI_STATUS add_image(EFI_BOOT_SERVICES* bs, LIST_ENTRY* images, const WCHAR* name, TYPE_OF_MEMORY memory_type,
                     const WCHAR* dir, bool dll, BOOT_DRIVER_LIST_ENTRY* bdle, unsigned int order,
                     bool no_reloc) {
    EFI_STATUS Status;
    image* img;

    Status = bs->AllocatePool(EfiLoaderData, sizeof(image), (void**)&img);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    // FIXME - show error if name too long?

    wcsncpy(img->name, name, sizeof(img->name) / sizeof(WCHAR));
    wcsncpy(img->dir, dir, sizeof(img->dir) / sizeof(WCHAR));
    InsertTailList(images, &img->list_entry);

    img->img = NULL;
    img->import_list = NULL;
    img->memory_type = memory_type;
    img->dll = dll;
    img->bdle = bdle;
    img->order = order;
    img->no_reloc = no_reloc;

    return EFI_SUCCESS;
}

static unsigned int julian_day(unsigned int year, unsigned int month, unsigned int day) {
    int a, b, c;

    a = month - 14;
    a /= 12;
    a += year + 4800;
    a *= 1461;
    a >>= 2;

    b = month - 14;
    b /= 12;
    b *= -12;
    b += month - 2;
    b *= 367;
    b /= 12;

    c = month - 14;
    c /= 12;
    c += year + 4900;
    c /= 100;
    c *= 3;
    c >>= 2;

    return a + b - c + day - 32075;
}

static void get_system_time(int64_t* time) {
    EFI_STATUS Status;
    EFI_TIME tm;
    unsigned int jd;
    int64_t t;

    Status = systable->RuntimeServices->GetTime(&tm, NULL);
    if (EFI_ERROR(Status)) {
        print_error(L"GetTime", Status);
        return;
    }

    jd = julian_day(tm.Year, tm.Month, tm.Day);
    jd -= 2305814; // January 1, 1601

    t = jd * 86400ull;
    t += (tm.Hour * 3600) + (tm.Minute * 60) + tm.Second;
    t *= 10000000;

    // FIXME - time zone?

    *time = t;
}

static uint64_t get_cpu_frequency(EFI_BOOT_SERVICES* bs) {
    uint64_t tsc1, tsc2;

    static const UINTN delay = 50; // 50 ms

    // FIXME - should probably do cpuid to check that rdtsc is available

    tsc1 = __rdtsc();

    bs->Stall(delay * 1000);

    tsc2 = __rdtsc();

    return (tsc2 - tsc1) * (1000 / delay);
}

static loader_store* initialize_loader_block(EFI_BOOT_SERVICES* bs, char* options, char* path, char* arc_name, unsigned int* store_pages,
                                             void** va, LIST_ENTRY* mappings, LIST_ENTRY* drivers, EFI_HANDLE image_handle,
                                             uint16_t version, uint16_t build, uint16_t revision, LOADER_BLOCK1A** pblock1a,
                                             LOADER_BLOCK1B** pblock1b, void*** registry_base, uint32_t** registry_length,
                                             LOADER_BLOCK2** pblock2, LOADER_EXTENSION_BLOCK1A** pextblock1a,
                                             LOADER_EXTENSION_BLOCK1B** pextblock1b, LOADER_EXTENSION_BLOCK3** pextblock3,
                                             uintptr_t** ploader_pages_spanned, LIST_ENTRY* core_drivers) {
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS addr;
    loader_store* store;
    unsigned int pages;
    LOADER_BLOCK1A* block1a;
    LOADER_BLOCK1B* block1b;
    LOADER_BLOCK1C* block1c;
    LOADER_BLOCK2* block2;
    LOADER_EXTENSION_BLOCK1A* extblock1a;
    LOADER_EXTENSION_BLOCK1B* extblock1b;
    LOADER_EXTENSION_BLOCK1C* extblock1c;
    LOADER_EXTENSION_BLOCK2B* extblock2b;
    LOADER_EXTENSION_BLOCK3* extblock3;
    LOADER_EXTENSION_BLOCK4* extblock4;
    LOADER_EXTENSION_BLOCK5A* extblock5a;
    uintptr_t* loader_pages_spanned;
    char* str;
    unsigned int pathlen;

    pages = sizeof(loader_store) / EFI_PAGE_SIZE;
    if (sizeof(loader_store) % EFI_PAGE_SIZE != 0)
        pages++;

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return NULL;
    }

    store = (loader_store*)(uintptr_t)addr;

    memset(store, 0, sizeof(loader_store));

    cpu_frequency = get_cpu_frequency(bs);

    if (version <= _WIN32_WINNT_WS03) {
        block1a = &store->loader_block_ws03.Block1a;
        block1b = &store->loader_block_ws03.Block1b;
        block1c = &store->loader_block_ws03.Block1c;
        block2 = &store->loader_block_ws03.Block2;
        extblock1a = &store->extension_ws03.Block1a;
        loader_pages_spanned = &store->extension_ws03.LoaderPagesSpanned;
        extblock1b = &store->extension_ws03.Block1b;
        extblock1c = &store->extension_ws03.Block1c;
        extblock2b = NULL;
        extblock3 = NULL;
        extblock4 = NULL;
        extblock5a = NULL;

        store->extension_ws03.Size = sizeof(LOADER_PARAMETER_EXTENSION_WS03);
        store->extension_ws03.Profile.Status = 2;
        store->extension_ws03.MajorVersion = version >> 8;
        store->extension_ws03.MinorVersion = version & 0xff;

        *registry_base = &store->loader_block_ws03.RegistryBase;
        *registry_length = &store->loader_block_ws03.RegistryLength;
    } else if (version == _WIN32_WINNT_VISTA) {
        block1a = &store->loader_block_vista.Block1a;
        block1b = &store->loader_block_vista.Block1b;
        block1c = &store->loader_block_vista.Block1c;
        block2 = &store->loader_block_vista.Block2;

        extblock3 = NULL;
        extblock4 = NULL;
        extblock5a = NULL;

        // FIXME - is x86 SP2 the same struct as amd64 SP2? Which does SP1 use?
        if (build >= 6002) { // service pack 2
            extblock1a = &store->extension_vista_sp2.Block1a;
            loader_pages_spanned = &store->extension_vista_sp2.LoaderPagesSpanned;
            extblock1b = &store->extension_vista_sp2.Block1b;
            extblock1c = &store->extension_vista_sp2.Block1c;
            extblock2b = &store->extension_vista_sp2.Block2b;

            store->extension_vista_sp2.Size = sizeof(LOADER_PARAMETER_EXTENSION_VISTA_SP2);
            store->extension_vista_sp2.Profile.Status = 2;
            store->extension_vista_sp2.MajorVersion = version >> 8;
            store->extension_vista_sp2.MinorVersion = version & 0xff;

            store->extension_vista_sp2.LoaderPerformanceData = &store->loader_performance_data;
        } else {
            extblock1a = &store->extension_vista.Block1a;
            loader_pages_spanned = &store->extension_vista.LoaderPagesSpanned;
            extblock1b = &store->extension_vista.Block1b;
            extblock1c = &store->extension_vista.Block1c;
            extblock2b = &store->extension_vista.Block2b;

            store->extension_vista.Size = sizeof(LOADER_PARAMETER_EXTENSION_VISTA);
            store->extension_vista.Profile.Status = 2;
            store->extension_vista.MajorVersion = version >> 8;
            store->extension_vista.MinorVersion = version & 0xff;

            store->extension_vista.LoaderPerformanceData = &store->loader_performance_data;
        }

        *registry_base = &store->loader_block_vista.RegistryBase;
        *registry_length = &store->loader_block_vista.RegistryLength;

        store->loader_block_vista.FirmwareInformation.FirmwareTypeEfi = 1;
        store->loader_block_vista.FirmwareInformation.EfiInformation.FirmwareVersion = systable->Hdr.Revision;
    } else if (version == _WIN32_WINNT_WIN7) {
        block1a = &store->loader_block_win7.Block1a;
        block1b = &store->loader_block_win7.Block1b;
        block1c = &store->loader_block_win7.Block1c;
        block2 = &store->loader_block_win7.Block2;
        extblock1a = &store->extension_win7.Block1a;
        loader_pages_spanned = &store->extension_win7.LoaderPagesSpanned;
        extblock1b = &store->extension_win7.Block1b;
        extblock1c = &store->extension_win7.Block1c;
        extblock2b = &store->extension_win7.Block2b;
        extblock3 = &store->extension_win7.Block3;
        extblock4 = NULL;
        extblock5a = NULL;

        store->extension_win7.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN7);
        store->extension_win7.Profile.Status = 2;

        store->loader_block_win7.OsMajorVersion = version >> 8;
        store->loader_block_win7.OsMinorVersion = version & 0xff;
        store->loader_block_win7.Size = sizeof(LOADER_PARAMETER_BLOCK_WIN7);

        store->extension_win7.TpmBootEntropyResult.ResultCode = TpmBootEntropyNoTpmFound;
        store->extension_win7.TpmBootEntropyResult.ResultStatus = STATUS_NOT_IMPLEMENTED;

        store->extension_win7.ProcessorCounterFrequency = cpu_frequency;

        *registry_base = &store->loader_block_win7.RegistryBase;
        *registry_length = &store->loader_block_win7.RegistryLength;

        store->loader_block_win7.FirmwareInformation.FirmwareTypeEfi = 1;
        store->loader_block_win7.FirmwareInformation.EfiInformation.FirmwareVersion = systable->Hdr.Revision;

        store->extension_win7.LoaderPerformanceData = &store->loader_performance_data;
    } else if (version == _WIN32_WINNT_WIN8) {
        block1a = &store->loader_block_win8.Block1a;
        block1b = &store->loader_block_win8.Block1b;
        block1c = &store->loader_block_win8.Block1c;
        block2 = &store->loader_block_win8.Block2;
        extblock1a = &store->extension_win8.Block1a;
        loader_pages_spanned = NULL;
        extblock1b = &store->extension_win8.Block1b;
        extblock1c = &store->extension_win8.Block1c;
        extblock2b = &store->extension_win8.Block2b;
        extblock3 = &store->extension_win8.Block3;
        extblock4 = &store->extension_win8.Block4;
        extblock5a = NULL;

        store->extension_win8.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN8);
        store->extension_win8.Profile.Status = 2;

        store->loader_block_win8.OsMajorVersion = version >> 8;
        store->loader_block_win8.OsMinorVersion = version & 0xff;
        store->loader_block_win8.Size = sizeof(LOADER_PARAMETER_BLOCK_WIN8);

        InitializeListHead(&store->loader_block_win8.EarlyLaunchListHead);
        InitializeListHead(&store->loader_block_win8.CoreDriverListHead);

        store->loader_block_win8.KernelStackSize = KERNEL_STACK_SIZE * EFI_PAGE_SIZE;

        store->loader_block_win8.CoreDriverListHead.Flink = core_drivers->Flink;
        store->loader_block_win8.CoreDriverListHead.Blink = core_drivers->Blink;
        store->loader_block_win8.CoreDriverListHead.Flink->Blink = &store->loader_block_win8.CoreDriverListHead;
        store->loader_block_win8.CoreDriverListHead.Blink->Flink = &store->loader_block_win8.CoreDriverListHead;

        store->extension_win8.BootEntropyResult.maxEntropySources = 7;

        *registry_base = &store->loader_block_win8.RegistryBase;
        *registry_length = &store->loader_block_win8.RegistryLength;

        store->loader_block_win8.FirmwareInformation.FirmwareTypeEfi = 1;
        store->loader_block_win8.FirmwareInformation.EfiInformation.FirmwareVersion = systable->Hdr.Revision;
        InitializeListHead(&store->loader_block_win8.FirmwareInformation.EfiInformation.FirmwareResourceList);

        store->extension_win8.LoaderPerformanceData = &store->loader_performance_data;
        store->extension_win8.ProcessorCounterFrequency = cpu_frequency;
    } else if (version == _WIN32_WINNT_WINBLUE) {
        block1a = &store->loader_block_win81.Block1a;
        block1b = &store->loader_block_win81.Block1b;
        block1c = &store->loader_block_win81.Block1c;
        block2 = &store->loader_block_win81.Block2;
        extblock1a = &store->extension_win81.Block1a;
        loader_pages_spanned = NULL;
        extblock1b = &store->extension_win81.Block1b;
        extblock1c = &store->extension_win81.Block1c;
        extblock2b = &store->extension_win81.Block2b;
        extblock3 = &store->extension_win81.Block3;
        extblock4 = &store->extension_win81.Block4;
        extblock5a = &store->extension_win81.Block5a;

        if (revision >= 18438)
            store->extension_win81.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN81);
        else
            store->extension_win81.Size = offsetof(LOADER_PARAMETER_EXTENSION_WIN81, padding4);

        store->extension_win81.Profile.Status = 2;

        store->loader_block_win81.OsMajorVersion = version >> 8;
        store->loader_block_win81.OsMinorVersion = version & 0xff;
        store->loader_block_win81.Size = sizeof(LOADER_PARAMETER_BLOCK_WIN81);

        InitializeListHead(&store->loader_block_win81.EarlyLaunchListHead);
        InitializeListHead(&store->loader_block_win81.CoreDriverListHead);

        store->loader_block_win81.KernelStackSize = KERNEL_STACK_SIZE * EFI_PAGE_SIZE;

        store->loader_block_win81.CoreDriverListHead.Flink = core_drivers->Flink;
        store->loader_block_win81.CoreDriverListHead.Blink = core_drivers->Blink;
        store->loader_block_win81.CoreDriverListHead.Flink->Blink = &store->loader_block_win81.CoreDriverListHead;
        store->loader_block_win81.CoreDriverListHead.Blink->Flink = &store->loader_block_win81.CoreDriverListHead;

        store->extension_win81.BootEntropyResult.maxEntropySources = 8;

        *registry_base = &store->loader_block_win81.RegistryBase;
        *registry_length = &store->loader_block_win81.RegistryLength;

        store->loader_block_win81.FirmwareInformation.FirmwareTypeEfi = 1;
        store->loader_block_win81.FirmwareInformation.EfiInformation.FirmwareVersion = systable->Hdr.Revision;
        InitializeListHead(&store->loader_block_win81.FirmwareInformation.EfiInformation.FirmwareResourceList);

        store->extension_win81.LoaderPerformanceData = &store->loader_performance_data;
        store->extension_win81.ProcessorCounterFrequency = cpu_frequency;

        if (kdnet_loaded) {
            memcpy(&store->debug_device_descriptor, &debug_device_descriptor, sizeof(debug_device_descriptor));
            store->extension_win81.KdDebugDevice = &store->debug_device_descriptor;
        }
    } else if (version == _WIN32_WINNT_WIN10) {
        LOADER_EXTENSION_BLOCK6* extblock6;

        block1a = &store->loader_block_win10.Block1a;
        block1b = &store->loader_block_win10.Block1b;
        block1c = &store->loader_block_win10.Block1c;
        block2 = &store->loader_block_win10.Block2;
        loader_pages_spanned = NULL;

        store->loader_block_win10.OsMajorVersion = version >> 8;
        store->loader_block_win10.OsMinorVersion = version & 0xff;

        if (build >= WIN10_BUILD_1803)
            store->loader_block_win10.Size = sizeof(LOADER_PARAMETER_BLOCK_WIN10);
        else
            store->loader_block_win10.Size = offsetof(LOADER_PARAMETER_BLOCK_WIN10, OsBootstatPathName);

        if (build >= WIN10_BUILD_1511)
            store->loader_block_win10.OsLoaderSecurityVersion = 1;

        InitializeListHead(&store->loader_block_win10.EarlyLaunchListHead);
        InitializeListHead(&store->loader_block_win10.CoreDriverListHead);
        InitializeListHead(&store->loader_block_win10.CoreExtensionsDriverListHead);
        InitializeListHead(&store->loader_block_win10.TpmCoreDriverListHead);

        store->loader_block_win10.KernelStackSize = KERNEL_STACK_SIZE * EFI_PAGE_SIZE;

        store->loader_block_win10.CoreDriverListHead.Flink = core_drivers->Flink;
        store->loader_block_win10.CoreDriverListHead.Blink = core_drivers->Blink;
        store->loader_block_win10.CoreDriverListHead.Flink->Blink = &store->loader_block_win10.CoreDriverListHead;
        store->loader_block_win10.CoreDriverListHead.Blink->Flink = &store->loader_block_win10.CoreDriverListHead;

        *registry_base = &store->loader_block_win10.RegistryBase;
        *registry_length = &store->loader_block_win10.RegistryLength;

        store->loader_block_win10.FirmwareInformation.FirmwareTypeEfi = 1;
        store->loader_block_win10.FirmwareInformation.EfiInformation.FirmwareVersion = systable->Hdr.Revision;
        InitializeListHead(&store->loader_block_win10.FirmwareInformation.EfiInformation.FirmwareResourceList);

        if (build >= WIN10_BUILD_2004) {
            extblock1a = &store->extension_win10_2004.Block1a;
            extblock1b = &store->extension_win10_2004.Block1b;
            extblock1c = &store->extension_win10_2004.Block1c;
            extblock2b = &store->extension_win10_2004.Block2b;
            extblock3 = &store->extension_win10_2004.Block3;
            extblock4 = &store->extension_win10_2004.Block4;
            extblock5a = &store->extension_win10_2004.Block5a;
            extblock6 = &store->extension_win10_2004.Block6;
            store->extension_win10_2004.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN10_2004);

            store->extension_win10_2004.Profile.Status = 2;
            store->extension_win10_2004.BootEntropyResult.maxEntropySources = 10;
            store->extension_win10_2004.MajorRelease = NTDDI_WIN10_20H1;
            store->extension_win10_2004.ProcessorCounterFrequency = cpu_frequency;
        } else if (build >= WIN10_BUILD_1903) {
            extblock1a = &store->extension_win10_1903.Block1a;
            extblock1b = &store->extension_win10_1903.Block1b;
            extblock1c = &store->extension_win10_1903.Block1c;
            extblock2b = &store->extension_win10_1903.Block2b;
            extblock3 = &store->extension_win10_1903.Block3;
            extblock4 = &store->extension_win10_1903.Block4;
            extblock5a = &store->extension_win10_1903.Block5a;
            extblock6 = &store->extension_win10_1903.Block6;
            store->extension_win10_1903.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN10_1903);

            store->extension_win10_1903.Profile.Status = 2;
            store->extension_win10_1903.BootEntropyResult.maxEntropySources = 10;

            // contrary to what you might expect, both 1903 and 1909 use the same value here
            store->extension_win10_1903.MajorRelease = NTDDI_WIN10_19H1;
            store->extension_win10_1903.ProcessorCounterFrequency = cpu_frequency;
        } else if (build == WIN10_BUILD_1809) {
            extblock1a = &store->extension_win10_1809.Block1a;
            extblock1b = &store->extension_win10_1809.Block1b;
            extblock1c = &store->extension_win10_1809.Block1c;
            extblock2b = &store->extension_win10_1809.Block2b;
            extblock3 = &store->extension_win10_1809.Block3;
            extblock4 = &store->extension_win10_1809.Block4;
            extblock5a = &store->extension_win10_1809.Block5a;
            extblock6 = &store->extension_win10_1809.Block6;
            store->extension_win10_1809.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN10_1809);

            store->extension_win10_1809.Profile.Status = 2;
            store->extension_win10_1809.BootEntropyResult.maxEntropySources = 10;
            store->extension_win10_1809.MajorRelease = NTDDI_WIN10_RS5;
            store->extension_win10_1809.ProcessorCounterFrequency = cpu_frequency;
        } else if (build >= WIN10_BUILD_1703) {
            extblock1a = &store->extension_win10_1703.Block1a;
            extblock1b = &store->extension_win10_1703.Block1b;
            extblock1c = &store->extension_win10_1703.Block1c;
            extblock2b = &store->extension_win10_1703.Block2b;
            extblock3 = &store->extension_win10_1703.Block3;
            extblock4 = &store->extension_win10_1703.Block4;
            extblock5a = &store->extension_win10_1703.Block5a;
            extblock6 = &store->extension_win10_1703.Block6;

            if (build >= WIN10_BUILD_1803)
                store->extension_win10_1703.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN10_1703);
            else
                store->extension_win10_1703.Size = offsetof(LOADER_PARAMETER_EXTENSION_WIN10_1703, MaxPciBusNumber);

            store->extension_win10_1703.Profile.Status = 2;

            store->extension_win10_1703.BootEntropyResult.maxEntropySources = 8;

            if (build == WIN10_BUILD_1703)
                store->extension_win10_1703.MajorRelease = NTDDI_WIN10_RS2;
            else if (build == WIN10_BUILD_1709)
                store->extension_win10_1703.MajorRelease = NTDDI_WIN10_RS3;
            else
                store->extension_win10_1703.MajorRelease = NTDDI_WIN10_RS4;

            store->extension_win10_1703.LoaderPerformanceData = &store->loader_performance_data;
            store->extension_win10_1703.ProcessorCounterFrequency = cpu_frequency;
        } else if (build >= WIN10_BUILD_1607) {
            extblock1a = &store->extension_win10_1607.Block1a;
            extblock1b = &store->extension_win10_1607.Block1b;
            extblock1c = &store->extension_win10_1607.Block1c;
            extblock2b = &store->extension_win10_1607.Block2b;
            extblock3 = &store->extension_win10_1607.Block3;
            extblock4 = &store->extension_win10_1607.Block4;
            extblock5a = &store->extension_win10_1607.Block5a;
            extblock6 = &store->extension_win10_1607.Block6;
            store->extension_win10_1607.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN10_1607);

            store->extension_win10_1607.Profile.Status = 2;

            store->extension_win10_1607.BootEntropyResult.maxEntropySources = 8;

            store->extension_win10_1607.MajorRelease = NTDDI_WIN10_RS1;

            store->extension_win10_1607.LoaderPerformanceData = &store->loader_performance_data;
            store->extension_win10_1607.ProcessorCounterFrequency = cpu_frequency;
        } else {
            extblock1a = &store->extension_win10.Block1a;
            extblock1b = &store->extension_win10.Block1b;
            extblock1c = &store->extension_win10.Block1c;
            extblock2b = &store->extension_win10.Block2b;
            extblock3 = &store->extension_win10.Block3;
            extblock4 = &store->extension_win10.Block4;
            extblock5a = &store->extension_win10.Block5a;
            extblock6 = &store->extension_win10.Block6;

            if (build < WIN10_BUILD_1511)
                store->extension_win10.Size = offsetof(LOADER_PARAMETER_EXTENSION_WIN10, SystemHiveRecoveryInfo) + sizeof(uint32_t);
            else
                store->extension_win10.Size = sizeof(LOADER_PARAMETER_EXTENSION_WIN10);

            store->extension_win10.Profile.Status = 2;

            store->extension_win10.BootEntropyResult.maxEntropySources = 8;

            store->extension_win10.LoaderPerformanceData = &store->loader_performance_data;
            store->extension_win10.ProcessorCounterFrequency = cpu_frequency;
        }

        if (kdnet_loaded) {
            memcpy(&store->debug_device_descriptor, &debug_device_descriptor, sizeof(debug_device_descriptor));
            extblock6->KdDebugDevice = &store->debug_device_descriptor;
        }
    } else {
        print(L"Unsupported Windows version.\r\n");
        return NULL;
    }

    InitializeListHead(&block1a->LoadOrderListHead);
    InitializeListHead(&block1a->MemoryDescriptorListHead);

    block1a->BootDriverListHead.Flink = drivers->Flink;
    block1a->BootDriverListHead.Blink = drivers->Blink;
    block1a->BootDriverListHead.Flink->Blink = &block1a->BootDriverListHead;
    block1a->BootDriverListHead.Blink->Flink = &block1a->BootDriverListHead;

    *va = (uint8_t*)*va + (STACK_SIZE * EFI_PAGE_SIZE);

    InitializeListHead(&extblock1c->FirmwareDescriptorListHead);
    extblock1c->AcpiTable = (void*)1; // FIXME - this is what freeldr does - it doesn't seem right...

    block2->Extension = &store->extension;
    block1c->NlsData = &store->nls;

    block1c->NlsData->AnsiCodePageData = nls.AnsiCodePageData;
    block1c->NlsData->OemCodePageData = nls.OemCodePageData;
    block1c->NlsData->UnicodeCodePageData = nls.UnicodeCodePageData;

    block1c->ArcDiskInformation = &store->arc_disk_information;
    InitializeListHead(&block1c->ArcDiskInformation->DiskSignatureListHead);

    str = store->strings;
    strcpy(str, arc_name);
    block1c->ArcBootDeviceName = str;

    str = &str[strlen(str) + 1];
    strcpy(str, arc_name);
    block1c->ArcHalDeviceName = str;

    str = &str[strlen(str) + 1];
    block1c->NtBootPathName = str;

    pathlen = strlen(path);

    *str = '\\'; str++;
    strcpy(str, path);

    if (path[pathlen] != '\\') { // add trailing backslash if not present
        str[pathlen] = '\\';
        str[pathlen + 1] = 0;
    }

    str = &str[strlen(str) + 1];
    strcpy(str, "\\");
    block1c->NtHalPathName = str;

    str = &str[strlen(str) + 1];

    if (options)
        strcpy(str, options);
    else
        *str = 0;

    block1c->LoadOptions = str;

    Status = find_hardware(bs, block1c, va, mappings, image_handle, version);
    if (EFI_ERROR(Status)) {
        print_error(L"find_hardware", Status);
        bs->FreePages(addr, pages);
        return NULL;
    }

    Status = find_disks(bs, &block1c->ArcDiskInformation->DiskSignatureListHead, va, mappings, block1c->ConfigurationRoot,
                        version >= _WIN32_WINNT_WIN7 || (version == _WIN32_WINNT_VISTA && build >= 6002));
    if (EFI_ERROR(Status)) {
        print_error(L"find_disks", Status);
        bs->FreePages(addr, pages);
        return NULL;
    }

    if (extblock2b) {
        InitializeListHead(&extblock2b->BootApplicationPersistentData);
    }

    if (extblock3) {
        InitializeListHead(&extblock3->AttachedHives);
    }

    if (extblock4) {
        InitializeListHead(&extblock4->HalExtensionModuleList);

        get_system_time(&extblock4->SystemTime);
        extblock4->DbgRtcBootTime = 1;
    }

    if (extblock5a) {
        extblock5a->ApiSetSchema = apisetva;
        extblock5a->ApiSetSchemaSize = apisetsize;
        InitializeListHead(&extblock5a->ApiSetSchemaExtensions);
    }

    *store_pages = pages;
    *pblock1a = block1a;
    *pblock1b = block1b;
    *pblock2 = block2;
    *pextblock1a = extblock1a;
    *pextblock1b = extblock1b;
    *pextblock3 = extblock3;
    *ploader_pages_spanned = loader_pages_spanned;

    return store;
}

static void fix_list_mapping(LIST_ENTRY* list, LIST_ENTRY* mappings) {
    LIST_ENTRY* le = list->Flink;

    while (le != list) {
        LIST_ENTRY* le2 = le->Flink;

        le->Flink = find_virtual_address(le->Flink, mappings);
        le->Blink = find_virtual_address(le->Blink, mappings);

        le = le2;
    }

    list->Flink = find_virtual_address(list->Flink, mappings);
    list->Blink = find_virtual_address(list->Blink, mappings);
}

static void fix_config_mapping(CONFIGURATION_COMPONENT_DATA* ccd, LIST_ENTRY* mappings, void* parent_va, void** va) {
    void* new_va = find_virtual_address(ccd, mappings);

    if (ccd->ComponentEntry.Identifier)
        ccd->ComponentEntry.Identifier = fix_address_mapping(ccd->ComponentEntry.Identifier, ccd, new_va);

    if (ccd->ConfigurationData)
        ccd->ConfigurationData = fix_address_mapping(ccd->ConfigurationData, ccd, new_va);

    if (ccd->Child) {
        void* child_va;

        fix_config_mapping(ccd->Child, mappings, new_va, &child_va);
        ccd->Child = child_va;
    }

    if (ccd->Sibling) {
        void* sibling_va;

        fix_config_mapping(ccd->Sibling, mappings, parent_va, &sibling_va);
        ccd->Sibling = sibling_va;
    }

    ccd->Parent = parent_va;

    *va = new_va;
}

static void fix_image_list_mapping(LOADER_BLOCK1A* block1, LIST_ENTRY* mappings) {
    LIST_ENTRY* le;

    le = block1->LoadOrderListHead.Flink;
    while (le != &block1->LoadOrderListHead) {
        KLDR_DATA_TABLE_ENTRY* dte = _CR(le, KLDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

        dte->BaseDllName.Buffer = find_virtual_address(dte->BaseDllName.Buffer, mappings);
        dte->FullDllName.Buffer = find_virtual_address(dte->FullDllName.Buffer, mappings);

        le = le->Flink;
    }

    fix_list_mapping(&block1->LoadOrderListHead, mappings);
}

static void fix_driver_list_mapping(LIST_ENTRY* list, LIST_ENTRY* mappings) {
    LIST_ENTRY* le;

    le = list->Flink;
    while (le != list) {
        BOOT_DRIVER_LIST_ENTRY* bdle = _CR(le, BOOT_DRIVER_LIST_ENTRY, Link);

        bdle->FilePath.Buffer = find_virtual_address(bdle->FilePath.Buffer, mappings);
        bdle->RegistryPath.Buffer = find_virtual_address(bdle->RegistryPath.Buffer, mappings);
        bdle->LdrEntry = find_virtual_address(bdle->LdrEntry, mappings);

        le = le->Flink;
    }

    fix_list_mapping(list, mappings);
}

static void fix_arc_disk_mapping(LOADER_BLOCK1C* block1, LIST_ENTRY* mappings, bool new_disk_format) {
    LIST_ENTRY* le;

    le = block1->ArcDiskInformation->DiskSignatureListHead.Flink;
    while (le != &block1->ArcDiskInformation->DiskSignatureListHead) {
        if (new_disk_format) {
            ARC_DISK_SIGNATURE_WIN7* arc = _CR(le, ARC_DISK_SIGNATURE_WIN7, ListEntry);

            arc->ArcName = find_virtual_address(arc->ArcName, mappings);
        } else {
            ARC_DISK_SIGNATURE* arc = _CR(le, ARC_DISK_SIGNATURE, ListEntry);

            arc->ArcName = find_virtual_address(arc->ArcName, mappings);
        }

        le = le->Flink;
    }

    fix_list_mapping(&block1->ArcDiskInformation->DiskSignatureListHead, mappings);
}

static void fix_store_mapping(loader_store* store, void* va, LIST_ENTRY* mappings, uint16_t version, uint16_t build) {
    void* ccd_va;
    LOADER_BLOCK1A* block1a;
    LOADER_BLOCK1C* block1c;
    LOADER_BLOCK2* block2;
    LOADER_EXTENSION_BLOCK1C* extblock1c;
    LOADER_EXTENSION_BLOCK2B* extblock2b;
    LOADER_EXTENSION_BLOCK3* extblock3;
    LOADER_EXTENSION_BLOCK4* extblock4;
    LOADER_EXTENSION_BLOCK5A* extblock5a;

    if (version <= _WIN32_WINNT_WS03) {
        block1a = &store->loader_block_ws03.Block1a;
        block1c = &store->loader_block_ws03.Block1c;
        block2 = &store->loader_block_ws03.Block2;
        extblock1c = &store->extension_ws03.Block1c;
        extblock2b = NULL;
        extblock3 = NULL;
        extblock4 = NULL;
        extblock5a = NULL;
    } else if (version == _WIN32_WINNT_VISTA) {
        block1a = &store->loader_block_vista.Block1a;
        block1c = &store->loader_block_vista.Block1c;
        block2 = &store->loader_block_vista.Block2;
        extblock1c = &store->extension_vista.Block1c;
        extblock2b = &store->extension_vista.Block2b;
        extblock3 = NULL;
        extblock4 = NULL;
        extblock5a = NULL;

        store->loader_block_vista.FirmwareInformation.EfiInformation.VirtualEfiRuntimeServices =
            find_virtual_address(&systable->RuntimeServices->GetTime, mappings);

        store->extension_vista.LoaderPerformanceData =
            find_virtual_address(store->extension_vista.LoaderPerformanceData, mappings);
    } else if (version == _WIN32_WINNT_WIN7) {
        block1a = &store->loader_block_win7.Block1a;
        block1c = &store->loader_block_win7.Block1c;
        block2 = &store->loader_block_win7.Block2;
        extblock1c = &store->extension_win7.Block1c;
        extblock2b = &store->extension_win7.Block2b;
        extblock3 = &store->extension_win7.Block3;
        extblock4 = NULL;
        extblock5a = NULL;

        store->loader_block_win7.FirmwareInformation.EfiInformation.VirtualEfiRuntimeServices =
            find_virtual_address(&systable->RuntimeServices->GetTime, mappings);

        store->extension_win7.LoaderPerformanceData =
            find_virtual_address(store->extension_win7.LoaderPerformanceData, mappings);
    } else if (version == _WIN32_WINNT_WIN8) {
        block1a = &store->loader_block_win8.Block1a;
        block1c = &store->loader_block_win8.Block1c;
        block2 = &store->loader_block_win8.Block2;
        extblock1c = &store->extension_win8.Block1c;
        extblock2b = &store->extension_win8.Block2b;
        extblock3 = &store->extension_win8.Block3;
        extblock4 = &store->extension_win8.Block4;
        extblock5a = NULL;

        fix_list_mapping(&store->loader_block_win8.EarlyLaunchListHead, mappings);

        fix_driver_list_mapping(&store->loader_block_win8.CoreDriverListHead, mappings);

        store->loader_block_win8.FirmwareInformation.EfiInformation.VirtualEfiRuntimeServices =
            find_virtual_address(&systable->RuntimeServices->GetTime, mappings);

        fix_list_mapping(&store->loader_block_win8.FirmwareInformation.EfiInformation.FirmwareResourceList, mappings);

        store->extension_win8.LoaderPerformanceData =
            find_virtual_address(store->extension_win8.LoaderPerformanceData, mappings);
    } else if (version == _WIN32_WINNT_WINBLUE) {
        block1a = &store->loader_block_win81.Block1a;
        block1c = &store->loader_block_win81.Block1c;
        block2 = &store->loader_block_win81.Block2;
        extblock1c = &store->extension_win81.Block1c;
        extblock2b = &store->extension_win81.Block2b;
        extblock3 = &store->extension_win81.Block3;
        extblock4 = &store->extension_win81.Block4;
        extblock5a = &store->extension_win81.Block5a;

        fix_list_mapping(&store->loader_block_win81.EarlyLaunchListHead, mappings);

        fix_driver_list_mapping(&store->loader_block_win81.CoreDriverListHead, mappings);

        store->loader_block_win81.FirmwareInformation.EfiInformation.VirtualEfiRuntimeServices =
            find_virtual_address(&systable->RuntimeServices->GetTime, mappings);
        store->loader_block_win81.FirmwareInformation.EfiInformation.EfiMemoryMap =
            find_virtual_address(store->loader_block_win81.FirmwareInformation.EfiInformation.EfiMemoryMap, mappings);

        fix_list_mapping(&store->loader_block_win81.FirmwareInformation.EfiInformation.FirmwareResourceList, mappings);

        store->extension_win81.LoaderPerformanceData =
            find_virtual_address(store->extension_win81.LoaderPerformanceData, mappings);

        if (store->extension_win81.KdDebugDevice)
            store->extension_win81.KdDebugDevice = find_virtual_address(store->extension_win81.KdDebugDevice, mappings);
    } else if (version == _WIN32_WINNT_WIN10) {
        LOADER_EXTENSION_BLOCK6* extblock6;

        block1a = &store->loader_block_win10.Block1a;
        block1c = &store->loader_block_win10.Block1c;
        block2 = &store->loader_block_win10.Block2;

        fix_list_mapping(&store->loader_block_win10.EarlyLaunchListHead, mappings);
        fix_list_mapping(&store->loader_block_win10.CoreExtensionsDriverListHead, mappings);
        fix_list_mapping(&store->loader_block_win10.TpmCoreDriverListHead, mappings);

        fix_driver_list_mapping(&store->loader_block_win10.CoreDriverListHead, mappings);

        store->loader_block_win10.FirmwareInformation.EfiInformation.VirtualEfiRuntimeServices =
            find_virtual_address(&systable->RuntimeServices->GetTime, mappings);
        store->loader_block_win10.FirmwareInformation.EfiInformation.EfiMemoryMap =
            find_virtual_address(store->loader_block_win10.FirmwareInformation.EfiInformation.EfiMemoryMap, mappings);

        fix_list_mapping(&store->loader_block_win10.FirmwareInformation.EfiInformation.FirmwareResourceList, mappings);

        if (build >= WIN10_BUILD_2004) {
            extblock1c = &store->extension_win10_2004.Block1c;
            extblock2b = &store->extension_win10_2004.Block2b;
            extblock3 = &store->extension_win10_2004.Block3;
            extblock4 = &store->extension_win10_2004.Block4;
            extblock5a = &store->extension_win10_2004.Block5a;
            extblock6 = &store->extension_win10_2004.Block6;
        } else if (build >= WIN10_BUILD_1903) {
            extblock1c = &store->extension_win10_1903.Block1c;
            extblock2b = &store->extension_win10_1903.Block2b;
            extblock3 = &store->extension_win10_1903.Block3;
            extblock4 = &store->extension_win10_1903.Block4;
            extblock5a = &store->extension_win10_1903.Block5a;
            extblock6 = &store->extension_win10_1903.Block6;
        } else if (build == WIN10_BUILD_1809) {
            extblock1c = &store->extension_win10_1809.Block1c;
            extblock2b = &store->extension_win10_1809.Block2b;
            extblock3 = &store->extension_win10_1809.Block3;
            extblock4 = &store->extension_win10_1809.Block4;
            extblock5a = &store->extension_win10_1809.Block5a;
            extblock6 = &store->extension_win10_1809.Block6;
        } else if (build >= WIN10_BUILD_1703) {
            extblock1c = &store->extension_win10_1703.Block1c;
            extblock2b = &store->extension_win10_1703.Block2b;
            extblock3 = &store->extension_win10_1703.Block3;
            extblock4 = &store->extension_win10_1703.Block4;
            extblock5a = &store->extension_win10_1703.Block5a;
            extblock6 = &store->extension_win10_1703.Block6;

            store->extension_win10_1703.LoaderPerformanceData =
                find_virtual_address(store->extension_win10_1703.LoaderPerformanceData, mappings);
        } else if (build >= WIN10_BUILD_1607) {
            extblock1c = &store->extension_win10_1607.Block1c;
            extblock2b = &store->extension_win10_1607.Block2b;
            extblock3 = &store->extension_win10_1607.Block3;
            extblock4 = &store->extension_win10_1607.Block4;
            extblock5a = &store->extension_win10_1607.Block5a;
            extblock6 = &store->extension_win10_1607.Block6;

            store->extension_win10_1607.LoaderPerformanceData =
                find_virtual_address(store->extension_win10_1607.LoaderPerformanceData, mappings);
        } else {
            extblock1c = &store->extension_win10.Block1c;
            extblock2b = &store->extension_win10.Block2b;
            extblock3 = &store->extension_win10.Block3;
            extblock4 = &store->extension_win10.Block4;
            extblock5a = &store->extension_win10.Block5a;
            extblock6 = &store->extension_win10.Block6;

            store->extension_win10.LoaderPerformanceData =
                find_virtual_address(store->extension_win10.LoaderPerformanceData, mappings);
        }

        if (extblock6->KdDebugDevice)
            extblock6->KdDebugDevice = find_virtual_address(extblock6->KdDebugDevice, mappings);
    } else {
        print(L"Unsupported Windows version.\r\n");
        return;
    }

    fix_image_list_mapping(block1a, mappings);

    fix_driver_list_mapping(&block1a->BootDriverListHead, mappings);

    fix_config_mapping(block1c->ConfigurationRoot, mappings, NULL, &ccd_va);
    block1c->ConfigurationRoot = ccd_va;

    block2->Extension = fix_address_mapping(block2->Extension, store, va);
    block1c->NlsData = fix_address_mapping(block1c->NlsData, store, va);

    fix_arc_disk_mapping(block1c, mappings, version >= _WIN32_WINNT_WIN7 || (version == _WIN32_WINNT_VISTA && build >= 6002));
    block1c->ArcDiskInformation = fix_address_mapping(block1c->ArcDiskInformation, store, va);

    if (block1c->ArcBootDeviceName)
        block1c->ArcBootDeviceName = find_virtual_address(block1c->ArcBootDeviceName, mappings);

    if (block1c->ArcHalDeviceName)
        block1c->ArcHalDeviceName = find_virtual_address(block1c->ArcHalDeviceName, mappings);

    if (block1c->NtBootPathName)
        block1c->NtBootPathName = find_virtual_address(block1c->NtBootPathName, mappings);

    if (block1c->NtHalPathName)
        block1c->NtHalPathName = find_virtual_address(block1c->NtHalPathName, mappings);

    if (block1c->LoadOptions)
        block1c->LoadOptions = find_virtual_address(block1c->LoadOptions, mappings);

    fix_list_mapping(&extblock1c->FirmwareDescriptorListHead, mappings);

    if (extblock2b)
        fix_list_mapping(&extblock2b->BootApplicationPersistentData, mappings);

    if (extblock3) {
        if (extblock3->BgContext)
            extblock3->BgContext = find_virtual_address(extblock3->BgContext, mappings);

        fix_list_mapping(&extblock3->AttachedHives, mappings);
    }

    if (extblock4)
        fix_list_mapping(&extblock4->HalExtensionModuleList, mappings);

    if (extblock5a)
        fix_list_mapping(&extblock5a->ApiSetSchemaExtensions, mappings);

    for (unsigned int i = 0; i < MAXIMUM_DEBUG_BARS; i++) {
        if (store->debug_device_descriptor.BaseAddress[i].Valid && store->debug_device_descriptor.BaseAddress[i].Type == CmResourceTypeMemory) {
            store->debug_device_descriptor.BaseAddress[i].TranslatedAddress =
                find_virtual_address(store->debug_device_descriptor.BaseAddress[i].TranslatedAddress, mappings);
        }
    }

    if (store->debug_device_descriptor.Memory.VirtualAddress)
        store->debug_device_descriptor.Memory.VirtualAddress = find_virtual_address(store->debug_device_descriptor.Memory.VirtualAddress, mappings);
}

static void set_gdt_entry(gdt_entry* gdt, uint16_t selector, uint32_t base, uint32_t limit, uint8_t type,
                          uint8_t ring, bool granularity, uint8_t seg_mode, bool long_mode) {
    gdt_entry* entry = (gdt_entry*)((uint8_t*)gdt + selector);

    entry->BaseLow = (uint16_t)(base & 0xffff);
    entry->BaseMid = (uint8_t)((base >> 16) & 0xff);
    entry->BaseHi  = (uint8_t)((base >> 24) & 0xff);

    if (limit < 0x100000)
        entry->Granularity = 0;
    else {
        limit >>= 12;
        entry->Granularity = 1;
    }

    entry->LimitLow = (uint16_t)(limit & 0xffff);
    entry->LimitHi = (limit >> 16) & 0x0f;

    entry->Type = type & 0x1f;
    entry->Dpl = ring & 0x3;
    entry->Pres = type != 0;
    entry->Sys = 0;
    entry->Long = long_mode ? 1 : 0;
    entry->Default_Big = !!(seg_mode & 2);

    if (granularity)
        entry->Granularity = true;
}

static void* initialize_gdt(EFI_BOOT_SERVICES* bs, KTSS* tss, KTSS* nmitss, KTSS* dftss, KTSS* mctss,
                            uint16_t version, void* pcrva) {
    EFI_STATUS Status;
    gdt_entry* gdt;
    EFI_PHYSICAL_ADDRESS addr;

#ifdef __x86_64__
    UNUSED(version);
    UNUSED(pcrva);
    UNUSED(nmitss);
    UNUSED(dftss);
    UNUSED(mctss);
#endif

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, GDT_PAGES, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return NULL;
    }

    gdt = (gdt_entry*)(uintptr_t)addr;

    memset(gdt, 0, GDT_PAGES * EFI_PAGE_SIZE);

#ifdef _X86_
    set_gdt_entry(gdt, KGDT_NULL, 0x0000, 0, 0, 0, false, 0, false);
    set_gdt_entry(gdt, KGDT_R0_CODE, 0x0000, 0xffffffff, TYPE_CODE, 0, false, 2, false);
    set_gdt_entry(gdt, KGDT_R0_DATA, 0x0000, 0xffffffff, TYPE_DATA, 0, false, 2, false);
    set_gdt_entry(gdt, KGDT_R3_CODE, 0x0000, 0xffffffff, TYPE_CODE, 3, false, 2, false);
    set_gdt_entry(gdt, KGDT_R3_DATA, 0x0000, 0xffffffff, TYPE_DATA, 3, false, 2, false);
    set_gdt_entry(gdt, KGDT_TSS, (uintptr_t)tss, 0x78-1, TYPE_TSS32A, 0, false, 0, false);

    // Vista requires the granularity bit to be cleared, otherwise it messes up when
    // trying to initialize secondary processors.
    if (version < _WIN32_WINNT_VISTA)
        set_gdt_entry(gdt, KGDT_R0_PCR, (uintptr_t)pcrva, 0x1, TYPE_DATA, 0, true, 2, false);
    else
        set_gdt_entry(gdt, KGDT_R0_PCR, (uintptr_t)pcrva, 0xfff, TYPE_DATA, 0, false, 2, false);

    set_gdt_entry(gdt, KGDT_R3_TEB, 0x0000, 0xfff, TYPE_DATA | DESCRIPTOR_ACCESSED, 3, false, 2, false);
    set_gdt_entry(gdt, KGDT_VDM_TILE, 0x0400, 0xffff, TYPE_DATA, 3, false, 0, false);
    set_gdt_entry(gdt, KGDT_LDT, 0x0000, 0, 0, 0, false, 0, false);

    if (dftss)
        set_gdt_entry(gdt, KGDT_DF_TSS, (uintptr_t)dftss, 0x67, TYPE_TSS32A, 0, false, 0, false);
    else
        set_gdt_entry(gdt, KGDT_DF_TSS, 0x20000, 0xffff, TYPE_TSS32A, 0, false, 0, false);

    if (nmitss)
        set_gdt_entry(gdt, KGDT_NMI_TSS, (uintptr_t)nmitss, 0x67, TYPE_CODE, 0, false, 0, false);
    else
        set_gdt_entry(gdt, KGDT_NMI_TSS, 0x20000, 0xffff, TYPE_CODE, 0, false, 0, false);

    set_gdt_entry(gdt, 0x60, 0x20000, 0xffff, TYPE_DATA, 0, false, 0, false);
    set_gdt_entry(gdt, 0x68, 0xb8000, 0x3fff, TYPE_DATA, 0, false, 0, false);
    set_gdt_entry(gdt, 0x70, 0xffff7000, (NUM_GDT * sizeof(gdt_entry)) - 1, TYPE_DATA, 0, false, 0, false);

    if (mctss)
        set_gdt_entry(gdt, KGDT_MC_TSS, (uintptr_t)mctss, 0x67, TYPE_CODE, 0, false, 0, false);
#elif defined(__x86_64__)
    set_gdt_entry(gdt, KGDT_NULL, 0, 0, 0, 0, false, 0, false);
    set_gdt_entry(gdt, KGDT_R0_CODE, 0, 0, TYPE_CODE, 0, false, 0, true);
    set_gdt_entry(gdt, KGDT_R0_DATA, 0, 0, TYPE_DATA, 0, false, 0, true);
    set_gdt_entry(gdt, KGDT_R3_CMCODE, 0, 0xffffffff, TYPE_CODE, 3, true, 2, false);
    set_gdt_entry(gdt, KGDT_R3_DATA, 0, 0xffffffff, TYPE_DATA, 3, false, 2, false);
    set_gdt_entry(gdt, KGDT_R3_CODE, 0, 0, TYPE_CODE, 3, false, 0, true);

    set_gdt_entry(gdt, KGDT_TSS, (uintptr_t)tss, sizeof(KTSS), TYPE_TSS32A, 0, false, 0, false);
    *(uint64_t*)((uint8_t*)gdt + KGDT_TSS + 8) = ((uintptr_t)tss) >> 32;

    set_gdt_entry(gdt, KGDT_R3_CMTEB, 0, 0xfff, TYPE_DATA, 3, false, 2, false);
    set_gdt_entry(gdt, KGDT_R0_LDT, 0, 0xffffffff, TYPE_CODE, 0, true, 2, false);
#endif

    return gdt;
}

#ifdef DEBUG_EARLY_FAULTS
static void draw_text(const char* s, text_pos* p) {
    unsigned int len = strlen(s);

    for (unsigned int i = 0; i < len; i++) {
        char* v = font8x8_basic[(unsigned int)s[i]];

        if (s[i] == '\n') {
            p->y++;
            p->x = 0;
            continue;
        }

        uint32_t* base = (uint32_t*)store2->bgc.internal.framebuffer + (store2->bgc.internal.pixels_per_scan_line * p->y * 8) + (p->x * 8);

        for (unsigned int y = 0; y < 8; y++) {
            uint8_t v2 = v[y];
            uint32_t* buf = base + (store2->bgc.internal.pixels_per_scan_line * y);

            for (unsigned int x = 0; x < 8; x++) {
                if (v2 & 1)
                    *buf = 0xffffffff;
                else
                    *buf = 0;

                v2 >>= 1;
                buf++;
            }
        }

        p->x++;
    }
}

static void draw_text_hex(uint64_t v, text_pos* p) {
    char s[17], *t;

    if (v == 0) {
        draw_text("0", p);
        return;
    }

    s[16] = 0;
    t = &s[16];

    while (v != 0) {
        t = &t[-1];

        if ((v & 0xf) >= 10)
            *t = (v & 0xf) - 10 + 'a';
        else
            *t = (v & 0xf) + '0';

        v >>= 4;
    }

    draw_text(t, p);
}

static void page_fault(uintptr_t error_code, uintptr_t rip, uintptr_t cs, uintptr_t* stack) {
    if (store2->bgc.internal.framebuffer) {
        text_pos p;

        p.x = p.y = 0;
        draw_text("Page fault!\n", &p);

        draw_text("cr2: ", &p);
        draw_text_hex(__readcr2(), &p);
        draw_text("\n", &p);

        draw_text("error code: ", &p);
        draw_text_hex(error_code, &p);
        draw_text("\n", &p);

        draw_text("rip: ", &p);
        draw_text_hex(rip, &p);
        draw_text("\n", &p);

        draw_text("cs: ", &p);
        draw_text_hex(cs, &p);
        draw_text("\n", &p);

        draw_text("stack:\n", &p);
        for (unsigned int i = 0; i < 16; i++) {
            draw_text_hex(stack[i+3], &p);
            draw_text("\n", &p);
        }
    }

    halt();
}

__attribute__((naked))
static void page_fault_wrapper() {
    __asm__ __volatile__ (
        "pop rcx\n\t"
        "mov rdx, [rsp]\n\t"
        "mov r8, [rsp+8]\n\t"
        "mov r9, rsp\n\t"
        "call %0\n\t"
        "iret\n\t"
        :
        : "a" (page_fault)
    );
}
#endif

static void* initialize_idt(EFI_BOOT_SERVICES* bs) {
    EFI_STATUS Status;
    idt_entry* idt;
    GDTIDT old;
    EFI_PHYSICAL_ADDRESS addr;

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, IDT_PAGES, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return NULL;
    }

    idt = (idt_entry*)(uintptr_t)addr;

    memset(idt, 0, IDT_PAGES * EFI_PAGE_SIZE);

#ifdef _MSC_VER
    __sidt(&old);
#else
    __asm__ __volatile__ (
        "sidt %0\n\t"
        :
        : "m" (old)
    );
#endif

    memcpy(idt, (void*)(uintptr_t)old.Base, old.Limit + 1);

#ifdef DEBUG_EARLY_FAULTS
    uintptr_t func = (uintptr_t)(void*)page_fault_wrapper;

    // page fault
    idt[0xe].offset_1 = func & 0xffff;
    idt[0xe].selector = KGDT_R0_CODE;
    idt[0xe].ist = 0;
    idt[0xe].type_attr = 0x8f;
    idt[0xe].offset_2 = (func >> 16) & 0xffff;
    idt[0xe].offset_3 = func >> 32;
    idt[0xe].zero = 0;
#endif

    return idt;
}

#ifdef _MSC_VER
void __stdcall set_gdt2(GDTIDT* desc, uint16_t selector);
#endif

static void set_gdt(gdt_entry* gdt) {
    GDTIDT desc;

    desc.Base = (uintptr_t)gdt;
    desc.Limit = (NUM_GDT * sizeof(gdt_entry)) - 1;

#ifndef _MSC_VER
    // set GDT
    __asm__ __volatile__ (
        "lgdt %0\n\t"
        :
        : "m" (desc)
    );

    // set task register
    __asm__ __volatile__ (
        "mov ax, %0\n\t"
        "ltr ax\n\t"
        :
        : "" ((uint32_t)KGDT_TSS)
        : "ax"
    );

#ifdef _X86_
    // change cs to 0x8
    __asm__ __volatile__ (
        "ljmp %0, label\n\t"
        "label:\n\t"
        :
        : "i" (0x8)
    );
#elif defined(__x86_64__)
    // change cs to 0x10
    __asm__ __volatile__ (
        "mov rax, %0\n\t"
        "push rax\n\t"
        "lea rax, label\n\t"
        "push rax\n\t"
        "lretq\n\t"
        "label:\n\t"
        :
        : "i" (0x10)
        : "rax"
    );

    // change ss to 0x18
    __asm__ __volatile__ (
        "mov ax, %0\n\t"
        "mov ss, ax\n\t"
        :
        : "i" (0x18)
        : "ax"
    );
#endif
#else
    set_gdt2(&desc, KGDT_TSS);
#endif
}

static void set_idt(idt_entry* idt) {
    GDTIDT desc;

    desc.Base = (uintptr_t)idt;
    desc.Limit = (NUM_IDT * sizeof(idt_entry)) - 1;

    // set GDT
#ifdef _MSC_VER
    __lidt(&desc);
#else
    __asm__ __volatile__ (
        "lidt %0\n\t"
        :
        : "m" (desc)
    );
#endif
}

static KTSS* allocate_tss(EFI_BOOT_SERVICES* bs) {
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS addr;
    KTSS* tss;

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(sizeof(KTSS)), &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return NULL;
    }

    tss = (KTSS*)(uintptr_t)addr;

    memset(tss, 0, PAGE_COUNT(sizeof(KTSS)) * EFI_PAGE_SIZE);

    return tss;
}

static void* allocate_page(EFI_BOOT_SERVICES* bs) {
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS addr;

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return NULL;
    }

    return (void*)(uintptr_t)addr;
}

static void find_apic() {
    int cpu_info[4];

    __cpuid(cpu_info, 1);

    if (!(cpu_info[3] & 0x200)) {
        print(L"CPU does not have an onboard APIC.\r\n");
        return;
    }

    apic = (void*)(uintptr_t)__readmsr(0x1b);

    apic = (void*)((uintptr_t)apic & 0xfffff000);
}

static EFI_STATUS open_file_case_insensitive(EFI_FILE_HANDLE dir, WCHAR** pname, EFI_FILE_HANDLE* h) {
    EFI_STATUS Status;
    unsigned int len, bs;
    UINTN size;
    WCHAR* name = *pname;
    WCHAR tmp[MAX_PATH];

    len = wcslen(name);
    bs = len;

    for (unsigned int i = 0; i < len; i++) {
        if (name[i] == '\\') {
            bs = i;
            break;
        }
    }

    memcpy(tmp, name, bs * sizeof(WCHAR));
    tmp[bs] = 0;

    Status = dir->Open(dir, h, tmp, EFI_FILE_MODE_READ, 0);
    if (Status != EFI_NOT_FOUND) {
        if (name[bs] == 0)
            *pname = &name[bs];
        else
            *pname = &name[bs + 1];

        return Status;
    }

    Status = dir->SetPosition(dir, 0);
    if (EFI_ERROR(Status)) {
        print_error(L"dir->SetPosition", Status);
        return Status;
    }

    do {
        WCHAR* fn;
        WCHAR buf[1024];

        size = sizeof(buf);

        Status = dir->Read(dir, &size, buf);
        if (EFI_ERROR(Status)) {
            print_error(L"dir->Read", Status);
            return Status;
        }

        if (size == 0)
            break;

        fn = ((EFI_FILE_INFO*)buf)->FileName;

        if (!wcsicmp(tmp, fn)) {
            if (name[bs] == 0)
                *pname = &name[bs];
            else
                *pname = &name[bs + 1];

            return dir->Open(dir, h, fn, EFI_FILE_MODE_READ, 0);
        }
    } while (true);

    return EFI_NOT_FOUND;
}

EFI_STATUS open_file(EFI_FILE_HANDLE dir, EFI_FILE_HANDLE* h, const WCHAR* name) {
    EFI_FILE_HANDLE orig_dir = dir;
    EFI_STATUS Status;

    Status = dir->Open(dir, h, (WCHAR*)name, EFI_FILE_MODE_READ, 0);
    if (Status != EFI_NOT_FOUND)
        return Status;

    while (name[0] != 0) {
        Status = open_file_case_insensitive(dir, (WCHAR**)&name, h);
        if (EFI_ERROR(Status)) {
            if (dir != orig_dir)
                dir->Close(dir);

            return Status;
        }

        if (dir != orig_dir)
            dir->Close(dir);

        if (name[0] == 0)
            return EFI_SUCCESS;

        dir = *h;
    }

    return EFI_INVALID_PARAMETER;
}

EFI_STATUS read_file(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE dir, const WCHAR* name, void** data, size_t* size) {
    EFI_STATUS Status;
    EFI_FILE_HANDLE file;
    size_t file_size, pages;
    EFI_PHYSICAL_ADDRESS addr;

    Status = open_file(dir, &file, name);
    if (EFI_ERROR(Status))
        return Status;

    {
        EFI_FILE_INFO file_info;
        EFI_GUID guid = EFI_FILE_INFO_ID;
        UINTN size = sizeof(EFI_FILE_INFO);

        Status = file->GetInfo(file, &guid, &size, &file_info);

        if (Status == EFI_BUFFER_TOO_SMALL) {
            EFI_FILE_INFO* file_info2;

            Status = bs->AllocatePool(EfiLoaderData, size, (void**)&file_info2);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                file->Close(file);
                return Status;
            }

            Status = file->GetInfo(file, &guid, &size, file_info2);
            if (EFI_ERROR(Status)) {
                print_error(L"file->GetInfo", Status);
                bs->FreePool(file_info2);
                file->Close(file);
                return Status;
            }

            file_size = file_info2->FileSize;

            bs->FreePool(file_info2);
        } else if (EFI_ERROR(Status)) {
            print_error(L"file->GetInfo", Status);
            file->Close(file);
            return Status;
        } else
            file_size = file_info.FileSize;
    }

    pages = file_size / EFI_PAGE_SIZE;
    if (file_size % EFI_PAGE_SIZE != 0)
        pages++;

    if (pages == 0) {
        file->Close(file);
        return EFI_INVALID_PARAMETER;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        file->Close(file);
        return Status;
    }

    *data = (uint8_t*)(uintptr_t)addr;
    *size = file_size;

    {
        UINTN read_size = pages * EFI_PAGE_SIZE;

        Status = file->Read(file, &read_size, *data);
        if (EFI_ERROR(Status)) {
            print_error(L"file->Read", Status);
            bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)*data, pages);
            file->Close(file);
            return Status;
        }
    }

    file->Close(file);

    return EFI_SUCCESS;
}

static EFI_STATUS load_nls(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE system32, EFI_REGISTRY_HIVE* hive, HKEY ccs, uint16_t build) {
    EFI_STATUS Status;
    HKEY key;
    WCHAR s[255], acp[MAX_PATH], oemcp[MAX_PATH], lang[MAX_PATH];
    uint32_t length, type;

    Status = hive->FindKey(hive, ccs, L"Control\\Nls\\CodePage", &key);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->FindKey", Status);
        return Status;
    }

    // query CCS\Control\Nls\CodePage\ACP

    length = sizeof(s);

    Status = hive->QueryValue(hive, key, L"ACP", s, &length, &type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValue", Status);
        return Status;
    }

    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        print(L"Type of Control\\Nls\\CodePage\\ACP value was ");
        print_hex(type);
        print(L", expected REG_SZ.\r\n");
        return EFI_INVALID_PARAMETER;
    }

    length = sizeof(acp);

    Status = hive->QueryValue(hive, key, s, acp, &length, &type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValue", Status);
        return Status;
    }

    // query CCS\Control\Nls\CodePage\OEMCP

    length = sizeof(s);

    Status = hive->QueryValue(hive, key, L"OEMCP", s, &length, &type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValue", Status);
        return Status;
    }

    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        print(L"Type of Control\\Nls\\CodePage\\OEMCP value was ");
        print_hex(type);
        print(L", expected REG_SZ.\r\n");
        return EFI_INVALID_PARAMETER;
    }

    length = sizeof(oemcp);

    Status = hive->QueryValue(hive, key, s, oemcp, &length, &type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValue", Status);
        return Status;
    }

    if (build >= WIN10_BUILD_1803)
        wcsncpy(lang, L"l_intl.nls", sizeof(lang) / sizeof(WCHAR));
    else {
        // query CCS\Control\Nls\Language\Default

        Status = hive->FindKey(hive, ccs, L"Control\\Nls\\Language", &key);
        if (EFI_ERROR(Status)) {
            print_error(L"hive->FindKey", Status);
            return Status;
        }

        length = sizeof(s);

        Status = hive->QueryValue(hive, key, L"Default", s, &length, &type);
        if (EFI_ERROR(Status)) {
            print_error(L"hive->QueryValue", Status);
            return Status;
        }

        if (type != REG_SZ && type != REG_EXPAND_SZ) {
            print(L"Type of Control\\Nls\\Language\\Default value was ");
            print_hex(type);
            print(L", expected REG_SZ.\r\n");
            return EFI_INVALID_PARAMETER;
        }

        length = sizeof(lang);

        Status = hive->QueryValue(hive, key, s, lang, &length, &type);
        if (EFI_ERROR(Status)) {
            print_error(L"hive->QueryValue", Status);
            return Status;
        }
    }

    // open files and read into memory

    memset(&nls, 0, sizeof(nls));

    print(L"Loading NLS file ");
    print(acp);
    print(L".\r\n");

    Status = read_file(bs, system32, acp, &nls.AnsiCodePageData, &acp_size);
    if (EFI_ERROR(Status)) {
        print_error(L"read_file", Status);
        return Status;
    }

    print(L"Loading NLS file ");
    print(oemcp);
    print(L".\r\n");

    Status = read_file(bs, system32, oemcp, &nls.OemCodePageData, &oemcp_size);
    if (EFI_ERROR(Status)) {
        print_error(L"read_file", Status);
        return Status;
    }

    print(L"Loading NLS file ");
    print(lang);
    print(L".\r\n");

    Status = read_file(bs, system32, lang, &nls.UnicodeCodePageData, &lang_size);
    if (EFI_ERROR(Status)) {
        print_error(L"read_file", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS load_drivers(EFI_BOOT_SERVICES* bs, EFI_REGISTRY_HIVE* hive, HKEY ccs, LIST_ENTRY* images, LIST_ENTRY* boot_drivers,
                               LIST_ENTRY* mappings, void** va, LIST_ENTRY* core_drivers, int32_t hwconfig, WCHAR* fs_driver) {
    EFI_STATUS Status;
    HKEY services, sgokey;
    WCHAR name[255], group[255], *sgo;
    unsigned int i;
    LIST_ENTRY drivers;
    LIST_ENTRY* le;
    uint32_t length, reg_type;
    size_t boot_list_size;

    static const WCHAR reg_prefix[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
    static const WCHAR system_root[] = L"\\SystemRoot\\";

    InitializeListHead(&drivers);

    Status = hive->FindKey(hive, ccs, L"Services", &services);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->FindKey", Status);
        return Status;
    }

    i = 0;

    do {
        HKEY key;
        uint32_t type, start, tag;
        WCHAR image_path[MAX_PATH], dir[MAX_PATH], *image_name;
        size_t pos;
        driver* d;
        bool is_fs_driver;

        Status = hive->EnumKeys(hive, services, i, name, sizeof(name) / sizeof(WCHAR));

        if (Status == EFI_NOT_FOUND)
            break;
        else if (EFI_ERROR(Status))
            print_error(L"hive->EnumKeys", Status);

        Status = hive->FindKey(hive, services, name, &key);
        if (EFI_ERROR(Status)) {
            print_error(L"hive->FindKey", Status);
            return Status;
        }

        Status = hive->QueryValue(hive, key, L"Type", &type, &length, &reg_type);

        if (EFI_ERROR(Status) || reg_type != REG_DWORD || (type != SERVICE_KERNEL_DRIVER && type != SERVICE_FILE_SYSTEM_DRIVER)) {
            i++;
            continue;
        }

        is_fs_driver = fs_driver && !wcsicmp(name, fs_driver);

        length = sizeof(start);

        Status = hive->QueryValue(hive, key, L"Start", &start, &length, &reg_type);
        if (EFI_ERROR(Status) || reg_type != REG_DWORD || (start != SERVICE_BOOT_START && !is_fs_driver)) {
            i++;
            continue;
        }

        if (hwconfig != -1 && !is_fs_driver) {
            HKEY sokey;

            Status = hive->FindKey(hive, key, L"StartOverride", &sokey);
            if (!EFI_ERROR(Status)) {
                WCHAR soname[12];
                uint32_t soval;

                itow(hwconfig, soname);

                length = sizeof(soval);

                Status = hive->QueryValue(hive, sokey, soname, &soval, &length, &reg_type);
                if (!EFI_ERROR(Status) && reg_type == REG_DWORD) {
                    start = soval;

                    if (start != SERVICE_BOOT_START) {
                        i++;
                        continue;
                    }
                }
            }
        }

        length = sizeof(image_path);

        Status = hive->QueryValue(hive, key, L"ImagePath", image_path, &length, &reg_type);

        if (EFI_ERROR(Status) || (reg_type != REG_SZ && reg_type != REG_EXPAND_SZ)) {
            wcsncpy(image_path, L"system32\\drivers\\", sizeof(image_path) / sizeof(WCHAR));
            wcsncat(image_path, name, sizeof(image_path) / sizeof(WCHAR));
            wcsncat(image_path, L".sys", sizeof(image_path) / sizeof(WCHAR));
        } else
            image_path[length / sizeof(WCHAR)] = 0;

        // remove \SystemRoot\ prefix if present
        if (wcslen(image_path) > (sizeof(system_root) / sizeof(WCHAR)) - 1 && !memcmp(image_path, system_root, (sizeof(system_root) / sizeof(WCHAR)) - 1))
            memcpy(image_path, &image_path[(sizeof(system_root) / sizeof(WCHAR)) - 1], (wcslen(image_path) * sizeof(WCHAR)) - sizeof(system_root) + (2*sizeof(WCHAR)));

        pos = wcslen(image_path) - 1;
        while (true) {
            if (image_path[pos] == '\\') {
                image_path[pos] = 0;
                wcsncpy(dir, image_path, sizeof(dir) / sizeof(WCHAR));

                image_name = &image_path[pos + 1];
                break;
            }

            if (pos == 0)
                break;

            pos--;
        }

        // FIXME - check dir and image_name definitely have values

        Status = bs->AllocatePool(EfiLoaderData, sizeof(driver), (void**)&d);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            goto end;
        }

        Status = bs->AllocatePool(EfiLoaderData, (wcslen(name) + 1) * sizeof(WCHAR), (void**)&d->name);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            bs->FreePool(d);
            goto end;
        }

        memcpy(d->name, name, (wcslen(name) + 1) * sizeof(WCHAR));

        Status = bs->AllocatePool(EfiLoaderData, (wcslen(image_name) + 1) * sizeof(WCHAR), (void**)&d->file);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            bs->FreePool(d->name);
            bs->FreePool(d);
            goto end;
        }

        memcpy(d->file, image_name, (wcslen(image_name) + 1) * sizeof(WCHAR));

        Status = bs->AllocatePool(EfiLoaderData, (wcslen(dir) + 1) * sizeof(WCHAR), (void**)&d->dir);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            bs->FreePool(d->file);
            bs->FreePool(d->name);
            bs->FreePool(d);
            goto end;
        }

        memcpy(d->dir, dir, (wcslen(dir) + 1) * sizeof(WCHAR));

        d->group = NULL;

        length = sizeof(group);

        Status = hive->QueryValue(hive, key, L"Group", group, &length, &reg_type);

        if (!EFI_ERROR(Status) && reg_type == REG_SZ) {
            group[length / sizeof(WCHAR)] = 0;

            Status = bs->AllocatePool(EfiLoaderData, (wcslen(group) + 1) * sizeof(WCHAR), (void**)&d->group);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                bs->FreePool(d->dir);
                bs->FreePool(d->file);
                bs->FreePool(d->name);
                bs->FreePool(d);
                goto end;
            }

            memcpy(d->group, group, (wcslen(group) + 1) * sizeof(WCHAR));
        }

        length = sizeof(tag);

        Status = hive->QueryValue(hive, key, L"Tag", &tag, &length, &reg_type);

        if (!EFI_ERROR(Status) && reg_type == REG_DWORD)
            d->tag = tag;
        else
            d->tag = 0xffffffff;

        InsertTailList(&drivers, &d->list_entry);

        i++;
    } while (true);

    // order by group

    Status = hive->FindKey(hive, ccs, L"Control\\ServiceGroupOrder", &sgokey);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->FindKey", Status);
        goto end;
    }

    length = sizeof(sgo);

    Status = hive->QueryValueNoCopy(hive, sgokey, L"List", (void**)&sgo, &length, &reg_type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValue", Status);
        goto end;
    }

    if (reg_type != REG_MULTI_SZ) {
        print(L"Control\\ServiceGroupOrder\\List was ");
        print_hex(reg_type);
        print(L", expected REG_MULTI_SZ.\r\n");
        Status = EFI_INVALID_PARAMETER;
        goto end;
    }

    {
        LIST_ENTRY drivers2;
        WCHAR* s = sgo;
        HKEY golkey = 0;

        Status = hive->FindKey(hive, ccs, L"Control\\GroupOrderList", &golkey);
        if (EFI_ERROR(Status))
            print_error(L"hive->FindKey", Status);

        InitializeListHead(&drivers2);

        while (s[0] != 0) {
            LIST_ENTRY list;
            uint32_t* gol;

            InitializeListHead(&list);

            le = drivers.Flink;
            while (le != &drivers) {
                LIST_ENTRY* le2 = le->Flink;
                driver* d = _CR(le, driver, list_entry);

                if (!wcsicmp(s, d->group)) {
                    RemoveEntryList(&d->list_entry);
                    InsertTailList(&list, &d->list_entry);
                }

                le = le2;
            }

            if (IsListEmpty(&list)) { // nothing found
                s = &s[wcslen(s) + 1];
                continue;
            }

            if (golkey != 0) {
                Status = hive->QueryValueNoCopy(hive, golkey, s, (void**)&gol, &length, &reg_type);
                if (!EFI_ERROR(Status) && length > sizeof(uint32_t) && reg_type == REG_BINARY) {
                    uint32_t arrlen = gol[0];

                    if (length < (arrlen + 1) * sizeof(uint32_t))
                        arrlen = (length / sizeof(uint32_t)) - 1;

                    gol = &gol[1];

                    for (uint32_t j = 0; j < arrlen; j++) {
                        le = list.Flink;
                        while (le != &list) {
                            LIST_ENTRY* le2 = le->Flink;
                            driver* d = _CR(le, driver, list_entry);

                            if (d->tag == gol[j]) {
                                RemoveEntryList(&d->list_entry);
                                InsertTailList(&drivers2, &d->list_entry);
                            }

                            le = le2;
                        }
                    }
                }
            }

            // add anything left over
            le = list.Flink;
            while (le != &list) {
                LIST_ENTRY* le2 = le->Flink;
                driver* d = _CR(le, driver, list_entry);

                RemoveEntryList(&d->list_entry);
                InsertTailList(&drivers2, &d->list_entry);

                le = le2;
            }

            s = &s[wcslen(s) + 1];
        }

        // add anything left over, not in any of the specified groups

        while (!IsListEmpty(&drivers)) {
            driver* d = _CR(drivers.Flink, driver, list_entry);

            RemoveEntryList(&d->list_entry);
            InsertTailList(&drivers2, &d->list_entry);
        }

        // move drivers2 back to drivers

        drivers.Flink = drivers2.Flink;
        drivers.Blink = drivers2.Blink;
        drivers.Flink->Blink = &drivers;
        drivers.Blink->Flink = &drivers;
    }

    // FIXME - make sure dependencies come first

    boot_list_size = 0;

    le = drivers.Flink;

    while (le != &drivers) {
        driver* d = _CR(le, driver, list_entry);

        boot_list_size += sizeof(BOOT_DRIVER_LIST_ENTRY);
        boot_list_size += (wcslen(d->dir) + 1 + wcslen(d->file)) * sizeof(WCHAR);
        boot_list_size += sizeof(reg_prefix) - sizeof(WCHAR) + (wcslen(d->name) * sizeof(WCHAR)) + sizeof(WCHAR);

        le = le->Flink;
    }

    {
        EFI_PHYSICAL_ADDRESS addr;
        void* pa;
        void* va2 = *va;
        unsigned int imgnum = 1;

        Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(boot_list_size), &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            goto end;
        }

        pa = (void*)(uintptr_t)addr;

        le = drivers.Flink;

        while (le != &drivers) {
            driver* d = _CR(le, driver, list_entry);
            BOOT_DRIVER_LIST_ENTRY* bdle = (BOOT_DRIVER_LIST_ENTRY*)pa;

            memset(bdle, 0, sizeof(BOOT_DRIVER_LIST_ENTRY));

            pa = (uint8_t*)pa + sizeof(BOOT_DRIVER_LIST_ENTRY);

            bdle->FilePath.Length = bdle->FilePath.MaximumLength = (wcslen(d->dir) + 1 + wcslen(d->file)) * sizeof(WCHAR);
            bdle->FilePath.Buffer = pa;

            memcpy(pa, d->dir, wcslen(d->dir) * sizeof(WCHAR));
            pa = (uint8_t*)pa + (wcslen(d->dir) * sizeof(WCHAR));

            *(WCHAR*)pa = '\\';
            pa = (uint8_t*)pa + sizeof(WCHAR);

            memcpy(pa, d->file, wcslen(d->file) * sizeof(WCHAR));
            pa = (uint8_t*)pa + (wcslen(d->file) * sizeof(WCHAR));

            bdle->RegistryPath.Length = bdle->RegistryPath.MaximumLength = sizeof(reg_prefix) - sizeof(WCHAR) + (wcslen(d->name) * sizeof(WCHAR));
            bdle->RegistryPath.Buffer = pa;

            memcpy(pa, reg_prefix, sizeof(reg_prefix) - sizeof(WCHAR));
            pa = (uint8_t*)pa + sizeof(reg_prefix) - sizeof(WCHAR);

            memcpy(pa, d->name, bdle->RegistryPath.Length - sizeof(reg_prefix) + sizeof(WCHAR));
            pa = (uint8_t*)pa + bdle->RegistryPath.Length - sizeof(reg_prefix) + sizeof(WCHAR);

            *(WCHAR*)pa = 0;
            pa = (uint8_t*)pa + sizeof(WCHAR);

            bdle->LdrEntry = NULL;

            if (core_drivers && !wcsicmp(d->group, L"Core")) {
                InsertTailList(core_drivers, &bdle->Link);
            } else {
                InsertTailList(boot_drivers, &bdle->Link);
            }

            Status = add_image(bs, images, d->file, LoaderSystemCode, d->dir, false, bdle, imgnum, false);
            if (EFI_ERROR(Status)) {
                print(L"Error while loading ");
                print(d->file);
                print(L".\r\n");
                print_error(L"add_image", Status);
                goto end;
            }

            imgnum++;

            le = le->Flink;
        }

        Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)addr, PAGE_COUNT(boot_list_size), LoaderSystemBlock);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        va2 = (uint8_t*)va2 + (PAGE_COUNT(boot_list_size) * EFI_PAGE_SIZE);
        *va = va2;
    }

    Status = EFI_SUCCESS;

end:
    while (!IsListEmpty(&drivers)) {
        driver* d = _CR(drivers.Flink, driver, list_entry);

        RemoveEntryList(&d->list_entry);

        bs->FreePool(d->name);
        bs->FreePool(d->file);
        bs->FreePool(d->dir);

        if (d->group)
            bs->FreePool(d->group);

        bs->FreePool(d);
    }

    return Status;
}

static EFI_STATUS load_errata_inf(EFI_BOOT_SERVICES* bs, EFI_REGISTRY_HIVE* hive, HKEY ccs, EFI_FILE_HANDLE windir,
                                  uint16_t version) {
    EFI_STATUS Status;
    HKEY key;
    WCHAR name[MAX_PATH];
    uint32_t length, type;

    static WCHAR infdir[] = L"inf\\";

    if (version >= _WIN32_WINNT_VISTA)
        Status = hive->FindKey(hive, ccs, L"Control\\Errata", &key);
    else
        Status = hive->FindKey(hive, ccs, L"Control\\BiosInfo", &key);

    if (EFI_ERROR(Status)) {
        print_error(L"hive->FindKey", Status);
        return Status;
    }

    memcpy(name, infdir, sizeof(infdir));

    length = sizeof(name) - sizeof(infdir) + sizeof(WCHAR);

    Status = hive->QueryValue(hive, key, L"InfName", &name[(sizeof(infdir) / sizeof(WCHAR)) - 1], &length, &type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValueNoCopy", Status);
        return Status;
    }

    print(L"Loading ");
    print(name);
    print(L".\r\n");

    Status = read_file(bs, windir, name, &errata_inf, &errata_inf_size);

    if (Status == EFI_NOT_FOUND) {
        print(name);
        print(L" not found\r\n");
        return EFI_SUCCESS;
    } else if (EFI_ERROR(Status)) {
        print(L"Error when reading ");
        print(name);
        print(L".\r\n");
        print_error(L"read_file", Status);
        return Status;
    }

    return Status;
}

static EFI_STATUS load_registry(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE system32, EFI_REGISTRY_PROTOCOL* reg,
                                void** data, uint32_t* size, LIST_ENTRY* images, LIST_ENTRY* drivers, LIST_ENTRY* mappings,
                                void** va, uint16_t version, uint16_t build, EFI_FILE_HANDLE windir, LIST_ENTRY* core_drivers,
                                WCHAR* fs_driver) {
    EFI_STATUS Status;
    EFI_FILE_HANDLE file = NULL;
    EFI_REGISTRY_HIVE* hive;
    uint32_t set, length, type;
    HKEY rootkey, key, ccs;
    WCHAR ccs_name[14];
    int32_t hwconfig = -1;

    Status = open_file(system32, &file, L"config\\SYSTEM");
    if (EFI_ERROR(Status))
        return Status;

    Status = reg->OpenHive(file, &hive);
    if (EFI_ERROR(Status)) {
        print_error(L"OpenHive", Status);
        file->Close(file);
        return Status;
    }

    Status = file->Close(file);
    if (EFI_ERROR(Status))
        print_error(L"file close", Status);

    // find where CurrentControlSet should point to

    // FIXME - LastKnownGood?

    Status = hive->FindRoot(hive, &rootkey);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->FindRoot", Status);
        goto end;
    }

    Status = hive->FindKey(hive, rootkey, L"Select", &key);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->FindKey", Status);
        goto end;
    }

    length = sizeof(set);

    Status = hive->QueryValue(hive, key, L"Default", &set, &length, &type);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->QueryValue", Status);
        goto end;
    }

    if (type != REG_DWORD) {
        print(L"Select\\Default value type was ");
        print_hex(type);
        print(L", expected DWORD.\r\n");

        Status = EFI_INVALID_PARAMETER;
        goto end;
    }

    wcsncpy(ccs_name, L"ControlSet00x", sizeof(ccs_name) / sizeof(WCHAR));
    ccs_name[12] = (set % 10) + '0';

    Status = hive->FindKey(hive, rootkey, ccs_name, &ccs);
    if (EFI_ERROR(Status)) {
        print(L"Could not find ");
        print(ccs_name);
        print(L"\r\n.");

        print_error(L"hive->FindKey", Status);
        goto end;
    }

    if (version >= _WIN32_WINNT_WIN8) {
        Status = hive->FindKey(hive, rootkey, L"HardwareConfig", &key);
        if (EFI_ERROR(Status)) {
            print_error(L"hive->FindKey", Status);
            goto end;
        }

        length = sizeof(hwconfig);

        // FIXME - should be identifying based on BIOS version etc.?
        // FIXME - looks like we should be creating a random GUID if no BIOS match

        Status = hive->QueryValue(hive, key, L"LastId", &hwconfig, &length, &type);
        if (EFI_ERROR(Status)) {
            print_error(L"hive->QueryValue", Status);
            goto end;
        }

        if (type != REG_DWORD) {
            print(L"HardwareConfig\\LastId value type was ");
            print_hex(type);
            print(L", expected DWORD.\r\n");

            Status = EFI_INVALID_PARAMETER;
            goto end;
        }

        // FIXME - also grab GUID, and put into loader block?
    }

    Status = load_drivers(bs, hive, ccs, images, drivers, mappings, va, version >= _WIN32_WINNT_WIN8 ? core_drivers : NULL,
                          hwconfig, fs_driver);
    if (EFI_ERROR(Status)) {
        print_error(L"load_drivers", Status);
        goto end;
    }

    Status = load_nls(bs, system32, hive, ccs, build);
    if (EFI_ERROR(Status)) {
        print_error(L"load_nls", Status);
        goto end;
    }

    Status = load_errata_inf(bs, hive, ccs, windir, version);
    if (EFI_ERROR(Status))
        print_error(L"load_errata_inf", Status);

    Status = hive->StealData(hive, data, size);
    if (EFI_ERROR(Status)) {
        print_error(L"hive->StealData", Status);
        goto end;
    }

end:
    {
        EFI_STATUS Status2 = hive->Close(hive);

        if (EFI_ERROR(Status2))
            print_error(L"hive close", Status2);
    }

    return Status;
}

static EFI_STATUS map_nls(EFI_BOOT_SERVICES* bs, NLS_DATA_BLOCK* nls, void** va, LIST_ENTRY* mappings) {
    EFI_STATUS Status;
    void* va2 = *va;

    Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)nls->AnsiCodePageData,
                         PAGE_COUNT(acp_size), LoaderNlsData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    nls->AnsiCodePageData = va2;
    va2 = (uint8_t*)va2 + (PAGE_COUNT(acp_size) * EFI_PAGE_SIZE);

    Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)nls->OemCodePageData,
                         PAGE_COUNT(oemcp_size), LoaderNlsData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    nls->OemCodePageData = va2;
    va2 = (uint8_t*)va2 + (PAGE_COUNT(oemcp_size) * EFI_PAGE_SIZE);

    Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)nls->UnicodeCodePageData,
                         PAGE_COUNT(lang_size), LoaderNlsData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    nls->UnicodeCodePageData = va2;
    va2 = (uint8_t*)va2 + (PAGE_COUNT(lang_size) * EFI_PAGE_SIZE);

    *va = va2;

    return EFI_SUCCESS;
}

static EFI_STATUS map_errata_inf(EFI_BOOT_SERVICES* bs, LOADER_EXTENSION_BLOCK1A* extblock1a, void** va,
                                 LIST_ENTRY* mappings) {
    EFI_STATUS Status;
    void* va2 = *va;

    Status = add_mapping(bs, mappings, va2, errata_inf, PAGE_COUNT(errata_inf_size), LoaderRegistryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    extblock1a->EmInfFileImage = va2;
    extblock1a->EmInfFileSize = errata_inf_size;

    va2 = (uint8_t*)va2 + (PAGE_COUNT(errata_inf_size) * EFI_PAGE_SIZE);

    *va = va2;

    return EFI_SUCCESS;
}

static void add_loader_entry(image* img, LOADER_BLOCK1A* block1, void** pa, bool dll,
                             BOOT_DRIVER_LIST_ENTRY* bdle, bool no_reloc) {
    KLDR_DATA_TABLE_ENTRY* dte;
    void* pa2 = *pa;
    size_t dir_len;

    dte = (KLDR_DATA_TABLE_ENTRY*)pa2;
    pa2 = (uint8_t*)pa2 + sizeof(KLDR_DATA_TABLE_ENTRY);

    memset(dte, 0, sizeof(KLDR_DATA_TABLE_ENTRY));

    dte->DllBase = img->va;
    dte->SizeOfImage = img->img->GetSize(img->img);
    img->img->GetEntryPoint(img->img, &dte->EntryPoint);
    dte->CheckSum = img->img->GetCheckSum(img->img);

    dte->BaseDllName.Length = dte->BaseDllName.MaximumLength = wcslen(img->name) * sizeof(WCHAR);
    dte->BaseDllName.Buffer = (WCHAR*)pa2;
    pa2 = (uint8_t*)pa2 + dte->BaseDllName.MaximumLength + sizeof(WCHAR);

    memcpy(dte->BaseDllName.Buffer, img->name, dte->BaseDllName.Length + sizeof(WCHAR));

    dir_len = wcslen(img->dir);

    dte->FullDllName.Length = dte->FullDllName.MaximumLength =
        sizeof(system_root) - sizeof(WCHAR) + (dir_len * sizeof(WCHAR)) + sizeof(WCHAR) + dte->BaseDllName.Length;
    dte->FullDllName.Buffer = (WCHAR*)pa2;
    pa2 = (uint8_t*)pa2 + dte->FullDllName.MaximumLength + sizeof(WCHAR);

    wcsncpy(dte->FullDllName.Buffer, system_root, dte->FullDllName.Length / sizeof(WCHAR));
    wcsncat(dte->FullDllName.Buffer, img->dir, dte->FullDllName.Length / sizeof(WCHAR));
    wcsncat(dte->FullDllName.Buffer, L"\\", dte->FullDllName.Length / sizeof(WCHAR));
    wcsncat(dte->FullDllName.Buffer, img->name, dte->FullDllName.Length / sizeof(WCHAR));

    dte->EntryProcessed = 1;
    dte->LoadCount = 1;

    if (dll)
        dte->Flags |= LDRP_DRIVER_DEPENDENT_DLL;

    if (img->img->GetDllCharacteristics(img->img) & IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY)
        dte->Flags |= LDRP_IMAGE_INTEGRITY_FORCED;

    if (no_reloc)
        dte->DontRelocate = 1;

    InsertTailList(&block1->LoadOrderListHead, &dte->InLoadOrderLinks);

    if (bdle)
        bdle->LdrEntry = dte;

    *pa = pa2;
}

static EFI_STATUS generate_images_list(EFI_BOOT_SERVICES* bs, LIST_ENTRY* images, LOADER_BLOCK1A* block1,
                                       void** va, LIST_ENTRY* mappings) {
    EFI_STATUS Status;
    LIST_ENTRY* le;
    size_t size = 0;
    EFI_PHYSICAL_ADDRESS addr;
    void* pa;

    le = images->Flink;
    while (le != images) {
        image* img = _CR(le, image, list_entry);
        size_t name_len;

        size += sizeof(KLDR_DATA_TABLE_ENTRY);

        name_len = wcslen(img->name);
        size += (name_len + 1) * sizeof(WCHAR);
        size += sizeof(system_root) - sizeof(WCHAR) + ((wcslen(img->dir) + name_len + 1 + 1) * sizeof(WCHAR));

        le = le->Flink;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(size), &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    pa = (void*)(uintptr_t)addr;

    le = images->Flink;
    while (le != images) {
        image* img = _CR(le, image, list_entry);

        add_loader_entry(img, block1, &pa, img->dll, img->bdle, img->no_reloc);

        le = le->Flink;
    }

    Status = add_mapping(bs, mappings, *va, (void*)(uintptr_t)addr, PAGE_COUNT(size), LoaderSystemBlock);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    *va = (uint8_t*)*va + (PAGE_COUNT(size) * EFI_PAGE_SIZE);

    return EFI_SUCCESS;
}

static EFI_STATUS make_images_contiguous(EFI_BOOT_SERVICES* bs, LIST_ENTRY* images) {
    EFI_STATUS Status;
    LIST_ENTRY* le;
    size_t size = 0;
    EFI_PHYSICAL_ADDRESS addr;

    le = images->Flink;
    while (le != images) {
        image* img = _CR(le, image, list_entry);
        UINT32 imgsize = img->img->GetSize(img->img);

        if ((imgsize % EFI_PAGE_SIZE) != 0)
            imgsize = ((imgsize / EFI_PAGE_SIZE) + 1) * EFI_PAGE_SIZE;

        size += imgsize;

        le = le->Flink;
    }

    if ((size % 0x400000) != 0)
        size += 0x400000 - (size % 0x400000);

    // FIXME - loop through memory map and find address ourselves

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, (size + 0x400000 - EFI_PAGE_SIZE) / EFI_PAGE_SIZE, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    // align to 4MB

    if ((addr % 0x400000) != 0)
        addr += 0x400000 - (addr % 0x400000);

    le = images->Flink;
    while (le != images) {
        image* img = _CR(le, image, list_entry);
        UINT32 imgsize = img->img->GetSize(img->img);

        Status = img->img->MoveAddress(img->img, addr);
        if (EFI_ERROR(Status)) {
            print_error(L"MovePages", Status);
            return Status;
        }

        if ((imgsize % EFI_PAGE_SIZE) != 0)
            imgsize = ((imgsize / EFI_PAGE_SIZE) + 1) * EFI_PAGE_SIZE;

        addr += imgsize;

        le = le->Flink;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS load_drvdb(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE windir, void** va, LIST_ENTRY* mappings,
                             LOADER_EXTENSION_BLOCK1B* extblock1b) {
    EFI_STATUS Status;
    void* data;
    size_t size;

    Status = read_file(bs, windir, L"AppPatch\\drvmain.sdb", &data, &size);

    if (Status == EFI_NOT_FOUND) {
        print(L"drvmain.sdb not found\r\n");
        return EFI_SUCCESS;
    } else if (EFI_ERROR(Status)) {
        print(L"Error when reading AppPatch\\drvmain.sdb.\r\n");
        print_error(L"read_file", Status);
        return Status;
    }

    if (size == 0)
        return EFI_SUCCESS;

    Status = add_mapping(bs, mappings, *va, data, PAGE_COUNT(size), LoaderRegistryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    extblock1b->DrvDBImage = *va;
    extblock1b->DrvDBSize = size;

    *va = (uint8_t*)*va + (PAGE_COUNT(size) * EFI_PAGE_SIZE);

    return EFI_SUCCESS;
}

EFI_STATUS load_image(image* img, WCHAR* name, EFI_PE_LOADER_PROTOCOL* pe, void* va, EFI_FILE_HANDLE dir,
                      command_line* cmdline, uint16_t build) {
    EFI_STATUS Status;
    EFI_FILE_HANDLE file;
    bool is_kdstub = false;

    if (!wcsicmp(name, L"kdcom.dll") && cmdline->debug_type && strcmp(cmdline->debug_type, "com")) {
        unsigned int len = strlen(cmdline->debug_type);
        unsigned int wlen;
        WCHAR* newfile;

        Status = utf8_to_utf16(NULL, 0, &wlen, cmdline->debug_type, len);
        if (EFI_ERROR(Status)) {
            print_error(L"utf8_to_utf16", Status);
            return Status;
        }

        Status = systable->BootServices->AllocatePool(EfiLoaderData, wlen + (7 * sizeof(WCHAR)), (void**)&newfile);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return Status;
        }

        newfile[0] = 'k';
        newfile[1] = 'd';

        Status = utf8_to_utf16(&newfile[2], wlen, &wlen, cmdline->debug_type, len);
        if (EFI_ERROR(Status)) {
            print_error(L"utf8_to_utf16", Status);
            systable->BootServices->FreePool(newfile);
            return Status;
        }

        newfile[(wlen / sizeof(WCHAR)) + 2] = '.';
        newfile[(wlen / sizeof(WCHAR)) + 3] = 'd';
        newfile[(wlen / sizeof(WCHAR)) + 4] = 'l';
        newfile[(wlen / sizeof(WCHAR)) + 5] = 'l';
        newfile[(wlen / sizeof(WCHAR)) + 6] = 0;

        print(L"Opening ");
        print(newfile);
        print(L" instead.\r\n");
        Status = open_file(dir, &file, newfile);

        if (Status == EFI_NOT_FOUND) {
            print(L"Could not find ");
            print(newfile);
            print(L", opening original file.\r\n");

            Status = open_file(dir, &file, name);
        }

        systable->BootServices->FreePool(newfile);
    } else if (!wcsicmp(name, L"kdstub.dll")) {
        Status = EFI_NOT_FOUND;

        if (!strcmp(cmdline->debug_type, "net")) {
            Status = kdnet_init(systable->BootServices, dir, &file, &debug_device_descriptor);

            if (Status == EFI_NOT_FOUND)
                print(L"Could not find override, opening original file.\r\n");
            else if (EFI_ERROR(Status)) {
                print_error(L"kdnet_init", Status);
                return Status;
            } else {
                kdnet_loaded = true;
                is_kdstub = true;
            }
        }

        if (Status == EFI_NOT_FOUND)
            Status = open_file(dir, &file, name);
    } else if (!wcsicmp(name, L"hal.dll") && cmdline->hal) {
        print(L"Opening ");
        print(cmdline->hal);
        print(L" as ");
        print(name);
        print(L".\r\n");

        Status = open_file(dir, &file, cmdline->hal);

        if (Status == EFI_NOT_FOUND) {
            print(L"Could not find ");
            print(cmdline->hal);
            print(L", opening original file.\r\n");

            Status = open_file(dir, &file, name);
        }
    } else if (!wcsicmp(name, L"ntoskrnl.exe") && cmdline->kernel) {
        print(L"Opening ");
        print(cmdline->kernel);
        print(L" as ");
        print(name);
        print(L".\r\n");

        Status = open_file(dir, &file, cmdline->kernel);

        if (Status == EFI_NOT_FOUND) {
            print(L"Could not find ");
            print(cmdline->kernel);
            print(L", opening original file.\r\n");

            Status = open_file(dir, &file, name);
        }
    } else
        Status = open_file(dir, &file, name);

    if (EFI_ERROR(Status)) {
        if (Status != EFI_NOT_FOUND) {
            print(L"Loading of ");
            print(name);
            print(L" failed.\r\n");

            print_error(L"file open", Status);
        }

        return Status;
    }

    img->va = va;

    Status = pe->Load(file, !is_kdstub ? va : NULL, &img->img);
    if (EFI_ERROR(Status)) {
        print_error(L"PE load", Status);
        file->Close(file);
        return Status;
    }

    print(L"Loaded ");
    print(img->name);
    print(L" at ");
    print_hex((uintptr_t)va);
    print(L".\r\n");

    Status = file->Close(file);
    if (EFI_ERROR(Status))
        print_error(L"file close", Status);

    if (is_kdstub) {
        kdstub = img;

        Status = allocate_kdnet_hw_context(img->img, &debug_device_descriptor, build);
        if (EFI_ERROR(Status)) {
            print_error(L"allocate_kdnet_hw_context", Status);
            return Status;
        }

        Status = img->img->Relocate(img->img, (uintptr_t)va);
        if (EFI_ERROR(Status))
            print_error(L"Relocate", Status);
    }

    return Status;
}

static void fix_image_order(LIST_ENTRY* images) {
    image* kernel = _CR(images->Flink, image, list_entry);
    image* hal = _CR(images->Flink->Flink, image, list_entry);
    LIST_ENTRY* le;
    LIST_ENTRY list;
    unsigned int max_order = 0;

    RemoveEntryList(&kernel->list_entry);
    RemoveEntryList(&hal->list_entry);

    le = images->Flink;
    while (le != images) {
        image* img = _CR(le, image, list_entry);

        if (img->order > max_order)
            max_order = img->order;

        le = le->Flink;
    }

    InitializeListHead(&list);

    for (unsigned int i = 0; i <= max_order; i++) {
        le = images->Flink;

        while (le != images) {
            LIST_ENTRY* le2 = le->Flink;
            image* img = _CR(le, image, list_entry);

            if (img->order == i) {
                RemoveEntryList(&img->list_entry);
                InsertTailList(&list, &img->list_entry);
            }

            le = le2;
        }
    }

    // kernel and HAL always need to be first

    InsertHeadList(&list, &hal->list_entry);
    InsertHeadList(&list, &kernel->list_entry);

    // move list
    images->Flink = list.Flink;
    images->Blink = list.Blink;
    images->Flink->Blink = images;
    images->Blink->Flink = images;
}

static EFI_STATUS resolve_forward(char* name, uint64_t* address) {
    WCHAR dll[MAX_PATH];
    LIST_ENTRY* le;
    char* func;

    {
        WCHAR* s;
        char* c;

        c = name;
        s = dll;

        while (*c != 0 && *c != '.') {
            *s = *c;
            s++;
            c++;
        }

        *s = 0;

        func = c;

        if (*func == '.')
            func++;
    }

    // FIXME - handle ordinals

    le = images.Flink;
    while (le != &images) {
        image* img = _CR(le, image, list_entry);
        WCHAR name[MAX_PATH];

        wcsncpy(name, img->name, sizeof(name) / sizeof(WCHAR));

        {
            WCHAR* s = name;

            while (*s != 0) {
                if (*s == '.') {
                    *s = 0;
                    break;
                }

                s++;
            }
        }

        if (wcsicmp(name, dll)) {
            le = le->Flink;
            continue;
        }

        return img->img->FindExport(img->img, func, address, resolve_forward);
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS initialize_csm(EFI_HANDLE image_handle, EFI_BOOT_SERVICES* bs) {
    EFI_GUID guid = EFI_LEGACY_BIOS_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count;
    EFI_STATUS Status;

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);
    if (EFI_ERROR(Status))
        return Status;

    if (count == 0) {
        Status = EFI_NOT_FOUND;
        goto end;
    }

    for (unsigned int i = 0; i < count; i++) {
        EFI_LEGACY_BIOS_PROTOCOL* csm = NULL;

        Status = bs->OpenProtocol(handles[i], &guid, (void**)&csm, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status)) {
            print_error(L"OpenProtocol", Status);
            continue;
        }

        Status = csm->ShadowAllLegacyOproms(csm);
        if (EFI_ERROR(Status)) {
            print_error(L"csm->ShadowAllLegacyOproms", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            goto end;
        }

        bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
    }

    Status = EFI_SUCCESS;

end:
    bs->FreePool(handles);

    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel(image* img, EFI_PE_LOADER_PROTOCOL* pe, void* va, EFI_FILE_HANDLE system32,
                              command_line* cmdline) {
    EFI_STATUS Status;
#ifdef _X86_
    bool try_pae = true;
    int cpu_info[4];

    __cpuid(cpu_info, 0x80000001);

    if (!(cpu_info[3] & 0x40)) // PAE not supported
        try_pae = false;

    if (try_pae) {
        bool nx_supported = true;

        if (cmdline->nx == NX_ALWAYSOFF)
            nx_supported = false;
        else {
            if (!(cpu_info[3] & 0x100000)) // NX not supported
                nx_supported = false;
        }

        if (!nx_supported && (cmdline->pae == PAE_DEFAULT || cmdline->pae == PAE_FORCEDISABLE))
            try_pae = false;
    }

    if (!try_pae) {
        Status = load_image(img, L"ntoskrnl.exe", pe, va, system32, cmdline, 0);
        if (EFI_ERROR(Status)) {
            print_error(L"load_image", Status);
            return Status;
        }

        if (img->img->GetCharacteristics(img->img) & IMAGE_FILE_LARGE_ADDRESS_AWARE) {
            print(L"Error - kernel has PAE flag set\r\n");
            return EFI_INVALID_PARAMETER;
        }

        pae = false;

        return Status;
    }

    if (cmdline->kernel)
        Status = EFI_NOT_FOUND;
    else
        Status = load_image(img, L"ntkrnlpa.exe", pe, va, system32, cmdline, 0);

    if (Status == EFI_NOT_FOUND) {
#endif
        Status = load_image(img, L"ntoskrnl.exe", pe, va, system32, cmdline, 0);
#ifdef _X86_
    }
#endif

    if (EFI_ERROR(Status)) {
        print_error(L"load_image", Status);
        return Status;
    }

#ifdef _X86_
    pae = img->img->GetCharacteristics(img->img) & IMAGE_FILE_LARGE_ADDRESS_AWARE;
#endif

    return EFI_SUCCESS;
}

static bool is_numeric(const char* s) {
    if (s[0] == 0)
        return false;

    while (*s != 0) {
        if (*s < '0' || *s > '9')
            return false;

        s++;
    }

    return true;
}

static void parse_option(const char* option, size_t len, command_line* cmdline) {
    EFI_STATUS Status;

    static const char debugport[] = "DEBUGPORT=";
    static const char hal[] = "HAL=";
    static const char kernel[] = "KERNEL=";
    static const char subvol[] = "SUBVOL=";
#ifdef _X86_
    static const char pae[] = "PAE";
    static const char nopae[] = "NOPAE";
    static const char nx[] = "NOEXECUTE=";
    static const char optin[] = "OPTIN";
    static const char optout[] = "OPTOUT";
    static const char alwaysoff[] = "ALWAYSOFF";
    static const char alwayson[] = "ALWAYSON";
#endif

    if (len > sizeof(debugport) - 1 && !strnicmp(option, debugport, sizeof(debugport) - 1)) {
        Status = systable->BootServices->AllocatePool(EfiLoaderData, len - sizeof(debugport) + 1 + 1, (void**)&cmdline->debug_type);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return;
        }

        memcpy(cmdline->debug_type, option + sizeof(debugport) - 1, len - sizeof(debugport) + 1);
        cmdline->debug_type[len - sizeof(debugport) + 1] = 0;

        // make lowercase
        for (unsigned int i = 0; i < len - sizeof(debugport) + 1; i++) {
            if (cmdline->debug_type[i] >= 'A' && cmdline->debug_type[i] <= 'Z')
                cmdline->debug_type[i] += 'a' - 'A';
        }

        if (cmdline->debug_type[0] == 'c' && cmdline->debug_type[1] == 'o' && cmdline->debug_type[2] == 'm' && is_numeric(&cmdline->debug_type[3]))
            cmdline->debug_type[3] = 0;
    } else if (len > sizeof(hal) - 1 && !strnicmp(option, hal, sizeof(hal) - 1)) {
        unsigned int wlen;

        Status = utf8_to_utf16(NULL, 0, &wlen, &option[sizeof(hal) - 1], len - sizeof(hal) + 1);
        if (EFI_ERROR(Status)) {
            print_error(L"utf8_to_utf16", Status);
            return;
        }

        Status = systable->BootServices->AllocatePool(EfiLoaderData, wlen + sizeof(WCHAR), (void**)&cmdline->hal);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return;
        }

        Status = utf8_to_utf16(cmdline->hal, wlen, &wlen, &option[sizeof(hal) - 1], len - sizeof(hal) + 1);
        if (EFI_ERROR(Status)) {
            print_error(L"utf8_to_utf16", Status);
            systable->BootServices->FreePool(cmdline->hal);
            cmdline->hal = NULL;
            return;
        }

        cmdline->hal[wlen / sizeof(WCHAR)] = 0;
    } else if (len > sizeof(kernel) - 1 && !strnicmp(option, kernel, sizeof(kernel) - 1)) {
        unsigned int wlen;

        Status = utf8_to_utf16(NULL, 0, &wlen, &option[sizeof(kernel) - 1], len - sizeof(kernel) + 1);
        if (EFI_ERROR(Status)) {
            print_error(L"utf8_to_utf16", Status);
            return;
        }

        Status = systable->BootServices->AllocatePool(EfiLoaderData, wlen + sizeof(WCHAR), (void**)&cmdline->kernel);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return;
        }

        Status = utf8_to_utf16(cmdline->kernel, wlen, &wlen, &option[sizeof(kernel) - 1], len - sizeof(kernel) + 1);
        if (EFI_ERROR(Status)) {
            print_error(L"utf8_to_utf16", Status);
            systable->BootServices->FreePool(cmdline->kernel);
            cmdline->kernel = NULL;
            return;
        }

        cmdline->kernel[wlen / sizeof(WCHAR)] = 0;
    } else if (len > sizeof(subvol) - 1 && !strnicmp(option, subvol, sizeof(subvol) - 1)) {
        uint64_t sn = 0;
        const char* s = &option[sizeof(subvol) - 1];

        len -= sizeof(subvol) - 1;

        while (len > 0) {
            sn *= 0x10;

            if (*s >= '0' && *s <= '9')
                sn |= *s - '0';
            else if (*s >= 'a' && *s <= 'f')
                sn |= *s - 'a' + 0xa;
            else if (*s >= 'A' && *s <= 'F')
                sn |= *s - 'A' + 0xa;
            else {
                print(L"Malformed SUBVOL value.\r\n");
                return;
            }

            s++;
            len--;
        }

        cmdline->subvol = sn;
#ifdef _X86_
    } else if (len == sizeof(pae) - 1 && !strnicmp(option, pae, sizeof(pae) - 1))
        cmdline->pae = PAE_FORCEENABLE;
    else if (len == sizeof(nopae) - 1 && !strnicmp(option, nopae, sizeof(nopae) - 1))
        cmdline->pae = PAE_FORCEDISABLE;
    else if (len > sizeof(nx) - 1 && !strnicmp(option, nx, sizeof(nx) - 1)) {
        const char* val = option + sizeof(nx) - 1;
        unsigned int vallen = len - sizeof(nx) + 1;

        if (vallen == sizeof(optin) - 1 && !strnicmp(val, optin, sizeof(optin) - 1))
            cmdline->nx = NX_OPTIN;
        else if (vallen == sizeof(optout) - 1 && !strnicmp(val, optout, sizeof(optout) - 1))
            cmdline->nx = NX_OPTOUT;
        else if (vallen == sizeof(alwaysoff) - 1 && !strnicmp(val, alwaysoff, sizeof(alwaysoff) - 1))
            cmdline->nx = NX_ALWAYSOFF;
        else if (vallen == sizeof(alwayson) - 1 && !strnicmp(val, alwayson, sizeof(alwayson) - 1))
            cmdline->nx = NX_ALWAYSON;
#endif
    }
}

static EFI_STATUS allocate_pcr(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void** va, uint16_t build, void** pcrva) {
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS addr;
    void* pcr;

#ifndef _X86_
    UNUSED(build);
#endif

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PCR_PAGES, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    pcr = (void*)(uintptr_t)addr;

    memset(pcr, 0, EFI_PAGE_SIZE * PCR_PAGES);

#ifdef _X86_
    if (build < WIN10_BUILD_1703)
        *pcrva = (void*)KIP0PCRADDRESS;
    else {
#endif
        *pcrva = *va;
        *va = (uint8_t*)*va + (PCR_PAGES * EFI_PAGE_SIZE);
#ifdef _X86_
    }
#endif

    Status = add_mapping(bs, mappings, *pcrva, pcr, PCR_PAGES, LoaderStartupPcrPage);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

static void parse_options(const char* options, command_line* cmdline) {
    const char* s;
    const char* t;

    memset(cmdline, 0, sizeof(command_line));

    s = options;
    t = s;

    while (true) {
        while (*t != ' ' && *t != 0) {
            t++;
        }

        if (t != s)
            parse_option(s, t - s, cmdline);

        if (*t == 0)
            return;

        t++;
        s = t;
    }
}

static EFI_STATUS set_graphics_mode(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, LIST_ENTRY* mappings, void** va,
                                    uint16_t version, uint16_t build, void* bgc, LOADER_EXTENSION_BLOCK3* extblock3) {
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count;
    EFI_STATUS Status;
    unsigned int bg_version;
    bgblock1* block1;
    bgblock2* block2;

    if (version < _WIN32_WINNT_WINBLUE) { // Win 8 (and 7?)
        BOOT_GRAPHICS_CONTEXT_V1* bgc1 = (BOOT_GRAPHICS_CONTEXT_V1*)bgc;

        bg_version = 1;
        block1 = &bgc1->block1;
        block2 = &bgc1->block2;
    } else if (version == _WIN32_WINNT_WINBLUE || build < WIN10_BUILD_1703) { // 8.1, 1507, 1511, 1607
        BOOT_GRAPHICS_CONTEXT_V2* bgc2 = (BOOT_GRAPHICS_CONTEXT_V2*)bgc;

        bg_version = 2;
        block1 = &bgc2->block1;
        block2 = &bgc2->block2;
    } else if (build < WIN10_BUILD_1803) { // 1703 and 1709
        BOOT_GRAPHICS_CONTEXT_V3* bgc3 = (BOOT_GRAPHICS_CONTEXT_V3*)bgc;

        bg_version = 3;
        block1 = &bgc3->block1;
        block2 = &bgc3->block2;
    } else { // 1803 on
        BOOT_GRAPHICS_CONTEXT_V4* bgc4 = (BOOT_GRAPHICS_CONTEXT_V4*)bgc;

        bg_version = 4;
        block1 = &bgc4->block1;
        block2 = &bgc4->block2;
    }

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);
    if (EFI_ERROR(Status))
        return Status;

    for (unsigned int i = 0; i < count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
        EFI_PHYSICAL_ADDRESS rp;
        unsigned int mode = 0;
        unsigned int pixels = 0;

        Status = bs->OpenProtocol(handles[i], &guid, (void**)&gop, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status)) {
            print_error(L"OpenProtocol", Status);
            continue;
        }

        for (unsigned int j = 0; j < gop->Mode->MaxMode; j++) {
            UINTN size;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;

            Status = gop->QueryMode(gop, j, &size, &info);
            if (EFI_ERROR(Status)) {
                print_error(L"QueryMode", Status);
                continue;
            }

#if 0
            print(L"Mode ");
            print_dec(j);
            print(L": PixelFormat = ");
            print_dec(info->PixelFormat);
            print(L", ");
            print_dec(info->HorizontalResolution);
            print(L"x");
            print_dec(info->VerticalResolution);
            print(L"\r\n");

            if (info->PixelFormat == PixelBitMask) {
                print(L"Bit mask: red = ");
                print_hex(info->PixelInformation.RedMask);
                print(L", green = ");
                print_hex(info->PixelInformation.GreenMask);
                print(L", blue = ");
                print_hex(info->PixelInformation.BlueMask);
                print(L"\r\n");
            }
#endif

            // choose the best mode
            // FIXME - allow user to override this

            if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor &&
                info->HorizontalResolution * info->VerticalResolution > pixels) {
                mode = j;
                pixels = info->HorizontalResolution * info->VerticalResolution;
            }

            // FIXME - does Windows support anything other than BGR?
        }

        Status = gop->SetMode(gop, mode);
        if (EFI_ERROR(Status)) {
            print_error(L"SetMode", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            goto end;
        }

        // map framebuffer

        Status = add_mapping(bs, mappings, *va, (void*)(uintptr_t)gop->Mode->FrameBufferBase,
                             PAGE_COUNT(gop->Mode->FrameBufferSize), LoaderFirmwarePermanent);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            goto end;
        }

        block1->version = bg_version;
        block1->internal.unk1 = 1; // ?
        block1->internal.unk2 = 1; // ?
        block1->internal.unk3 = 0; // ?
        block1->internal.unk4 = 0xc4; // ? (0xf4 is BIOS graphics?)
        block1->internal.height = gop->Mode->Info->VerticalResolution;
        block1->internal.width = gop->Mode->Info->HorizontalResolution;
        block1->internal.pixels_per_scan_line = gop->Mode->Info->PixelsPerScanLine;
        block1->internal.format = 5; // 4 = 24-bit colour, 5 = 32-bit colour (see BgpGetBitsPerPixel)
#ifdef __x86_64__
        block1->internal.bits_per_pixel = 32;
#endif
        block1->internal.framebuffer = *va;

        *va = (uint8_t*)*va + (PAGE_COUNT(gop->Mode->FrameBufferSize) * EFI_PAGE_SIZE);

        // allocate and map reserve pool (used as scratch space?)

        block2->reserve_pool_size = 0x4000;

        Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(block2->reserve_pool_size), &rp);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            goto end;
        }

        Status = add_mapping(bs, mappings, *va, (void*)(uintptr_t)rp,
                             PAGE_COUNT(block2->reserve_pool_size), LoaderFirmwarePermanent); // FIXME - what should the memory type be?
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            goto end;
        }

        block2->reserve_pool = *va;

        *va = (uint8_t*)*va + (PAGE_COUNT(block2->reserve_pool_size) * EFI_PAGE_SIZE);

        // map fonts

        if (system_font) {
            Status = add_mapping(bs, mappings, *va, system_font, PAGE_COUNT(system_font_size),
                                 LoaderFirmwarePermanent); // FIXME - what should the memory type be?
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                goto end;
            }

            block1->system_font = *va;
            block1->system_font_size = system_font_size;

            *va = (uint8_t*)*va + (PAGE_COUNT(system_font_size) * EFI_PAGE_SIZE);
        }

        if (console_font) {
            Status = add_mapping(bs, mappings, *va, console_font, PAGE_COUNT(console_font_size),
                                 LoaderFirmwarePermanent); // FIXME - what should the memory type be?
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                goto end;
            }

            block1->console_font = *va;
            block1->console_font_size = console_font_size;

            *va = (uint8_t*)*va + (PAGE_COUNT(console_font_size) * EFI_PAGE_SIZE);
        }

        extblock3->BgContext = bgc;

        bs->CloseProtocol(handles[i], &guid, image_handle, NULL);

        Status = EFI_SUCCESS;
        goto end;
    }

    Status = EFI_NOT_FOUND;

end:
    bs->FreePool(handles);

    return Status;
}

static EFI_STATUS map_debug_descriptor(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void** va, DEBUG_DEVICE_DESCRIPTOR* ddd) {
    EFI_STATUS Status;
    void* va2 = *va;

    for (unsigned int i = 0; i < MAXIMUM_DEBUG_BARS; i++) {
        if (ddd->BaseAddress[i].Valid && ddd->BaseAddress[i].Type == CmResourceTypeMemory) {
            // FIXME - disable write-caching etc.
            Status = add_mapping(bs, mappings, va2, ddd->BaseAddress[i].TranslatedAddress,
                                 PAGE_COUNT(ddd->BaseAddress[i].Length), LoaderFirmwarePermanent);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            va2 = (uint8_t*)va2 + (PAGE_COUNT(ddd->BaseAddress[i].Length) * EFI_PAGE_SIZE);
        }
    }

//     if (ddd->Memory.Length != 0) {
//         Status = add_mapping(bs, mappings, va2, ddd->Memory.VirtualAddress,
//                              ddd->Memory.Length / EFI_PAGE_SIZE, LoaderFirmwarePermanent);
//         if (EFI_ERROR(Status)) {
//             print_error(L"add_mapping", Status);
//             return Status;
//         }
//
//         va2 = (uint8_t*)va2 + ddd->Memory.Length;
//     }

    *va = va2;

    return EFI_SUCCESS;
}

static EFI_STATUS load_fonts(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE windir) {
    EFI_STATUS Status;
    EFI_FILE_HANDLE fonts = NULL;

    Status = open_file(windir, &fonts, L"Fonts");
    if (EFI_ERROR(Status)) {
        print(L"Could not open Fonts directory.\r\n");
        print_error(L"open_file", Status);
        return Status;
    }

    // FIXME - allow user to choose fonts?

    // Windows 10 uses Segoe Light for system, Segoe Mono Boot for console

    Status = read_file(bs, fonts, L"arial.ttf", &system_font, &system_font_size);
    if (EFI_ERROR(Status)) {
        print_error(L"read_file", Status);
        return Status;
    }

    Status = read_file(bs, fonts, L"cour.ttf", &console_font, &console_font_size);
    if (EFI_ERROR(Status)) {
        print_error(L"read_file", Status);
        return Status;
    }

    return EFI_SUCCESS;
}

#if defined(_MSC_VER) && defined(__x86_64__)
void call_startup(void* stack, void* loader_block, void* KiSystemStartup);
#endif

static EFI_STATUS boot(EFI_HANDLE image_handle, EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE root, char* options,
                       char* path, char* arc_name, EFI_PE_LOADER_PROTOCOL* pe, EFI_REGISTRY_PROTOCOL* reg,
                       command_line* cmdline, WCHAR* fs_driver) {
    EFI_STATUS Status;
    EFI_FILE_HANDLE windir = NULL, system32 = NULL, drivers_dir = NULL;
    LIST_ENTRY mappings;
    KERNEL_ENTRY_POINT KiSystemStartup;
    LIST_ENTRY* le;
    void* va;
    void* va2;
    loader_store* store;
    unsigned int store_pages;
    gdt_entry* gdt;
    idt_entry* idt;
    KTSS* tssphys;
    KTSS* tss;
    KTSS* nmitss = NULL;
    KTSS* dftss = NULL;
    KTSS* mctss = NULL;
    void* usd;
    void* store_va;
    void* registry;
    uint32_t reg_size;
    LIST_ENTRY drivers;
    LIST_ENTRY core_drivers;
    uint32_t version_ms, version_ls;
    uint16_t version;
    uint16_t build, revision;
    LOADER_BLOCK1A* block1a;
    LOADER_BLOCK1B* block1b;
    void** registry_base;
    uint32_t* registry_length;
    LOADER_BLOCK2* block2;
    LOADER_EXTENSION_BLOCK1A* extblock1a;
    LOADER_EXTENSION_BLOCK1B* extblock1b;
    LOADER_EXTENSION_BLOCK3* extblock3;
    uintptr_t* loader_pages_spanned;
    unsigned int pathlen, pathwlen;
    WCHAR* pathw;
    KPCR* pcrva = NULL;
    bool kdstub_export_loaded = false;

    static const WCHAR drivers_dir_path[] = L"system32\\drivers";

    pathlen = strlen(path);

    Status = utf8_to_utf16(NULL, 0, &pathwlen, path, pathlen);
    if (EFI_ERROR(Status)) {
        print_error(L"utf8_to_utf16", Status);
        return Status;
    }

    Status = bs->AllocatePool(EfiLoaderData, pathwlen + sizeof(WCHAR), (void**)&pathw);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    Status = utf8_to_utf16(pathw, pathwlen, &pathwlen, path, pathlen);
    if (EFI_ERROR(Status)) {
        print_error(L"utf8_to_utf16", Status);
        return Status;
    }

    pathw[pathwlen / sizeof(WCHAR)] = 0;

    // check if \\Windows exists
    Status = open_file(root, &windir, pathw);
    if (EFI_ERROR(Status)) {
        print(L"Could not open ");
        print(pathw);
        print(L" on volume.\r\n");
        print_error(L"Open", Status);
        return Status;
    }

    Status = open_file(windir, &system32, L"system32");
    if (EFI_ERROR(Status)) {
        print(L"Could not open system32.\r\n");
        print_error(L"open_file", Status);
        return Status;
    }

    InitializeListHead(&images);
    InitializeListHead(&mappings);

    Status = add_image(bs, &images, L"ntoskrnl.exe", LoaderSystemCode, L"system32", false, NULL, 0, false);
    if (EFI_ERROR(Status)) {
        print_error(L"add_image", Status);
        goto end;
    }

    Status = add_image(bs, &images, L"hal.dll", LoaderHalCode, L"system32", true, NULL, 0, false);
    if (EFI_ERROR(Status)) {
        print_error(L"add_image", Status);
        goto end;
    }

#ifdef _X86_
    va = (void*)0x80000000;
#elif defined(__x86_64__)
    va = (void*)0xfffff80000000000;
#endif

    Status = process_memory_map(bs, &va, &mappings);
    if (EFI_ERROR(Status)) {
        print_error(L"process_memory_map", Status);
        goto end;
    }

    InitializeListHead(&drivers);
    InitializeListHead(&core_drivers);

#ifdef _X86_
    // load kernel at 81800000, to match Windows
    // FIXME - make sure no overlap! Load registry before kernel, and only then errata.inf?

    va2 = (void*)0x81800000;
#elif defined(__x86_64__)
    va2 = (void*)0xfffff80800000000;
#endif

    Status = load_kernel(_CR(images.Flink, image, list_entry), pe, va2, system32, cmdline);
    if (EFI_ERROR(Status)) {
        print_error(L"load_kernel", Status);
        goto end;
    }

    Status = _CR(images.Flink, image, list_entry)->img->GetVersion(_CR(images.Flink, image, list_entry)->img, &version_ms, &version_ls);
    if (EFI_ERROR(Status)) {
        print_error(L"GetVersion", Status);
        goto end;
    }

    version = ((version_ms >> 16) << 8) | (version_ms & 0xff);
    build = version_ls >> 16;
    revision = version_ls & 0xffff;

    // Some checked builds have the wrong version number
    if (build == 9200)
        version = _WIN32_WINNT_WIN8;
    else if (build == 9600)
        version = _WIN32_WINNT_WINBLUE;
    else if (version == 0x0700)
        version = _WIN32_WINNT_WIN7;

    print(L"Booting NT version ");
    print_dec(version >> 8);
    print(L".");
    print_dec(version & 0xff);
    print(L".");
    print_dec(build);
    print(L".");
    print_dec(revision);
    print(L".\r\n");

    Status = load_registry(bs, system32, reg, &registry, &reg_size, &images, &drivers, &mappings, &va, version, build,
                           windir, &core_drivers, fs_driver);
    if (EFI_ERROR(Status)) {
        print_error(L"load_registry", Status);
        goto end;
    }

#if 0
    Status = set_video_mode(bs, image_handle);
    if (EFI_ERROR(Status)) {
        print_error(L"set_video_mode", Status);
        goto end;
    }
#endif
    // FIXME - make sure va doesn't collide with identity-mapped EFI space

    if (version >= _WIN32_WINNT_WIN8) {
        Status = load_api_set(bs, &images, pe, system32, &va, version, &mappings, cmdline);
        if (EFI_ERROR(Status)) {
            print_error(L"load_api_set", Status);
            goto end;
        }
    }

    if (version >= _WIN32_WINNT_WINBLUE) {
        Status = add_image(bs, &images, L"crashdmp.sys", LoaderSystemCode, drivers_dir_path, false, NULL, 0, false);
        if (EFI_ERROR(Status)) {
            print_error(L"add_image", Status);
            goto end;
        }
    }

    va = va2;

    Status = open_file(windir, &drivers_dir, drivers_dir_path);
    if (EFI_ERROR(Status))
        drivers_dir = NULL;

    le = images.Flink;
    while (le != &images) {
        image* img = _CR(le, image, list_entry);

        // FIXME - if we fail opening a driver, fail according to ErrorType value (FS driver should be uber-fail?)

        if (!img->img) {
            bool is_driver_dir = false;

            if (drivers_dir) {
                size_t name_len = wcslen(img->dir);

                is_driver_dir = true;

                if (name_len != (sizeof(drivers_dir_path) / sizeof(WCHAR)) - 1)
                    is_driver_dir = false;
                else {
                    for (unsigned int i = 0; i < (sizeof(drivers_dir_path) / sizeof(WCHAR)) - 1; i++) {
                        WCHAR c1 = drivers_dir_path[i];
                        WCHAR c2 = img->dir[i];

                        if (c1 >= 'A' && c1 <= 'Z')
                            c1 = c1 - 'A' + 'a';

                        if (c2 >= 'A' && c2 <= 'Z')
                            c2 = c2 - 'A' + 'a';

                        if (c1 != c2) {
                            is_driver_dir = false;
                            break;
                        }
                    }
                }
            }

            if (is_driver_dir)
                Status = load_image(img, img->name, pe, va, drivers_dir, cmdline, build);
            else {
                EFI_FILE_HANDLE dir;

                Status = open_file(windir, &dir, img->dir);
                if (EFI_ERROR(Status)) {
                    print(L"Could not open ");
                    print(img->dir);
                    print(L".\r\n");
                    print_error(L"open_file", Status);
                    goto end;
                }

                Status = load_image(img, img->name, pe, va, dir, cmdline, build);

                dir->Close(dir);

                if (Status == EFI_NOT_FOUND)
                    Status = load_image(img, img->name, pe, va, drivers_dir, cmdline, build);
            }

            if (EFI_ERROR(Status)) {
                print_error(L"load_image", Status);
                goto end;
            }
        }

        {
            UINT32 size = img->img->GetSize(img->img);

            if ((size % EFI_PAGE_SIZE) != 0)
                size = ((size / EFI_PAGE_SIZE) + 1) * EFI_PAGE_SIZE;

            va = (uint8_t*)va + size;
        }

        {
            EFI_IMPORT_LIST list;
            UINTN size;

            size = sizeof(list);

            Status = img->img->ListImports(img->img, &list, &size);
            if (Status == EFI_BUFFER_TOO_SMALL) {
                Status = bs->AllocatePool(EfiLoaderData, size, (void**)&img->import_list);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePool", Status);
                    goto end;
                }

                Status = img->img->ListImports(img->img, img->import_list, &size);
                if (EFI_ERROR(Status)) {
                    print_error(L"img->ListImports", Status);
                    goto end;
                }

                for (unsigned int i = 0; i < img->import_list->NumberOfImports; i++) {
                    WCHAR s[MAX_PATH];
                    unsigned int j;
                    char* name = (char*)((uint8_t*)img->import_list + img->import_list->Imports[i]);

                    // FIXME - check length

                    j = 0;
                    do {
                        s[j] = name[j];
                        j++;
                    } while (name[j] != 0);

                    s[j] = 0;

                    // API set DLLs
                    if (version >= _WIN32_WINNT_WIN8 && (s[0] == 'E' || s[0] == 'e') && (s[1] == 'X' || s[1] == 'x') &&
                        (s[2] == 'T' || s[2] == 't') && s[3] == '-') {
                        WCHAR newname[MAX_PATH];

                        if (!search_api_set(s, newname, version))
                            continue;

                        print(L"Using ");
                        print(newname);
                        print(L" instead of ");
                        print(s);
                        print(L".\r\n");

                        wcsncpy(s, newname, sizeof(s) / sizeof(WCHAR));
                    }

                    {
                        LIST_ENTRY* le2 = images.Flink;
                        bool found = false;
                        bool no_reloc = img->no_reloc;

                        if (le == &images || le == images.Flink || img->no_reloc) // kernel or HAL
                            no_reloc = true;

                        while (le2 != &images) {
                            image* img2 = _CR(le2, image, list_entry);

                            if (!wcsicmp(s, img2->name)) {
                                found = true;

                                if (no_reloc)
                                    img2->no_reloc = true;

                                if (img2->order >= img->order)
                                    img2->order = img->order == 0 ? 0 : img->order - 1;

                                break;
                            }

                            le2 = le2->Flink;
                        }

                        if (!found) {
                            Status = add_image(bs, &images, s, LoaderSystemCode, img->dir, true, NULL, img->order == 0 ? 0 : img->order - 1, no_reloc);
                            if (EFI_ERROR(Status))
                                print_error(L"add_image", Status);
                        }
                    }
                }
            } else if (EFI_ERROR(Status)) {
                print_error(L"img->ListImports", Status);
                goto end;
            }
        }

        le = le->Flink;
    }

    if (drivers_dir)
        drivers_dir->Close(drivers_dir);

    if (IsListEmpty(&images)) {
        print(L"Error - no images loaded.\r\n");
        Status = EFI_INVALID_PARAMETER;
        goto end;
    }

    fix_image_order(&images);

    le = images.Flink;
    while (le != &images) {
        image* img = _CR(le, image, list_entry);

        if (!img->import_list) {
            le = le->Flink;
            continue;
        }

        for (unsigned int i = 0; i < img->import_list->NumberOfImports; i++) {
            WCHAR s[MAX_PATH];
            unsigned int j;
            char* name = (char*)((uint8_t*)img->import_list + img->import_list->Imports[i]);

            j = 0;
            do {
                s[j] = name[j];
                j++;
            } while (name[j] != 0);

            s[j] = 0;

            if (version >= _WIN32_WINNT_WIN8 && (s[0] == 'E' || s[0] == 'e') && (s[1] == 'X' || s[1] == 'x') &&
                (s[2] == 'T' || s[2] == 't') && s[3] == '-') {
                WCHAR newname[MAX_PATH];

                if (!search_api_set(s, newname, version))
                    continue;

                wcsncpy(s, newname, sizeof(s) / sizeof(WCHAR));
            }

            {
                LIST_ENTRY* le2 = images.Flink;

                while (le2 != &images) {
                    image* img2 = _CR(le2, image, list_entry);

                    if (!wcsicmp(s, img2->name)) {
                        Status = img->img->ResolveImports(img->img, name, img2->img, resolve_forward);
                        if (EFI_ERROR(Status)) {
                            print(L"Error when resolving imports for ");
                            print(img->name);
                            print(L" and ");
                            print(s);
                            print(L".\r\n");

                            print_error(L"ResolveImports", Status);
                            goto end;
                        }
                        break;
                    }

                    le2 = le2->Flink;
                }
            }
        }

        le = le->Flink;
    }

    Status = make_images_contiguous(bs, &images);
    if (EFI_ERROR(Status)) {
        print_error(L"make_images_contiguous", Status);
        goto end;
    }

    // avoid problems caused by large pages, by shunting virtual address
    // to next 4MB boundary
    va = (uint8_t*)va + (0x400000 - ((uintptr_t)va % 0x400000));

    {
        image* kernel = _CR(images.Flink, image, list_entry);

        Status = kernel->img->GetEntryPoint(kernel->img, (void**)&KiSystemStartup);
        if (EFI_ERROR(Status))
            print_error(L"img->GetEntryPoint", Status);
    }

    if (kdstub) {
        Status = find_kd_export(kdstub->img, build);
        if (EFI_ERROR(Status))
            print_error(L"find_kd_export", Status);
        else
            kdstub_export_loaded = true;
    }

    store = initialize_loader_block(bs, options, path, arc_name, &store_pages, &va, &mappings, &drivers, image_handle, version, build,
                                    revision, &block1a, &block1b, &registry_base, &registry_length, &block2, &extblock1a, &extblock1b,
                                    &extblock3, &loader_pages_spanned, &core_drivers);
    if (!store) {
        print(L"out of memory\r\n");
        Status = EFI_OUT_OF_RESOURCES;
        goto end;
    }

    {
        LIST_ENTRY* le = images.Flink;

        while (le != &images) {
            image* img = _CR(le, image, list_entry);
            uint32_t size, pages;
            IMAGE_SECTION_HEADER* sections;
            UINTN num_sections;

            size = img->img->GetSize(img->img);

            pages = size / EFI_PAGE_SIZE;
            if (size % EFI_PAGE_SIZE != 0)
                pages++;

            Status = add_mapping(bs, &mappings, img->va, (void*)(uintptr_t)img->img->GetAddress(img->img),
                                 pages, img->memory_type);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                goto end;
            }

            Status = img->img->GetSections(img->img, &sections, &num_sections);
            if (EFI_ERROR(Status)) {
                print_error(L"GetSections", Status);
                goto end;
            }

            for (unsigned int i = 0; i < num_sections; i++) {
                uint32_t section_size = sections[i].VirtualSize;
                uint32_t virtaddr = sections[i].VirtualAddress;
                uint32_t section_pages;

                if (virtaddr % EFI_PAGE_SIZE != 0) {
                    section_size += virtaddr % EFI_PAGE_SIZE;
                    virtaddr -= virtaddr % EFI_PAGE_SIZE;
                }

                section_pages = section_size / EFI_PAGE_SIZE;
                if (section_size % EFI_PAGE_SIZE != 0)
                    section_pages++;

                if (!(sections[i].Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)))
                    continue;

                if ((virtaddr / EFI_PAGE_SIZE) + section_pages > pages)
                    section_pages = (virtaddr / EFI_PAGE_SIZE) > pages ? 0 : pages - (virtaddr / EFI_PAGE_SIZE);
            }

            le = le->Flink;
        }
    }

    Status = add_mapping(bs, &mappings, va, store, store_pages, LoaderSystemBlock);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        goto end;
    }

    store_va = (loader_store*)va;
    va = (uint8_t*)va + (store_pages * EFI_PAGE_SIZE);

    Status = generate_images_list(bs, &images, block1a, &va, &mappings);
    if (EFI_ERROR(Status)) {
        print_error(L"generate_images_list", Status);
        goto end;
    }

    tssphys = allocate_tss(bs);
    if (!tssphys) {
        print(L"out of memory\r\n");
        Status = EFI_OUT_OF_RESOURCES;
        goto end;
    }

    Status = add_mapping(bs, &mappings, va, tssphys, PAGE_COUNT(sizeof(KTSS)), LoaderMemoryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        goto end;
    }

    tss = (KTSS*)va;
    va = (uint8_t*)va + (PAGE_COUNT(sizeof(KTSS)) * EFI_PAGE_SIZE);

#ifndef _X86_
    if (build >= WIN10_BUILD_1703) {
#endif
        Status = allocate_pcr(bs, &mappings, &va, build, (void**)&pcrva);
        if (EFI_ERROR(Status)) {
            print_error(L"allocate_pcr", Status);
            goto end;
        }
#ifndef _X86_
    }
#endif

    if (build >= WIN10_BUILD_1703)
        block1b->Prcb = &pcrva->PrcbData;

    usd = allocate_page(bs);
    if (!usd) {
        print(L"out of memory\r\n");
        Status = EFI_OUT_OF_RESOURCES;
        goto end;
    }

    memset(usd, 0, EFI_PAGE_SIZE);

    Status = add_mapping(bs, &mappings, (void*)KI_USER_SHARED_DATA, usd, 1, LoaderStartupPcrPage);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        goto end;
    }

#ifdef _X86_
    if (build >= WIN10_BUILD_1803) {
        void* nmitsspa;
        void* dftsspa;
        void* mctsspa;

        nmitsspa = allocate_page(bs);
        if (!nmitsspa) {
            print(L"out of memory\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto end;
        }

        memset(nmitsspa, 0, EFI_PAGE_SIZE);

        Status = add_mapping(bs, &mappings, va, nmitsspa, 1, LoaderMemoryData);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        nmitss = (KTSS*)va;
        va = (uint8_t*)va + EFI_PAGE_SIZE;

        dftsspa = allocate_page(bs);
        if (!dftsspa) {
            print(L"out of memory\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto end;
        }

        memset(dftsspa, 0, EFI_PAGE_SIZE);

        Status = add_mapping(bs, &mappings, va, dftsspa, 1, LoaderMemoryData);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        dftss = (KTSS*)va;
        va = (uint8_t*)va + EFI_PAGE_SIZE;

        mctsspa = allocate_page(bs);
        if (!mctsspa) {
            print(L"out of memory\r\n");
            Status = EFI_OUT_OF_RESOURCES;
            goto end;
        }

        memset(mctsspa, 0, EFI_PAGE_SIZE);

        Status = add_mapping(bs, &mappings, va, mctsspa, 1, LoaderMemoryData);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        mctss = (KTSS*)va;
        va = (uint8_t*)va + EFI_PAGE_SIZE;
    }
#endif

    gdt = initialize_gdt(bs, tss, nmitss, dftss, mctss, version, pcrva);
    if (!gdt) {
        print(L"initialize_gdt failed\r\n");
        Status = EFI_OUT_OF_RESOURCES;
        goto end;
    }

    Status = add_mapping(bs, &mappings, va, gdt, GDT_PAGES, LoaderMemoryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        goto end;
    }

    gdt = (gdt_entry*)va;
    va = (uint8_t*)va + (GDT_PAGES * EFI_PAGE_SIZE);

    idt = initialize_idt(bs);
    if (!gdt) {
        print(L"initialize_idt failed\r\n");
        Status = EFI_OUT_OF_RESOURCES;
        goto end;
    }

    Status = add_mapping(bs, &mappings, va, idt, IDT_PAGES, LoaderMemoryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        goto end;
    }

    idt = (idt_entry*)va;
    va = (uint8_t*)va + (IDT_PAGES * EFI_PAGE_SIZE);

    {
        EFI_PHYSICAL_ADDRESS addr;
        unsigned int allocation;

        /* It's not at all clear whether KernelStack ought to point to the beginning or
         * the end of the stack - MiMarkBootKernelStack assumes it's the beginnng, but once
         * we start to use it it's the end. To get round this, we allocate twice as much space
         * as we need to, and set KernelStack to be the midpoint. */

        allocation = KERNEL_STACK_SIZE;

        /* Allocate an extra page, so that nt!MiMarkBootGuardPage on Windows 8.1 doesn't clobber
         * something else. */
        allocation++;

        allocation *= 2;

        /* nt!KiInitializePcr on Windows 8.1 wants another 16KB for the ISR stack. */
        // FIXME - find out why amd64 1903 wants even more (what's the MDL memory type?)
        if (version >= _WIN32_WINNT_WIN10)
            allocation += 800; // FIXME - this is a guess. Dispatcher gets corrupted on multi-core system if this is too low?
        else if (version >= _WIN32_WINNT_WINBLUE)
            allocation += 4;

        Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, allocation, &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            goto end;
        }

        Status = add_mapping(bs, &mappings, va, (void*)(uintptr_t)addr, allocation, LoaderStartupKernelStack);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        block1b->KernelStack = (uint8_t*)va + ((KERNEL_STACK_SIZE + 1) * EFI_PAGE_SIZE); // end of stack

        va = (uint8_t*)va + (allocation * EFI_PAGE_SIZE);
    }

    find_apic();

    Status = map_nls(bs, &store->nls, &va, &mappings);
    if (EFI_ERROR(Status)) {
        print_error(L"map_nls", Status);
        goto end;
    }

    Status = load_drvdb(bs, windir, &va, &mappings, extblock1b);
    if (EFI_ERROR(Status)) {
        print_error(L"load_drvdb", Status);
        goto end;
    }

    if (errata_inf) {
        Status = map_errata_inf(bs, extblock1a, &va, &mappings);
        if (EFI_ERROR(Status)) {
            print_error(L"map_errata_inf", Status);
            goto end;
        }
    }

    Status = add_mapping(bs, &mappings, va, registry, PAGE_COUNT(reg_size), LoaderRegistryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        goto end;
    }

    if (version >= _WIN32_WINNT_WIN8) {
        Status = load_fonts(bs, windir);
        if (EFI_ERROR(Status))
            print_error(L"load_fonts", Status); // non-fatal
    }

    windir->Close(windir);
    windir = NULL;

    system32->Close(system32);
    system32 = NULL;

    *registry_base = va;
    *registry_length = reg_size;

    va = (uint8_t*)va + (PAGE_COUNT(reg_size) * EFI_PAGE_SIZE);

    Status = map_efi_runtime(bs, &mappings, &va, version);
    if (EFI_ERROR(Status)) {
        print_error(L"map_efi_runtime", Status);
        return Status;
    }

    if (version == _WIN32_WINNT_WINBLUE) {
        store->loader_block_win81.FirmwareInformation.EfiInformation.EfiMemoryMap = efi_runtime_map;
        store->loader_block_win81.FirmwareInformation.EfiInformation.EfiMemoryMapSize = efi_runtime_map_size;
        store->loader_block_win81.FirmwareInformation.EfiInformation.EfiMemoryMapDescriptorSize = map_desc_size;
    } else if (version == _WIN32_WINNT_WIN10) {
        store->loader_block_win10.FirmwareInformation.EfiInformation.EfiMemoryMap = efi_runtime_map;
        store->loader_block_win10.FirmwareInformation.EfiInformation.EfiMemoryMapSize = efi_runtime_map_size;
        store->loader_block_win10.FirmwareInformation.EfiInformation.EfiMemoryMapDescriptorSize = map_desc_size;
    }

    Status = map_debug_descriptor(bs, &mappings, &va, &store->debug_device_descriptor);
    if (EFI_ERROR(Status)) {
        print_error(L"map_debug_descriptor", Status);
        return Status;
    }

#ifdef __x86_64__
    {
        EFI_PHYSICAL_ADDRESS addr;
        unsigned int pages;

        pages = KERNEL_STACK_SIZE;

        pages++; // for nt!MiMarkBootGuardPage on Windows 8.1

        Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            goto end;
        }

        Status = add_mapping(bs, &mappings, va, (void*)(uintptr_t)addr, pages, LoaderStartupKernelStack);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        va = (uint8_t*)va + (pages * EFI_PAGE_SIZE);

        tssphys->Rsp0 = (uintptr_t)va; // end of stack

        // Some interrupts, such as 2 (NMI), have their own stacks. We allocate all 8 just in case,
        // but Windows won't use all of them.

        for (unsigned int i = 0; i < 8; i++) {
            Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePages", Status);
                goto end;
            }

            Status = add_mapping(bs, &mappings, va, (void*)(uintptr_t)addr, pages, LoaderStartupKernelStack);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                goto end;
            }

            va = (uint8_t*)va + (pages * EFI_PAGE_SIZE);

            tssphys->Ist[i] = (uintptr_t)va; // end of stack
        }
    }
#endif

    root->Close(root);

    if (kdstub_export_loaded && kdnet_scratch) {
        Status = add_mapping(bs, &mappings, va, kdnet_scratch,
                             PAGE_COUNT(store->debug_device_descriptor.TransportData.HwContextSize), LoaderFirmwarePermanent);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            goto end;
        }

        kdnet_scratch = va;
        va = (uint8_t*)va + (PAGE_COUNT(store->debug_device_descriptor.TransportData.HwContextSize) * EFI_PAGE_SIZE);
    }

    if (version >= _WIN32_WINNT_WIN8) {
        Status = set_graphics_mode(bs, image_handle, &mappings, &va, version, build, &store->bgc, extblock3);
        if (EFI_ERROR(Status)) {
            print_error(L"set_graphics_mode", Status);
            print(L"GOP failed, falling back to CSM\r\n");
        }
    } else
        Status = EFI_NOT_FOUND;

    if (EFI_ERROR(Status)) {
        Status = initialize_csm(image_handle, bs);
        if (EFI_ERROR(Status)) {
            print_error(L"initialize_csm", Status);
            goto end;
        }
    }

    fix_store_mapping(store, store_va, &mappings, version, build);

    Status = enable_paging(image_handle, bs, &mappings, block1a, va, loader_pages_spanned);
    if (EFI_ERROR(Status)) {
        print_error(L"enable_paging", Status);
        goto end;
    }

    store = (loader_store*)store_va;
    store2 = store;

    set_gdt(gdt);
    set_idt(idt);

#if defined(_X86_) || defined(__x86_64__)
    /* Re-enable IDE interrupts - the IDE driver on OVMF disables them when not expecting anything,
     * which confuses Vista. */
    __outbyte(0x3f6, 0);
    __outbyte(0x376, 0);
#endif

//     halt();

    if (kdstub_export_loaded)
        kdstub_init(&store->debug_device_descriptor, build);

    // FIXME - can we print net_error_string and net_error_status somehow if this fails?

#ifdef __x86_64__
    // set syscall flag in EFER MSR
    __writemsr(0xc0000080, __readmsr(0xc0000080) | 1);

#ifndef _MSC_VER
    __asm__ __volatile__ (
        "mov rsp, %0\n\t"
        "lea rcx, %1\n\t"
        "call %2\n\t"
        :
        : "m" (tss->Rsp0), "m" (store->loader_block), "m" (KiSystemStartup)
        : "rcx"
    );
#else
    call_startup(tss->Rsp0, &store->loader_block, KiSystemStartup);
#endif

#else
    KiSystemStartup(&store->loader_block);
#endif

end:
    if (windir) {
        EFI_STATUS Status2 = windir->Close(windir);
        if (EFI_ERROR(Status2))
            print_error(L"windir close", Status2);
    }

    while (!IsListEmpty(&images)) {
        image* img = _CR(images.Flink, image, list_entry);

        if (img->img) {
            EFI_STATUS Status2 = img->img->Free(img->img);
            if (EFI_ERROR(Status2))
                print_error(L"img->Free", Status2);
        }

        if (img->import_list)
            bs->FreePool(img->import_list);

        RemoveEntryList(&img->list_entry);

        bs->FreePool(img);
    }

    while (!IsListEmpty(&mappings)) {
        mapping* m = _CR(mappings.Flink, mapping, list_entry);

        RemoveEntryList(&m->list_entry);

        bs->FreePool(m);
    }

    return Status;
}

static EFI_STATUS load_reg_proto(EFI_BOOT_SERVICES* bs, EFI_HANDLE ImageHandle, EFI_REGISTRY_PROTOCOL** reg) {
    EFI_STATUS Status;
    EFI_HANDLE* handles = NULL;
    UINTN count = 0;
    EFI_GUID guid = WINDOWS_REGISTRY_PROTOCOL;

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);
    if (EFI_ERROR(Status)) {
        print_error(L"bs->LocateHandleBuffer", Status);
        return Status;
    }

    for (unsigned int i = 0; i < count; i++) {
        Status = bs->OpenProtocol(handles[i], &guid, (void**)reg, ImageHandle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status))
            continue;

        bs->FreePool(handles);

        return Status;
    }

    // FIXME - what about CloseProtocol?

    print(L"Registry protocol not found.\r\n");

    bs->FreePool(handles);

    return EFI_NOT_FOUND;
}

static EFI_STATUS load_pe_proto(EFI_BOOT_SERVICES* bs, EFI_HANDLE ImageHandle, EFI_PE_LOADER_PROTOCOL** pe) {
    EFI_STATUS Status;
    EFI_HANDLE* handles = NULL;
    UINTN count = 0;
    EFI_GUID guid = PE_LOADER_PROTOCOL;

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);
    if (EFI_ERROR(Status)) {
        print_error(L"bs->LocateHandleBuffer", Status);
        return Status;
    }

    for (unsigned int i = 0; i < count; i++) {
        Status = bs->OpenProtocol(handles[i], &guid, (void**)pe, ImageHandle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status))
            continue;

        bs->FreePool(handles);

        return Status;
    }

    // FIXME - what about CloseProtocol?

    print(L"PE loader not found.\r\n");

    bs->FreePool(handles);

    return EFI_NOT_FOUND;
}

#ifndef _MSC_VER
static void change_stack2(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, void* stack_end, change_stack_cb cb) {
#ifdef _X86_
    __asm__ __volatile__ (
        "mov eax, %0\n\t"
        "mov ebx, %1\n\t"
        "mov ecx, %3\n\t"
        "mov edx, esp\n\t"
        "mov esp, %2\n\t"
        "push ebp\n\t"
        "mov ebp, esp\n\t"
        "push edx\n\t"
        "push ebx\n\t"
        "push eax\n\t"
        "call ecx\n\t"
        "pop edx\n\t"
        "pop ebp\n\t"
        "mov esp, edx\n\t"
        :
        : "m" (bs), "m" (image_handle), "m" (stack_end), "" (cb)
        : "eax", "ebx", "ecx", "edx"
    );
#elif defined(__x86_64__)
    // FIXME - probably should restore original rbx

    __asm__ __volatile__ (
        "mov rcx, %0\n\t"
        "mov rdx, %1\n\t"
        "mov rax, %3\n\t"
        "mov rbx, rsp\n\t"
        "mov rsp, %2\n\t"
        "push rbp\n\t"
        "mov rbp, rsp\n\t"
        "push rbx\n\t"
        "sub rsp, 32\n\t"
        "call rax\n\t"
        "add rsp, 32\n\t"
        "pop rbx\n\t"
        "pop rbp\n\t"
        "mov rsp, rbx\n\t"
        :
        : "m" (bs), "m" (image_handle), "m" (stack_end), "" (cb)
        : "rax", "rcx", "rdx", "rbx"
    );
#endif
}
#else // in ASM file
void __stdcall change_stack2(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, void* stack_end, change_stack_cb cb);
#endif

static EFI_STATUS change_stack(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, change_stack_cb cb) {
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS addr;
    void* stack_end;

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, STACK_SIZE, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    stack = (void*)(uintptr_t)addr;
    stack_end = (uint8_t*)stack + (STACK_SIZE * EFI_PAGE_SIZE);

#ifdef __x86_64__
    // GCC's function prologue on amd64 uses [rbp+0x10] and [rbp+0x18]
    stack_end = (uint8_t*)stack_end - EFI_PAGE_SIZE;
#endif

    change_stack2(bs, image_handle, stack_end, cb);

    // only returns if unsuccessful

    return EFI_SUCCESS;
}

static EFI_STATUS create_file_device_path(EFI_BOOT_SERVICES* bs, EFI_DEVICE_PATH* fs, const WCHAR* path,
                                          EFI_DEVICE_PATH_PROTOCOL** pdp) {
    EFI_STATUS Status;
    size_t path_len, fslen, fplen;
    FILEPATH_DEVICE_PATH* fp;
    EFI_DEVICE_PATH_PROTOCOL* dp;
    EFI_DEVICE_PATH_PROTOCOL* end_dp;

    path_len = wcslen(path) * sizeof(WCHAR);
    fplen = offsetof(FILEPATH_DEVICE_PATH, PathName[0]) + path_len + sizeof(WCHAR);

    {
        EFI_DEVICE_PATH_PROTOCOL* dpbit = fs;

        fslen = 0;
        do {
            if (dpbit->Type == END_DEVICE_PATH_TYPE)
                break;

            fslen += *(uint16_t*)dpbit->Length;
            dpbit = (EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)dpbit + *(uint16_t*)dpbit->Length);
        } while (true);
    }

    Status = bs->AllocatePool(EfiLoaderData, fslen + fplen + sizeof(EFI_DEVICE_PATH_PROTOCOL), (void**)&dp);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    memcpy(dp, fs, fslen);

    fp = (FILEPATH_DEVICE_PATH*)((uint8_t*)dp + fslen);

    fp->Header.Type = MEDIA_DEVICE_PATH;
    fp->Header.SubType = MEDIA_FILEPATH_DP;
    *(uint16_t*)fp->Header.Length = fplen;

    memcpy(fp->PathName, path, path_len + sizeof(WCHAR));

    end_dp = (EFI_DEVICE_PATH_PROTOCOL*)&fp->PathName[(path_len / sizeof(WCHAR)) + 1];
    SetDevicePathEndNode(end_dp);

    *pdp = dp;

    return EFI_SUCCESS;
}

EFI_STATUS open_parent_dir(EFI_FILE_IO_INTERFACE* fs, FILEPATH_DEVICE_PATH* dp, EFI_FILE_HANDLE* dir) {
    EFI_STATUS Status;
    unsigned int len;
    WCHAR* name;
    EFI_FILE_HANDLE root;

    if (dp->Header.Type != MEDIA_DEVICE_PATH || dp->Header.SubType != MEDIA_FILEPATH_DP)
        return EFI_INVALID_PARAMETER;

    len = *(uint16_t*)dp->Header.Length / sizeof(CHAR16);

    if (len == 0)
        return EFI_INVALID_PARAMETER;

    for (int i = len - 1; i >= 0; i--) {
        if (dp->PathName[i] == '\\') {
            len = i;
            break;
        }
    }

    if (len == 0) {
        if (dp->PathName[0] == '\\')
            len = 1;
        else
            return EFI_INVALID_PARAMETER;
    }

    Status = systable->BootServices->AllocatePool(EfiLoaderData, (len + 1) * sizeof(WCHAR), (void**)&name);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    memcpy(name, dp->PathName, len * sizeof(WCHAR));
    name[len] = 0;

    Status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(Status)) {
        print_error(L"OpenVolume", Status);
        systable->BootServices->FreePool(name);
        return Status;
    }

    Status = root->Open(root, dir, name, EFI_FILE_MODE_READ, 0);

    systable->BootServices->FreePool(name);
    root->Close(root);

    return Status;
}

static EFI_STATUS load_efi_drivers(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle) {
    EFI_STATUS Status;
    EFI_GUID guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID guid2 = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_GUID guid3 = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* image;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE dir, drivers;
    EFI_DEVICE_PATH* device_path;
    bool drivers_loaded = false;
    char buf[1024];

    static const WCHAR drivers_dir[] = L"drivers";

    Status = bs->OpenProtocol(image_handle, &guid, (void**)&image, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error(L"OpenProtocol", Status);
        return Status;
    }

    if (!image->DeviceHandle)
        goto end2;

    Status = bs->OpenProtocol(image->DeviceHandle, &guid2, (void**)&fs, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error(L"OpenProtocol", Status);
        goto end2;
    }

    Status = open_parent_dir(fs, (FILEPATH_DEVICE_PATH*)image->FilePath, &dir);
    if (EFI_ERROR(Status)) {
        print_error(L"open_parent_dir", Status);
        goto end;
    }

    Status = bs->HandleProtocol(image->DeviceHandle, &guid3, (void**)&device_path);
    if (EFI_ERROR(Status)) {
        print_error(L"HandleProtocol", Status);
        goto end;
    }

    Status = dir->Open(dir, &drivers, (WCHAR*)drivers_dir, EFI_FILE_MODE_READ, 0);

    dir->Close(dir);

    if (Status == EFI_NOT_FOUND) {
        // drivers directory not found - return without error
        Status = EFI_SUCCESS;
        goto end;
    } else if (EFI_ERROR(Status)) {
        print(L"Error opening \"drivers\" directory.\r\n");
        print_error(L"Open", Status);
        goto end;
    }

    do {
        WCHAR* fn;
        UINTN size;
        size_t len;
        EFI_HANDLE h;
        EFI_DEVICE_PATH_PROTOCOL* dp;
        WCHAR path[MAX_PATH];

        size = sizeof(buf);

        Status = drivers->Read(drivers, &size, buf);
        if (EFI_ERROR(Status)) {
            print_error(L"Read", Status);
            drivers->Close(drivers);
            goto end;
        }

        if (size == 0)
            break;

        fn = ((EFI_FILE_INFO*)buf)->FileName;

        len = wcslen(fn);

        // skip if not .efi file
        if (len < 4 || (fn[len - 1] != 'i' && fn[len - 1] != 'I') || (fn[len - 2] != 'f' && fn[len - 2] != 'F') ||
            (fn[len - 3] != 'e' && fn[len - 3] != 'E') || fn[len - 4] != '.')
            continue;

        print(L"Loading driver ");
        print(fn);
        print(L"... ");

        memcpy(path, ((FILEPATH_DEVICE_PATH*)image->FilePath)->PathName, *(uint16_t*)image->FilePath->Length);
        path[*(uint16_t*)image->FilePath->Length / sizeof(WCHAR)] = 0;

        for (int i = *(uint16_t*)image->FilePath->Length / sizeof(WCHAR); i >= 0; i--) {
            if (path[i] == '\\') {
                path[i+1] = 0;
                break;
            }
        }

        wcsncat(path, drivers_dir, sizeof(path) / sizeof(WCHAR));
        wcsncat(path, L"\\", sizeof(path) / sizeof(WCHAR));
        wcsncat(path, fn, sizeof(path) / sizeof(WCHAR));

        Status = create_file_device_path(bs, device_path, path, &dp);
        if (EFI_ERROR(Status)) {
            print(L"FAILED\r\n");
            bs->FreePool(dp);
            continue;
        }

        Status = bs->LoadImage(false, image_handle, dp, NULL, 0, &h);
        if (EFI_ERROR(Status)) {
            print(L"FAILED\r\n");
            bs->FreePool(dp);
            continue;
        }

        bs->FreePool(dp);

        Status = bs->StartImage(h, NULL, NULL);
        if (EFI_ERROR(Status)) {
            print(L"FAILED\r\n");
            continue;
        }

        print(L"success\r\n");
        drivers_loaded = true;
    } while (true);

    drivers->Close(drivers);

    if (drivers_loaded) {
        UINTN count;
        EFI_HANDLE* handles;

        Status = bs->LocateHandleBuffer(AllHandles, NULL, NULL, &count, &handles);
        if (EFI_ERROR(Status)) {
            print_error(L"LocateHandleBuffer", Status);
            goto end;
        }

        for (unsigned int i = 0; i < count; i++) {
            bs->ConnectController(handles[i], NULL, NULL, true);
        }

        bs->FreePool(handles);
    }

    Status = EFI_SUCCESS;

end:
    bs->CloseProtocol(image->DeviceHandle, &guid2, image_handle, NULL);

end2:
    bs->CloseProtocol(image_handle, &guid, image_handle, NULL);

    return Status;
}

static bool parse_arc_partition_name(const char* arc_name, unsigned int arc_name_len, unsigned int* disknum,
                                     unsigned int* partnum) {
    const char* s;

    const char arc_prefix[] = "multi(0)disk(0)rdisk(";
    const char arc_mid[] = ")partition(";

    if (arc_name_len < sizeof(arc_prefix) - 1 || memcmp(arc_name, arc_prefix, sizeof(arc_prefix) - 1))
        return false;

    *disknum = 0;

    s = arc_name + sizeof(arc_prefix) - 1;
    arc_name_len -= sizeof(arc_prefix) - 1;

    while (*s >= '0' && *s <= '9') {
        *disknum *= 10;
        *disknum += *s - '0';
        s++;
        arc_name_len--;
    }

    if (arc_name_len < (signed int)(sizeof(arc_mid) - 1) || memcmp(s, arc_mid, sizeof(arc_mid) - 1))
        return false;

    s += sizeof(arc_mid) - 1;
    arc_name_len -= sizeof(arc_mid) - 1;

    *partnum = 0;

    while (*s >= '0' && *s <= '9') {
        *partnum *= 10;
        *partnum += *s - '0';
        s++;
        arc_name_len--;
    }

    if (*s != ')')
        return false;

    s++;
    arc_name_len--;

    if (arc_name_len != 0)
        return false;

    return true;
}

static EFI_STATUS parse_arc_name(EFI_BOOT_SERVICES* bs, char* system_path, EFI_FILE_IO_INTERFACE** fs,
                                 char** arc_name, char** path, EFI_HANDLE* fs_handle) {
    EFI_STATUS Status;
    EFI_GUID guid = SIMPLE_FILE_SYSTEM_PROTOCOL;
    char* s;
    unsigned int vollen;
    unsigned int disknum, partnum;
    block_device* bd;
    LIST_ENTRY* le;

    s = system_path;
    while (*s != '\\' && *s != 0) {
        s++;
    }

    vollen = s - system_path;

    *path = s;

    if (parse_arc_partition_name(system_path, vollen, &disknum, &partnum)) {
        // find volume
        bd = NULL;

        le = block_devices.Flink;
        while (le != &block_devices) {
            block_device* bd2 = _CR(le, block_device, list_entry);

            if (bd2->disk_num == disknum && bd2->part_num == partnum) {
                bd = bd2;
                break;
            }

            le = le->Flink;
        }

        if (!bd) {
            print(L"Could not find partition ");
            print_dec(partnum);
            print(L" on disk ");
            print_dec(disknum);
            print(L".\r\n");
            return EFI_INVALID_PARAMETER;
        }

        Status = bs->LocateDevicePath(&guid, &bd->device_path, fs_handle);
        if (EFI_ERROR(Status)) {
            print(L"Could not open filesystem protocol for device path. Is filesystem driver installed?\r\n");
            print_error(L"LocateDevicePath", Status);
            return Status;
        }

        Status = bs->OpenProtocol(*fs_handle, &guid, (void**)fs, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status)) {
            print_error(L"OpenProtocol", Status);
            return Status;
        }
    } else {
        EFI_GUID quibble_guid = EFI_QUIBBLE_PROTOCOL_GUID;
        EFI_HANDLE* handles = NULL;
        UINTN count;

        Status = bs->LocateHandleBuffer(ByProtocol, &quibble_guid, NULL, &count, &handles);
        if (EFI_ERROR(Status)) {
            print(L"Unable to parse ARC name.\r\n");
            return Status;
        }

        *fs = NULL;

        for (unsigned int i = 0; i < count; i++) {
            EFI_QUIBBLE_PROTOCOL* quib = NULL;
            char* buf = NULL;
            UINTN len = 0;

            Status = bs->OpenProtocol(handles[i], &quibble_guid, (void**)&quib, image_handle, NULL,
                                      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
            if (EFI_ERROR(Status)) {
                print_error(L"OpenProtocol", Status);
                continue;
            }

            Status = quib->GetArcName(quib, NULL, &len);

            if (Status == EFI_BUFFER_TOO_SMALL) {
                Status = bs->AllocatePool(EfiLoaderData, len, (void**)&buf);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePool", Status);
                    continue;
                }

                Status = quib->GetArcName(quib, buf, &len);
            }

            if (Status == EFI_SUCCESS && buf) {
                if (len == vollen && !memcmp(system_path, buf, vollen)) {
                    bs->FreePool(buf);
                    bs->CloseProtocol(handles[i], &quibble_guid, image_handle, NULL);

                    Status = bs->OpenProtocol(handles[i], &guid, (void**)fs, image_handle, NULL,
                                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
                    if (EFI_ERROR(Status)) {
                        print_error(L"OpenProtocol", Status);
                        return Status;
                    }

                    *fs_handle = handles[i];

                    break;
                }
            }

            if (buf)
                bs->FreePool(buf);

            bs->CloseProtocol(handles[i], &quibble_guid, image_handle, NULL);
        }

        if (handles)
            bs->FreePool(handles);

        if (!*fs) {
            print(L"Unable to parse ARC name.\r\n");
            return EFI_INVALID_PARAMETER;
        }
    }

    Status = bs->AllocatePool(EfiLoaderData, *path - system_path + 1, (void**)arc_name);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    memcpy(*arc_name, system_path, *path - system_path);
    (*arc_name)[*path - system_path] = 0;

    if ((*path)[0] == '\\')
        (*path)++;

    return EFI_SUCCESS;
}

static void EFIAPI stack_changed(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle) {
    EFI_STATUS Status;
    UINTN Event;
    boot_option* opt;
    char* path;
    EFI_GUID guid = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_GUID quibble_guid = EFI_QUIBBLE_PROTOCOL_GUID;
    EFI_HANDLE fs_handle = NULL;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_QUIBBLE_PROTOCOL* quib;
    EFI_REGISTRY_PROTOCOL* reg = NULL;
    EFI_PE_LOADER_PROTOCOL* pe = NULL;
    char* arc_name;
    command_line cmdline;
    EFI_FILE_HANDLE root = NULL;
    WCHAR* fs_driver = NULL;

    Status = show_menu(systable, &opt);
    if (Status == EFI_ABORTED)
        return;
    else if (EFI_ERROR(Status)) {
        print_error(L"show_menu", Status);
        return;
    }

    if (!opt->system_path) {
        print(L"SystemPath not set.\r\n");
        bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
        return;
    }

    Status = parse_arc_name(bs, opt->system_path, &fs, &arc_name, &path, &fs_handle);
    if (EFI_ERROR(Status)) {
        bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
        return;
    }

    {
        // replace slashes in options with spaces
        char* c = opt->options;

        while (*c != 0) {
            if (*c == '/')
                *c = ' ';

            c++;
        }
    }

    Status = load_reg_proto(bs, image_handle, &reg);
    if (EFI_ERROR(Status)) {
        print_error(L"load_reg_proto", Status);
        bs->FreePool(arc_name);
        bs->CloseProtocol(fs_handle, &guid, image_handle, NULL);
        bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
        return;
    }

    Status = load_pe_proto(bs, image_handle, &pe);
    if (EFI_ERROR(Status)) {
        print_error(L"load_pe_proto", Status);
        bs->FreePool(arc_name);
        bs->CloseProtocol(fs_handle, &guid, image_handle, NULL);
        bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
        return;
    }

    // test for quibble proto, and get new ARC name
    Status = bs->OpenProtocol(fs_handle, &quibble_guid, (void**)&quib, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (!EFI_ERROR(Status)) {
        UINTN len = path - opt->system_path;

        Status = quib->GetArcName(quib, arc_name, &len);

        if (Status == EFI_BUFFER_TOO_SMALL) {
            bs->FreePool(arc_name);

            Status = bs->AllocatePool(EfiLoaderData, len + 1, (void**)&arc_name);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                bs->CloseProtocol(fs_handle, &quibble_guid, image_handle, NULL);
                bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
                return;
            }

            Status = quib->GetArcName(quib, arc_name, &len);
        }

        if (EFI_ERROR(Status) && Status != EFI_UNSUPPORTED) {
            print_error(L"GetArcName", Status);
            bs->FreePool(arc_name);
            bs->CloseProtocol(fs_handle, &quibble_guid, image_handle, NULL);
            bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
            return;
        }

        arc_name[len] = 0;

        if (Status == EFI_SUCCESS) {
            print(L"ARC name is ");
            print_string(arc_name);
            print(L".\r\n");
        }

        len = 0;

        Status = quib->GetWindowsDriverName(quib, NULL, &len);

        if (Status == EFI_BUFFER_TOO_SMALL) {
            Status = bs->AllocatePool(EfiLoaderData, len, (void**)&fs_driver);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                bs->CloseProtocol(fs_handle, &quibble_guid, image_handle, NULL);
                bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
                return;
            }

            Status = quib->GetWindowsDriverName(quib, fs_driver, &len);
        }

        if (EFI_ERROR(Status) && Status != EFI_UNSUPPORTED) {
            print_error(L"GetWindowsDriverName", Status);
            bs->FreePool(arc_name);
            bs->FreePool(fs_driver);
            bs->CloseProtocol(fs_handle, &quibble_guid, image_handle, NULL);
            bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
            return;
        }

        bs->CloseProtocol(fs_handle, &quibble_guid, image_handle, NULL);
    }

    if (opt->options)
        parse_options(opt->options, &cmdline);

    if (cmdline.subvol != 0) {
        EFI_GUID open_subvol_guid = EFI_OPEN_SUBVOL_GUID;
        EFI_OPEN_SUBVOL_PROTOCOL* open_subvol;

        Status = bs->OpenProtocol(fs_handle, &open_subvol_guid, (void**)&open_subvol, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

        if (EFI_ERROR(Status)) {
            print(L"Could not open EFI_OPEN_SUBVOL_PROTOCOL on filesystem driver.\r\n");
            print_error(L"OpenProtocol", Status);
        } else {
            Status = open_subvol->OpenSubvol(open_subvol, cmdline.subvol, &root);
            if (EFI_ERROR(Status))
                print_error(L"OpenSubvol", Status);
        }
    }

    if (!root) {
        Status = fs->OpenVolume(fs, &root);
        if (EFI_ERROR(Status)) {
            print_error(L"OpenVolume", Status);
            bs->FreePool(arc_name);
            bs->CloseProtocol(fs_handle, &quibble_guid, image_handle, NULL);
            bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
            return;
        }
    }

    Status = boot(image_handle, bs, root, opt->options, path, arc_name, pe, reg, &cmdline, fs_driver);

    // shouldn't return

    print_error(L"boot", Status);

    bs->FreePool(arc_name);
    bs->CloseProtocol(fs_handle, &guid, image_handle, NULL);

    bs->WaitForEvent(1, &systable->ConIn->WaitForKey, &Event);
}

/* This is just used for setting the security cookie in the PE files - it doesn't
 * have to be cryptographically secure. */
static uint32_t get_random_seed() {
    EFI_STATUS Status;
    uint32_t seed;
    EFI_TIME tm;

    Status = systable->RuntimeServices->GetTime(&tm, NULL);
    if (EFI_ERROR(Status)) {
        print_error(L"GetTime", Status);
        return 0;
    }

    seed = (tm.Year << 16) | (tm.Month << 8) | tm.Day;
    seed ^= (tm.Hour << 16) | (tm.Minute << 8) | tm.Second;
    seed ^= tm.Nanosecond;

    return seed;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS Status;

    systable = SystemTable;
    image_handle = ImageHandle;

    Status = SystemTable->ConIn->Reset(systable->ConIn, false);
    if (EFI_ERROR(Status))
        return Status;

    Status = reg_register(systable->BootServices);
    if (EFI_ERROR(Status)) {
        print(L"Error registering registry protocol.\r\n");
        goto end2;
    }

    Status = pe_register(systable->BootServices, get_random_seed());
    if (EFI_ERROR(Status)) {
        print(L"Error registering PE loader protocol.\r\n");
        goto end;
    }

    Status = load_efi_drivers(systable->BootServices, ImageHandle);
    if (EFI_ERROR(Status)) {
        print_error(L"load_efi_drivers", Status);
        goto end;
    }

    Status = look_for_block_devices(systable->BootServices);
    if (EFI_ERROR(Status)) {
        print_error(L"look_for_block_devices", Status);
        goto end;
    }

    Status = change_stack(systable->BootServices, ImageHandle, stack_changed);
    if (EFI_ERROR(Status)) {
        print_error(L"change_stack", Status);
        goto end;
    }

    // FIXME - unload drivers

end:
    pe_unregister();

end2:
    reg_unregister();

    return Status;
}
