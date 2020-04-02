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

#include <stdint.h>

#define HV_HBLOCK_SIGNATURE 0x66676572  // "regf"

#define HSYS_MAJOR 1
#define HSYS_MINOR 3
#define HFILE_TYPE_PRIMARY 0
#define HBASE_FORMAT_MEMORY 1

#define CM_KEY_HASH_LEAF        0x686c  // "lh"
#define CM_KEY_INDEX_ROOT       0x6972  // "ri"
#define CM_KEY_NODE_SIGNATURE   0x6b6e  // "nk"
#define CM_KEY_VALUE_SIGNATURE  0x6b76  // "vk"

#define KEY_IS_VOLATILE                 0x0001
#define KEY_HIVE_EXIT                   0x0002
#define KEY_HIVE_ENTRY                  0x0004
#define KEY_NO_DELETE                   0x0008
#define KEY_SYM_LINK                    0x0010
#define KEY_COMP_NAME                   0x0020
#define KEY_PREDEF_HANDLE               0x0040
#define KEY_VIRT_MIRRORED               0x0080
#define KEY_VIRT_TARGET                 0x0100
#define KEY_VIRTUAL_STORE               0x0200

#define VALUE_COMP_NAME                 0x0001

// stupid name... this means "small enough not to warrant its own cell"
#define CM_KEY_VALUE_SPECIAL_SIZE       0x80000000

#define HIVE_FILENAME_MAXLEN 31

#pragma pack(push,1)

typedef struct {
    uint32_t Signature;
    uint32_t Sequence1;
    uint32_t Sequence2;
    uint64_t TimeStamp;
    uint32_t Major;
    uint32_t Minor;
    uint32_t Type;
    uint32_t Format;
    uint32_t RootCell;
    uint32_t Length;
    uint32_t Cluster;
    WCHAR FileName[HIVE_FILENAME_MAXLEN + 1];
    uint32_t Reserved1[99];
    uint32_t CheckSum;
    uint32_t Reserved2[0x37E];
    uint32_t BootType;
    uint32_t BootRecover;
} HBASE_BLOCK;

typedef struct {
    uint16_t Signature;
    uint16_t Flags;
    uint64_t LastWriteTime;
    uint32_t Spare;
    uint32_t Parent;
    uint32_t SubKeyCount;
    uint32_t VolatileSubKeyCount;
    uint32_t SubKeyList;
    uint32_t VolatileSubKeyList;
    uint32_t ValuesCount;
    uint32_t Values;
    uint32_t Security;
    uint32_t Class;
    uint32_t MaxNameLen;
    uint32_t MaxClassLen;
    uint32_t MaxValueNameLen;
    uint32_t MaxValueDataLen;
    uint32_t WorkVar;
    uint16_t NameLength;
    uint16_t ClassLength;
    WCHAR Name[1];
} CM_KEY_NODE;

typedef struct {
    uint32_t Cell;
    uint32_t HashKey;
} CM_INDEX;

typedef struct {
    uint16_t Signature;
    uint16_t Count;
    CM_INDEX List[1];
} CM_KEY_FAST_INDEX;

typedef struct {
    uint16_t Signature;
    uint16_t NameLength;
    uint32_t DataLength;
    uint32_t Data;
    uint32_t Type;
    uint16_t Flags;
    uint16_t Spare;
    WCHAR Name[1];
} CM_KEY_VALUE;

typedef struct {
    uint16_t Signature;
    uint16_t Count;
    uint32_t List[1];
} CM_KEY_INDEX;

#pragma pack(pop)
