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

#define EFI_QUIBBLE_PROTOCOL_GUID { 0x98BCC8FF, 0xD212, 0x4B09, {0x84, 0x0C, 0x43, 0x19, 0xAD, 0x2E, 0xD3, 0x6A } }

typedef struct _EFI_QUIBBLE_PROTOCOL EFI_QUIBBLE_PROTOCOL;

typedef EFI_STATUS (EFIAPI* EFI_QUIBBLE_GET_ARC_NAME) (
    IN EFI_QUIBBLE_PROTOCOL* This,
    OUT char* ArcName,
    IN OUT UINTN* ArcNameLen
);

typedef EFI_STATUS (EFIAPI* EFI_QUIBBLE_GET_WINDOWS_DRIVER_NAME) (
    IN EFI_QUIBBLE_PROTOCOL* This,
    OUT CHAR16* DriverName,
    IN OUT UINTN* DriverNameLen
);

typedef struct _EFI_QUIBBLE_PROTOCOL {
    EFI_QUIBBLE_GET_ARC_NAME GetArcName;
    EFI_QUIBBLE_GET_WINDOWS_DRIVER_NAME GetWindowsDriverName;
} EFI_QUIBBLE_PROTOCOL;

#define EFI_OPEN_SUBVOL_GUID { 0x5861E4D5, 0xC7F1, 0x4932, {0xA0, 0x81, 0xF2, 0x2A, 0xAE, 0x8A, 0x82, 0x98 } }

typedef struct _EFI_OPEN_SUBVOL_PROTOCOL EFI_OPEN_SUBVOL_PROTOCOL;

typedef EFI_STATUS (EFIAPI* EFI_OPEN_SUBVOL_FUNC) (
    IN EFI_OPEN_SUBVOL_PROTOCOL* This,
    IN UINT64 Subvol,
    OUT EFI_FILE_HANDLE* File
);

typedef struct _EFI_OPEN_SUBVOL_PROTOCOL {
    EFI_OPEN_SUBVOL_FUNC OpenSubvol;
} EFI_OPEN_SUBVOL_PROTOCOL;

#define EFI_QUIBBLE_INFO_PROTOCOL_GUID { 0x89498E00, 0xAE8F, 0x4B23, {0x86, 0x11, 0x71, 0x2A, 0xE1, 0x2F, 0xC8, 0xD9 } }

typedef void (EFIAPI* EFI_QUIBBLE_INFO_PRINT) (
    IN const char* s
);

typedef struct _EFI_QUIBBLE_INFO_PROTOCOL {
    EFI_QUIBBLE_INFO_PRINT Print;
} EFI_QUIBBLE_INFO_PROTOCOL;
