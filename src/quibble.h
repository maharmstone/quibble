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

#pragma once

#include <efibind.h>
#include <efidef.h>
#include <efidevp.h>
#include <efiprot.h>
#include <eficon.h>
#include <efiapi.h>
#include <efilink.h>
#include <efipart.h>
#include <stdbool.h>
#include "win.h"

// FIXME - only in debug mode
#ifndef _MSC_VER
#define halt() { \
    unsigned int __volatile wait = 1; \
    while (wait) { __asm __volatile("pause"); } \
}
#else
#define halt() { \
    volatile unsigned int wait = 1; \
    while (wait) { } \
}
#endif

#define UNUSED(x) (void)(x)

#define STACK_SIZE 8 // pages
#define KERNEL_STACK_SIZE 8 // pages

typedef struct {
    LIST_ENTRY list_entry;
    void* va;
    void* pa;
    unsigned int pages;
    TYPE_OF_MEMORY type;
} mapping;

typedef struct _EFI_PE_IMAGE EFI_PE_IMAGE;
typedef struct _EFI_IMPORT_LIST EFI_IMPORT_LIST;
typedef struct _EFI_PE_LOADER_PROTOCOL EFI_PE_LOADER_PROTOCOL;

#define MAX_PATH 260

typedef struct {
    WCHAR name[MAX_PATH];
    WCHAR dir[MAX_PATH];
    EFI_PE_IMAGE* img;
    void* va;
    EFI_IMPORT_LIST* import_list;
    TYPE_OF_MEMORY memory_type;
    bool dll;
    BOOT_DRIVER_LIST_ENTRY* bdle;
    unsigned int order;
    bool no_reloc;
    LIST_ENTRY list_entry;
} image;

typedef struct {
    char* name;
    WCHAR* namew;
    char* system_path;
    char* options;
} boot_option;

typedef struct {
    LIST_ENTRY list_entry;
    unsigned int disk_num;
    unsigned int part_num;
    EFI_DEVICE_PATH_PROTOCOL* device_path;
    ARC_DISK_SIGNATURE arc;
} block_device;

typedef struct _command_line command_line;

// boot.c
extern void* stack;
extern EFI_HANDLE image_handle;
extern uint64_t cpu_frequency;
EFI_STATUS add_image(EFI_BOOT_SERVICES* bs, LIST_ENTRY* images, const WCHAR* name, TYPE_OF_MEMORY memory_type,
                     const WCHAR* dir, bool dll, BOOT_DRIVER_LIST_ENTRY* bdle, unsigned int order,
                     bool no_reloc);
EFI_STATUS load_image(image* img, WCHAR* name, EFI_PE_LOADER_PROTOCOL* pe, void* va, EFI_FILE_HANDLE dir,
                      command_line* cmdline, uint16_t build);
EFI_STATUS open_file(EFI_FILE_HANDLE dir, EFI_FILE_HANDLE* h, const WCHAR* name);
EFI_STATUS read_file(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE dir, const WCHAR* name, void** data, size_t* size);
EFI_STATUS open_parent_dir(EFI_FILE_IO_INTERFACE* fs, FILEPATH_DEVICE_PATH* dp, EFI_FILE_HANDLE* dir);

// mem.c
#ifdef _X86_
extern bool pae;
#endif
extern EFI_MEMORY_DESCRIPTOR* efi_runtime_map;
extern UINTN efi_runtime_map_size, map_desc_size;

void* find_virtual_address(void* pa, LIST_ENTRY* mappings);
void* fix_address_mapping(void* addr, void* pa, void* va);
EFI_STATUS add_mapping(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void* va, void* pa, unsigned int pages,
                       TYPE_OF_MEMORY type);
EFI_STATUS enable_paging(EFI_HANDLE image_handle, EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings,
                         LOADER_BLOCK1A* block1, void* va, uintptr_t* loader_pages_spanned);
EFI_STATUS process_memory_map(EFI_BOOT_SERVICES* bs, void** va, LIST_ENTRY* mappings);
EFI_STATUS map_efi_runtime(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void** va, uint16_t version);

// hw.c
extern LIST_ENTRY block_devices;
EFI_STATUS find_hardware(EFI_BOOT_SERVICES* bs, LOADER_BLOCK1C* block1, void** va, LIST_ENTRY* mappings,
                         EFI_HANDLE image_handle, uint16_t version);
EFI_STATUS find_disks(EFI_BOOT_SERVICES* bs, LIST_ENTRY* disk_sig_list, void** va, LIST_ENTRY* mappings,
                      CONFIGURATION_COMPONENT_DATA* system_key, bool new_disk_format);
EFI_STATUS look_for_block_devices(EFI_BOOT_SERVICES* bs);
EFI_STATUS kdnet_init(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE dir, EFI_FILE_HANDLE* file, DEBUG_DEVICE_DESCRIPTOR* ddd);

// apiset.c
extern void* apisetva;
extern unsigned int apisetsize;
EFI_STATUS load_api_set(EFI_BOOT_SERVICES* bs, LIST_ENTRY* images, EFI_PE_LOADER_PROTOCOL* pe, EFI_FILE_HANDLE dir,
                        void** va, uint16_t version, LIST_ENTRY* mappings, command_line* cmdline);
bool search_api_set(WCHAR* dll, WCHAR* newname, uint16_t version);

// menu.c
EFI_STATUS show_menu(EFI_SYSTEM_TABLE* systable, boot_option** opt);

// debug.c
extern void* kdnet_scratch;
EFI_STATUS find_kd_export(EFI_PE_IMAGE* kdstub, uint16_t build);
EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build);
EFI_STATUS allocate_kdnet_hw_context(EFI_PE_IMAGE* kdstub, DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build);

// CSM (not in gnu-efi)

#define EFI_LEGACY_BIOS_PROTOCOL_GUID { 0xdb9a1e3d, 0x45cb, 0x4abb, {0x85, 0x3b, 0xe5, 0x38, 0x7f, 0xdb, 0x2e, 0x2d } }

typedef struct _EFI_LEGACY_BIOS_PROTOCOL EFI_LEGACY_BIOS_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_LEGACY_BIOS_SHADOW_ALL_LEGACY_OPROMS)(IN EFI_LEGACY_BIOS_PROTOCOL *This);

struct _EFI_LEGACY_BIOS_PROTOCOL {
    void* Int86;
    void* FarCall86;
    void* CheckPciRom;
    void* InstallPciRom;
    void* LegacyBoot;
    void* UpdateKeyboardLedStatus;
    void* GetBbsInfo;
    EFI_LEGACY_BIOS_SHADOW_ALL_LEGACY_OPROMS ShadowAllLegacyOproms;
    void* PrepareToBootEfi;
    void* GetLegacyRegion;
    void* CopyLegacyRegion;
    void* BootUnconventionalDevice;
};
