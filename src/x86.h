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

#define CR0_MP      0x00000002
#define CR0_WP      0x00010000
#define CR0_AM      0x00040000
#define CR0_PG      0x80000000

#define CR4_PAE     0x00000020
#define CR4_PGE     0x00000080

typedef struct {
    uint32_t Valid:1;
    uint32_t Write:1;
    uint32_t Owner:1;
    uint32_t WriteThrough:1;
    uint32_t CacheDisable:1;
    uint32_t Accessed:1;
    uint32_t Dirty:1;
    uint32_t LargePage:1;
    uint32_t Global:1;
    uint32_t CopyOnWrite:1;
    uint32_t Prototype:1;
    uint32_t Reserved:1;
    uint32_t PageFrameNumber:20;
} HARDWARE_PTE;

typedef struct {
    uint64_t Valid:1;
    uint64_t Write:1;
    uint64_t Owner:1;
    uint64_t WriteThrough:1;
    uint64_t CacheDisable:1;
    uint64_t Accessed:1;
    uint64_t Dirty:1;
    uint64_t LargePage:1;
    uint64_t Global:1;
    uint64_t CopyOnWrite:1;
    uint64_t Prototype:1;
    uint64_t reserved0:1;
    uint64_t PageFrameNumber:28;
    uint64_t reserved1:12;
    uint64_t SoftwareWsIndex:11;
    uint64_t NoExecute:1;
} HARDWARE_PTE_PAE;

typedef struct {
    uint16_t LimitLow;
    uint16_t BaseLow;
    uint32_t BaseMid:8;
    uint32_t Type:5;
    uint32_t Dpl:2;
    uint32_t Pres:1;
    uint32_t LimitHi:4;
    uint32_t Sys:1;
    uint32_t Long:1;
    uint32_t Default_Big:1;
    uint32_t Granularity:1;
    uint32_t BaseHi:8;
} gdt_entry;

#ifdef __x86_64__
typedef struct {
    uint16_t offset_1;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_2;
    uint32_t offset_3;
    uint32_t zero;
} idt_entry;
#else
typedef struct {
    uint16_t Offset;
    uint16_t Selector;
    uint16_t Access;
    uint16_t ExtendedOffset;
} idt_entry;
#endif

#define DESCRIPTOR_ACCESSED     0x1
#define DESCRIPTOR_READ_WRITE   0x2
#define DESCRIPTOR_EXECUTE_READ 0x2
#define DESCRIPTOR_EXPAND_DOWN  0x4
#define DESCRIPTOR_CONFORMING   0x4
#define DESCRIPTOR_CODE         0x8

#define TYPE_CODE   (0x10 | DESCRIPTOR_CODE | DESCRIPTOR_EXECUTE_READ)
#define TYPE_DATA   (0x10 | DESCRIPTOR_READ_WRITE)
#define TYPE_TSS32A 0x09

#define PAGE_COUNT(s) ((s + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)
#define GDT_PAGES PAGE_COUNT(NUM_GDT*sizeof(gdt_entry))
#define IDT_PAGES PAGE_COUNT(NUM_IDT*sizeof(idt_entry))

#define IOPM_COUNT              1
#define IOPM_DIRECTION_MAP_SIZE 32
#define IOPM_FULL_SIZE          8196

typedef struct {
    uint8_t DirectionMap[IOPM_DIRECTION_MAP_SIZE];
    uint8_t IoMap[IOPM_FULL_SIZE];
} KIIO_ACCESS_MAP;

#pragma pack(push,1)

#ifdef __x86_64__
typedef struct {
    uint32_t Reserved0;
    uint64_t Rsp0;
    uint64_t Rsp1;
    uint64_t Rsp2;
    uint64_t Ist[8];
    uint64_t Reserved1;
    uint16_t Reserved2;
    uint16_t IoMapBase;
} KTSS;
#else
typedef struct {
    uint16_t Backlink;
    uint16_t Reserved0;
    uint32_t Esp0;
    uint16_t Ss0;
    uint16_t Reserved1;
    uint32_t NotUsed1[4];
    uint32_t CR3;
    uint32_t Eip;
    uint32_t EFlags;
    uint32_t Eax;
    uint32_t Ecx;
    uint32_t Edx;
    uint32_t Ebx;
    uint32_t Esp;
    uint32_t Ebp;
    uint32_t Esi;
    uint32_t Edi;
    uint16_t Es;
    uint16_t Reserved2;
    uint16_t Cs;
    uint16_t Reserved3;
    uint16_t Ss;
    uint16_t Reserved4;
    uint16_t Ds;
    uint16_t Reserved5;
    uint16_t Fs;
    uint16_t Reserved6;
    uint16_t Gs;
    uint16_t Reserved7;
    uint16_t LDT;
    uint16_t Reserved8;
    uint16_t Flags;
    uint16_t IoMapBase;
    KIIO_ACCESS_MAP IoMaps[IOPM_COUNT];
    uint8_t IntDirectionMap[IOPM_DIRECTION_MAP_SIZE];
} KTSS;
#endif

#ifdef __x86_64__
typedef struct {
    uint16_t Limit;
    uint64_t Base;
} GDTIDT;
#else
typedef struct {
    uint16_t Limit;
    uint32_t Base;
} GDTIDT;
#endif

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id [6];
    uint8_t revision;
    uint32_t rsdt_physical_address;
    uint32_t length;
    uint64_t xsdt_physical_address;
    uint8_t extended_checksum;
    char reserved[3];
} RSDP_DESCRIPTOR;

typedef struct {
    uint16_t address;
    uint16_t segment;
} ivt_entry;

#pragma pack(pop)
