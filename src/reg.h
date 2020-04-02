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

#define WINDOWS_REGISTRY_PROTOCOL { 0x6C977486, 0xB9EB, 0x475C, {0xBC, 0xD4, 0x52, 0xD5, 0xDF, 0xB5, 0x63, 0x8F} }

#define REG_NONE                        0x00000000
#define REG_SZ                          0x00000001
#define REG_EXPAND_SZ                   0x00000002
#define REG_BINARY                      0x00000003
#define REG_DWORD                       0x00000004
#define REG_DWORD_BIG_ENDIAN            0x00000005
#define REG_LINK                        0x00000006
#define REG_MULTI_SZ                    0x00000007
#define REG_RESOURCE_LIST               0x00000008
#define REG_FULL_RESOURCE_DESCRIPTOR    0x00000009
#define REG_RESOURCE_REQUIREMENTS_LIST  0x0000000a
#define REG_QWORD                       0x0000000b

EFI_STATUS reg_register(EFI_BOOT_SERVICES* bs); // FIXME
EFI_STATUS reg_unregister(); // FIXME

typedef struct _EFI_REGISTRY_HIVE EFI_REGISTRY_HIVE;

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_OPEN_HIVE) (
    IN EFI_FILE_HANDLE File,
    OUT EFI_REGISTRY_HIVE** Hive
);

typedef struct {
    EFI_REGISTRY_OPEN_HIVE OpenHive;
} EFI_REGISTRY_PROTOCOL;

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_CLOSE) (
    IN EFI_REGISTRY_HIVE* This
);

typedef UINT32 HKEY;

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_FIND_ROOT) (
    IN EFI_REGISTRY_HIVE* This,
    OUT HKEY* Key
);

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_ENUM_KEYS) (
    IN EFI_REGISTRY_HIVE* This,
    IN HKEY Key,
    IN UINT32 Index,
    OUT WCHAR* Name,
    IN UINT32 NameLength
);

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_FIND_KEY) (
    IN EFI_REGISTRY_HIVE* This,
    IN HKEY Parent,
    IN const WCHAR* Path,
    OUT HKEY* Key
);

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_ENUM_VALUES) (
    IN EFI_REGISTRY_HIVE* This,
    IN HKEY Key,
    IN UINT32 Index,
    OUT WCHAR* Name,
    IN UINT32 NameLength,
    OUT UINT32* Type
);

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_QUERY_VALUE) (
    IN EFI_REGISTRY_HIVE* This,
    IN HKEY Key,
    IN const WCHAR* Name,
    OUT void* Data,
    IN OUT UINT32* DataLength,
    OUT UINT32* Type
);

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_QUERY_VALUE_NO_COPY) (
    IN EFI_REGISTRY_HIVE* This,
    IN HKEY Key,
    IN const WCHAR* Name,
    OUT void** Data,
    OUT UINT32* DataLength,
    OUT UINT32* Type
);

typedef EFI_STATUS (EFIAPI* EFI_REGISTRY_HIVE_STEAL_DATA) (
    IN EFI_REGISTRY_HIVE* This,
    OUT void** Data,
    OUT UINT32* Size
);

typedef struct _EFI_REGISTRY_HIVE {
    EFI_REGISTRY_HIVE_CLOSE Close;
    EFI_REGISTRY_HIVE_FIND_ROOT FindRoot;
    EFI_REGISTRY_HIVE_ENUM_KEYS EnumKeys;
    EFI_REGISTRY_HIVE_FIND_KEY FindKey;
    EFI_REGISTRY_HIVE_ENUM_VALUES EnumValues;
    EFI_REGISTRY_HIVE_QUERY_VALUE QueryValue;
    EFI_REGISTRY_HIVE_STEAL_DATA StealData;
    EFI_REGISTRY_HIVE_QUERY_VALUE_NO_COPY QueryValueNoCopy;
} EFI_REGISTRY_HIVE;
