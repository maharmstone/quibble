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

#define PE_LOADER_PROTOCOL { 0xBA5A36D4, 0xC83C, 0x4D81, {0xB1, 0x6E, 0xBF, 0x39, 0xF7, 0x40, 0xEA, 0x79 } }

#define IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY        0x0080

#define IMAGE_FILE_LARGE_ADDRESS_AWARE      0x0020

EFI_STATUS pe_register(EFI_BOOT_SERVICES* bs, uint32_t seed);
EFI_STATUS pe_unregister();

typedef struct _EFI_PE_IMAGE EFI_PE_IMAGE;

typedef EFI_STATUS (EFIAPI* EFI_PE_LOADER_LOAD) (
    IN EFI_FILE_HANDLE File,
    IN void* BaseAddress,
    OUT EFI_PE_IMAGE** Image
);

typedef struct _EFI_PE_LOADER_PROTOCOL {
    EFI_PE_LOADER_LOAD Load;
} EFI_PE_LOADER_PROTOCOL;

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_FREE) (
    IN EFI_PE_IMAGE* This
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_GET_ENTRY_POINT) (
    IN EFI_PE_IMAGE* This,
    OUT void** EntryPoint
);

typedef struct _EFI_IMPORT_LIST {
    UINT32 NumberOfImports;
    UINT32 Imports[0];
} EFI_IMPORT_LIST;

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_LIST_IMPORTS) (
    IN EFI_PE_IMAGE* This,
    OUT EFI_IMPORT_LIST* ImportList,
    IN OUT UINTN* BufferSize
);

typedef EFI_PHYSICAL_ADDRESS (EFIAPI* EFI_PE_IMAGE_GET_ADDRESS) (
    IN EFI_PE_IMAGE* This
);

typedef UINT32 (EFIAPI* EFI_PE_IMAGE_GET_SIZE) (
    IN EFI_PE_IMAGE* This
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_RESOLVE_FORWARD) (
    IN char* Name,
    OUT UINT64* Address
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_RESOLVE_IMPORTS) (
    IN EFI_PE_IMAGE* This,
    IN char* LibraryName,
    IN EFI_PE_IMAGE* Library,
    IN EFI_PE_IMAGE_RESOLVE_FORWARD ResolveForward
);

typedef UINT32 (EFIAPI* EFI_PE_IMAGE_GET_CHECKSUM) (
    IN EFI_PE_IMAGE* This
);

typedef UINT16 (EFIAPI* EFI_PE_IMAGE_GET_DLL_CHARACTERISTICS) (
    IN EFI_PE_IMAGE* This
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_MOVE_ADDRESS) (
    IN EFI_PE_IMAGE* This,
    IN EFI_PHYSICAL_ADDRESS NewAddress
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_GET_VERSION) (
    IN EFI_PE_IMAGE* This,
    OUT UINT32* VersionMS,
    OUT UINT32* VersionLS
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_FIND_EXPORT) (
    IN EFI_PE_IMAGE* This,
    IN char* Function,
    OUT UINT64* Address,
    IN EFI_PE_IMAGE_RESOLVE_FORWARD ResolveForward
);

typedef UINT32 (EFIAPI* EFI_PE_IMAGE_GET_CHARACTERISTICS) (
    IN EFI_PE_IMAGE* This
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_SET_NO_RELOC) (
    IN EFI_PE_IMAGE* This
);

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_RELOCATE) (
    IN EFI_PE_IMAGE* This,
    IN EFI_VIRTUAL_ADDRESS Address
);

#pragma pack(push,1)

typedef struct {
    char Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} IMAGE_SECTION_HEADER;

#pragma pack(pop)

#define IMAGE_SCN_MEM_DISCARDABLE   0x02000000
#define IMAGE_SCN_MEM_NOT_CACHED    0x04000000
#define IMAGE_SCN_MEM_NOT_PAGED     0x08000000
#define IMAGE_SCN_MEM_SHARED        0x10000000
#define IMAGE_SCN_MEM_EXECUTE       0x20000000
#define IMAGE_SCN_MEM_READ          0x40000000
#define IMAGE_SCN_MEM_WRITE         0x80000000

typedef EFI_STATUS (EFIAPI* EFI_PE_IMAGE_GET_SECTIONS) (
    IN EFI_PE_IMAGE* This,
    OUT IMAGE_SECTION_HEADER** Sections,
    OUT UINTN* NumberOfSections
);

typedef struct _EFI_PE_IMAGE {
    void* Data;
    EFI_PE_IMAGE_FREE Free;
    EFI_PE_IMAGE_GET_ENTRY_POINT GetEntryPoint;
    EFI_PE_IMAGE_LIST_IMPORTS ListImports;
    EFI_PE_IMAGE_GET_ADDRESS GetAddress;
    EFI_PE_IMAGE_GET_SIZE GetSize;
    EFI_PE_IMAGE_RESOLVE_IMPORTS ResolveImports;
    EFI_PE_IMAGE_GET_CHECKSUM GetCheckSum;
    EFI_PE_IMAGE_GET_DLL_CHARACTERISTICS GetDllCharacteristics;
    EFI_PE_IMAGE_MOVE_ADDRESS MoveAddress;
    EFI_PE_IMAGE_GET_VERSION GetVersion;
    EFI_PE_IMAGE_FIND_EXPORT FindExport;
    EFI_PE_IMAGE_GET_CHARACTERISTICS GetCharacteristics;
    EFI_PE_IMAGE_GET_SECTIONS GetSections;
    EFI_PE_IMAGE_RELOCATE Relocate;
} EFI_PE_IMAGE;
