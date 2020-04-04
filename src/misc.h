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
#include <efierr.h>
#include <stddef.h>

extern EFI_SYSTEM_TABLE* systable;

#define print(s) systable->ConOut->OutputString(systable->ConOut, (s))

void print_error(const WCHAR* func, EFI_STATUS Status);
void print_hex(uint64_t v);
void print_dec(uint32_t v);
void print_string(const char* s);
void itow(int v, WCHAR* w);
#ifdef _MSC_VER
int wcsicmp(const WCHAR* s1, const WCHAR* s2);
int stricmp(const char* s1, const char* s2);
int strnicmp(const char* s1, const char* s2, int n);
#endif
EFI_STATUS utf8_to_utf16(WCHAR* dest, unsigned int dest_max, unsigned int* dest_len, const char* src, unsigned int src_len);
EFI_STATUS utf16_to_utf8(char* dest, unsigned int dest_max, unsigned int* dest_len, const WCHAR* src, unsigned int src_len);
