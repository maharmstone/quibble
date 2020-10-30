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
#include <intrin.h>
#include "quibble.h"
#include "misc.h"
#include "x86.h"

#ifdef _X86_
bool pae = true;
HARDWARE_PTE* page_directory;
HARDWARE_PTE_PAE* pdpt;
#elif defined(__x86_64__)
HARDWARE_PTE_PAE* pml4;
#endif

#ifdef __x86_64__
#define HAL_MEMORY 0xffffffffffc00000
#endif

EFI_MEMORY_DESCRIPTOR* efi_memory_map;
EFI_MEMORY_DESCRIPTOR* efi_runtime_map;
UINTN efi_map_size, efi_runtime_map_size, map_desc_size;
extern void* apic;

static TYPE_OF_MEMORY map_memory_type(UINTN memory_type) {
    switch (memory_type) {
        case EfiReservedMemoryType:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
        case EfiPalCode:
            return LoaderSpecialMemory;

        case EfiUnusableMemory:
            return LoaderBad;

        default:
            return LoaderFree;
    }
}

void* fix_address_mapping(void* addr, void* pa, void* va) {
    return (uint8_t*)addr - (uint8_t*)pa + (uint8_t*)va;
}

void* find_virtual_address(void* pa, LIST_ENTRY* mappings) {
    LIST_ENTRY* le;

    le = mappings->Flink;
    while (le != mappings) {
        mapping* m = _CR(le, mapping, list_entry);

        if (m->va) {
            if ((uint8_t*)pa >= (uint8_t*)m->pa && (uint8_t*)pa < (uint8_t*)m->pa + (m->pages * EFI_PAGE_SIZE))
                return (uint8_t*)pa - (uint8_t*)m->pa + (uint8_t*)m->va;
        }

        le = le->Flink;
    }

    print(L"Could not find virtual address for physical address ");
    print_hex((uintptr_t)pa);
    print(L".\r\n");

    return NULL;
}

static EFI_STATUS map_memory(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, uintptr_t va, uintptr_t pa, unsigned int pages) {
    uintptr_t pfn = pa >> EFI_PAGE_SHIFT;

#ifdef _X86_
    UNUSED(mappings);

    if (pae) {
        do {
            HARDWARE_PTE_PAE* dir = (HARDWARE_PTE_PAE*)(pdpt[va >> 30].PageFrameNumber * EFI_PAGE_SIZE);
            unsigned int index = (va >> 21) & 0x1ff;
            unsigned int index2 = (va & 0x1ff000) >> 12;
            HARDWARE_PTE_PAE* page_table;

            if (!dir[index].Valid) { // allocate new page table
                EFI_STATUS Status;
                EFI_PHYSICAL_ADDRESS addr;

                Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePages", Status);
                    return Status;
                }

                memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

                dir[index].PageFrameNumber = addr / EFI_PAGE_SIZE;
                dir[index].Valid = 1;
                dir[index].Write = 1;

                page_table = (HARDWARE_PTE_PAE*)(uintptr_t)addr;
            } else
                page_table = (HARDWARE_PTE_PAE*)(dir[index].PageFrameNumber * EFI_PAGE_SIZE);

            page_table[index2].PageFrameNumber = pfn;
            page_table[index2].Valid = 1;
            page_table[index2].Write = 1;

            va += EFI_PAGE_SIZE;
            pfn++;
            pages--;
        } while (pages > 0);
    } else {
        do {
            unsigned int index = va >> 22;
            unsigned int index2 = (va & 0x3ff000) >> 12;
            HARDWARE_PTE* page_table;

            if (!page_directory[index].Valid) { // allocate new page table
                EFI_STATUS Status;
                EFI_PHYSICAL_ADDRESS addr;

                Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePages", Status);
                    return Status;
                }

                memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

                page_directory[index].PageFrameNumber = addr / EFI_PAGE_SIZE;
                page_directory[index].Valid = 1;
                page_directory[index].Write = 1;

                page_table = (HARDWARE_PTE*)(uintptr_t)addr;
            } else
                page_table = (HARDWARE_PTE*)(page_directory[index].PageFrameNumber * EFI_PAGE_SIZE);

            page_table[index2].PageFrameNumber = pfn;
            page_table[index2].Valid = 1;
            page_table[index2].Write = 1;

            va += EFI_PAGE_SIZE;
            pfn++;
            pages--;
        } while (pages > 0);
    }
#elif defined(__x86_64__)
    do {
        HARDWARE_PTE_PAE* pdpt;
        HARDWARE_PTE_PAE* pd;
        HARDWARE_PTE_PAE* pt;
        unsigned int index = (va & 0xff8000000000) >> 39;
        unsigned int index2 = (va & 0x7fc0000000) >> 30;
        unsigned int index3 = (va & 0x3fe00000) >> 21;
        unsigned int index4 = (va & 0x1ff000) >> 12;

        if (!pml4[index].Valid) {
            EFI_STATUS Status;
            EFI_PHYSICAL_ADDRESS addr;

            Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePages", Status);
                return Status;
            }

            Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)addr, 1, LoaderMemoryData);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

            pml4[index].PageFrameNumber = addr / EFI_PAGE_SIZE;
            pml4[index].Valid = 1;
            pml4[index].Write = 1;

            pdpt = (HARDWARE_PTE_PAE*)(uintptr_t)addr;
        } else {
            uint64_t ptr;

            /* Somewhat surprising behaviour from gcc here - combining the following two lines into one
             * results in the value being sign-extended. Compiler bug? */

            ptr = pml4[index].PageFrameNumber;
            ptr <<= EFI_PAGE_SHIFT;

            pdpt = (HARDWARE_PTE_PAE*)(uintptr_t)ptr;
        }

        if (!pdpt[index2].Valid) {
            EFI_STATUS Status;
            EFI_PHYSICAL_ADDRESS addr;

            Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePages", Status);
                return Status;
            }

            Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)addr, 1, LoaderMemoryData);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

            pdpt[index2].PageFrameNumber = addr / EFI_PAGE_SIZE;
            pdpt[index2].Valid = 1;
            pdpt[index2].Write = 1;

            pd = (HARDWARE_PTE_PAE*)(uintptr_t)addr;
        } else {
            uint64_t ptr;

            ptr = pdpt[index2].PageFrameNumber;
            ptr <<= EFI_PAGE_SHIFT;

            pd = (HARDWARE_PTE_PAE*)(uintptr_t)ptr;
        }

        if (!pd[index3].Valid) {
            EFI_STATUS Status;
            EFI_PHYSICAL_ADDRESS addr;

            Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePages", Status);
                return Status;
            }

            Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)addr, 1, LoaderMemoryData);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

            pd[index3].PageFrameNumber = addr / EFI_PAGE_SIZE;
            pd[index3].Valid = 1;
            pd[index3].Write = 1;

            pt = (HARDWARE_PTE_PAE*)(uintptr_t)addr;
        } else {
            uint64_t ptr;

            ptr = pd[index3].PageFrameNumber;
            ptr <<= EFI_PAGE_SHIFT;

            pt = (HARDWARE_PTE_PAE*)(uintptr_t)ptr;
        }

        pt[index4].PageFrameNumber = pfn;
        pt[index4].Valid = 1;
        pt[index4].Write = 1;

        va += EFI_PAGE_SIZE;
        pfn++;
        pages--;
    } while (pages > 0);
#endif

    return EFI_SUCCESS;
}

#if 0
static void check_mappings(LIST_ENTRY* mappings) {
    LIST_ENTRY *le, *prevle;

    prevle = mappings;
    le = mappings->Flink;
    while (le != mappings) {
        mapping* m = _CR(le, mapping, list_entry);
        mapping* m2 = _CR(le->Flink, mapping, list_entry);

        if (le->Flink == mappings)
            return;

        if (m2->pa < m->pa || (uint8_t*)m->pa + (m->pages * EFI_PAGE_SIZE) > m2->pa || le->Blink != prevle) {
            int i = 0;

            print(L"ERROR\r\n");

            le = mappings->Flink;
            while (le != mappings) {
                mapping* m = _CR(le, mapping, list_entry);

                print_hex((uintptr_t)m->pa);
                print(L", ");
                print_hex(m->pages);
                print(L", ");
                print_hex(m->type);

                if (i % 3 == 2)
                    print(L"\r\n");
                else
                    print(L"\t");

                if (le == NULL)
                    break;

                le = le->Flink;
                i++;
            }

            halt();
        }

        prevle = le;
        le = le->Flink;
    }
}
#endif

EFI_STATUS add_mapping(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void* va, void* pa, unsigned int pages,
                       TYPE_OF_MEMORY type) {
    EFI_STATUS Status;
    mapping* m;
    LIST_ENTRY* le;
    void* pa_end = (uint8_t*)pa + (pages * EFI_PAGE_SIZE) - 1;

    Status = bs->AllocatePool(EfiLoaderData, sizeof(mapping), (void**)&m);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    m->va = va;
    m->pa = pa;
    m->pages = pages;
    m->type = type;

    le = mappings->Flink;
    while (le != mappings) {
        mapping* m2 = _CR(le, mapping, list_entry);
        void* pa2_end = (uint8_t*)m2->pa + (m2->pages * EFI_PAGE_SIZE) - 1;

        if (pa_end > m2->pa && pa_end <= pa2_end) { // split off beginning of block
            mapping* m3;
            size_t pages2;

            if (m2->type != LoaderFree) {
                print(L"error - cutting into non-free mapping\r\n");
                halt();
                return EFI_INVALID_PARAMETER;
            }

            pages2 = ((uint8_t*)pa2_end - (uint8_t*)pa_end) / EFI_PAGE_SIZE;

            if (pages2 > 0) {
                Status = bs->AllocatePool(EfiLoaderData, sizeof(mapping), (void**)&m3);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePool", Status);
                    return Status;
                }

                m3->va = NULL;
                m3->pa = (uint8_t*)pa_end + 1;
                m3->pages = pages2;
                m3->type = m2->type;

                InsertHeadList(&m2->list_entry, &m3->list_entry);
            }

            m2->pages = ((uint8_t*)pa_end + 1 - (uint8_t*)m2->pa) / EFI_PAGE_SIZE;

            pa2_end = (uint8_t*)m2->pa + (m2->pages * EFI_PAGE_SIZE) - 1;
        }

        if (m->pa > m2->pa && m->pa < pa2_end) { // split off end of block
            mapping* m3;
            size_t pages2;

            if (m2->type != LoaderFree) {
                print(L"error - cutting into non-free mapping\r\n");
                halt();
                return EFI_INVALID_PARAMETER;
            }

            pages2 = ((uint8_t*)pa2_end + 1 - (uint8_t*)m->pa) / EFI_PAGE_SIZE;

            if (pages2 > 0) {
                Status = bs->AllocatePool(EfiLoaderData, sizeof(mapping), (void**)&m3);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePool", Status);
                    return Status;
                }

                m3->va = NULL;
                m3->pa = m->pa;
                m3->pages = pages2;
                m3->type = m2->type;

                InsertHeadList(&m2->list_entry, &m3->list_entry);
            }

            m2->pages = ((uint8_t*)m->pa - (uint8_t*)m2->pa) / EFI_PAGE_SIZE;

            pa2_end = (uint8_t*)m2->pa + (m2->pages * EFI_PAGE_SIZE) - 1;
        }

        if ((m2->pa >= m->pa && pa2_end <= pa_end) || m2->pages == 0) { // remove block entirely
            LIST_ENTRY* le2 = le->Flink;

            if (m2->type != LoaderFree) {
                print(L"error - cutting into non-free mapping\r\n");
                halt();
                return EFI_INVALID_PARAMETER;
            }

            RemoveEntryList(&m2->list_entry);
            bs->FreePool(m2);

            le = le2;
            continue;
        }

        if (m2->pa > m->pa) {
            InsertHeadList(m2->list_entry.Blink, &m->list_entry);

            return EFI_SUCCESS;
        }

        le = le->Flink;
    }

    InsertTailList(mappings, &m->list_entry);

    return EFI_SUCCESS;
}

EFI_STATUS process_memory_map(EFI_BOOT_SERVICES* bs, void** va, LIST_ENTRY* mappings) {
    EFI_STATUS Status;
    UINTN key, count;
    UINT32 version;
    EFI_MEMORY_DESCRIPTOR* desc = NULL;
    uint8_t* va2 = *va;
    bool map_video_ram = true;

    efi_map_size = 0;

    do {
        EFI_STATUS Status2;

        Status = bs->GetMemoryMap(&efi_map_size, desc, &key, &map_desc_size, &version);

        if (!EFI_ERROR(Status))
            break;
        else if (Status != EFI_BUFFER_TOO_SMALL) {
            print_error(L"GetMemoryMap", Status);

            if (desc)
                bs->FreePool(desc);

            return Status;
        }

        if (desc)
            bs->FreePool(desc);

        Status2 = bs->AllocatePool(EfiLoaderData, efi_map_size, (void**)&desc);
        if (EFI_ERROR(Status2)) {
            print_error(L"AllocatePool", Status2);
            return Status2;
        }
    } while (Status == EFI_BUFFER_TOO_SMALL);

    if (!desc)
        return EFI_INVALID_PARAMETER;

    count = efi_map_size / map_desc_size;
    efi_memory_map = desc;

    for (unsigned int i = 0; i < count; i++) {
        TYPE_OF_MEMORY memory_type = map_memory_type(desc->Type);

        if (memory_type != LoaderFree) {
            Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)desc->PhysicalStart,
                                 desc->NumberOfPages, memory_type);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            va2 += desc->NumberOfPages * EFI_PAGE_SIZE;

            if (desc->PhysicalStart <= 0xa0000 && desc->PhysicalStart + (desc->NumberOfPages << EFI_PAGE_SHIFT) > 0xa0000)
                map_video_ram = false;
        } else {
            Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)desc->PhysicalStart,
                                 desc->NumberOfPages, LoaderFree);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }
        }

        // do identity map where we need to
        if (desc->Type == EfiLoaderCode) {
            Status = add_mapping(bs, mappings, (void*)(uintptr_t)desc->PhysicalStart, (void*)(uintptr_t)desc->PhysicalStart,
                                 desc->NumberOfPages, LoaderFirmwareTemporary);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }
        }

        desc = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)desc + map_desc_size);
    }

    // add video RAM and BIOS ROM, if not reported by GetMemoryMap
    if (map_video_ram) {
        Status = add_mapping(bs, mappings, NULL, (void*)0xa0000, 0x60, LoaderFirmwarePermanent);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            return Status;
        }
    }

    *va = va2;

    return EFI_SUCCESS;
}

static EFI_STATUS setup_memory_descriptor_list(LIST_ENTRY* mappings, LOADER_BLOCK1A* block1, void* pa, void* va) {
    LIST_ENTRY* le;
    void* data;

    data = pa;

    // populate list based on mappings

    le = mappings->Flink;
    while (le != mappings) {
        mapping* m = _CR(le, mapping, list_entry);
        MEMORY_ALLOCATION_DESCRIPTOR* mad = (MEMORY_ALLOCATION_DESCRIPTOR*)data;

        mad->MemoryType = m->type;
        mad->BasePage = (uintptr_t)m->pa / EFI_PAGE_SIZE;
        mad->PageCount = m->pages;

        InsertTailList(&block1->MemoryDescriptorListHead, &mad->ListEntry);

        data = (uint8_t*)data + sizeof(MEMORY_ALLOCATION_DESCRIPTOR);

        le = le->Flink;
    }

    // merge together where we can

    le = block1->MemoryDescriptorListHead.Flink;
    while (le != &block1->MemoryDescriptorListHead) {
        MEMORY_ALLOCATION_DESCRIPTOR* mad = _CR(le, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        MEMORY_ALLOCATION_DESCRIPTOR* mad2 = _CR(le->Flink, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);

        if (le->Flink == &block1->MemoryDescriptorListHead)
            break;

        if (mad->BasePage + mad->PageCount == mad2->BasePage && mad->MemoryType == mad2->MemoryType) {
            mad->PageCount += mad2->PageCount;
            RemoveEntryList(&mad2->ListEntry);
            continue;
        }

        le = le->Flink;
    }

#if 0
    {
        unsigned int i = 0;

        le = block1->MemoryDescriptorListHead.Flink;
        while (le != &block1->MemoryDescriptorListHead) {
            MEMORY_ALLOCATION_DESCRIPTOR* mad = _CR(le, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);

            print_hex(mad->BasePage);
            print(L", ");
            print_hex(mad->PageCount);
            print(L", ");
            print_hex(mad->MemoryType);

            if (i % 3 == 2)
                print(L"\r\n");
            else
                print(L"\t");

            le = le->Flink;
            i++;
        }
    }
#endif

    // change to virtual addresses

    le = block1->MemoryDescriptorListHead.Flink;

    while (le != &block1->MemoryDescriptorListHead) {
        LIST_ENTRY* le2 = le->Flink;

        if (le->Flink == &block1->MemoryDescriptorListHead)
            le->Flink = block1->MemoryDescriptorListHead.Flink->Blink;
        else
            le->Flink = fix_address_mapping(le->Flink, pa, va);

        if (le->Blink == &block1->MemoryDescriptorListHead)
            le->Blink = find_virtual_address(le->Blink, mappings);
        else
            le->Blink = fix_address_mapping(le->Blink, pa, va);

        le = le2;
    }

    block1->MemoryDescriptorListHead.Flink = fix_address_mapping(block1->MemoryDescriptorListHead.Flink, pa, va);
    block1->MemoryDescriptorListHead.Blink = fix_address_mapping(block1->MemoryDescriptorListHead.Blink, pa, va);

    return EFI_SUCCESS;
}

static EFI_STATUS allocate_mdl(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void* va,
                               void** pa, size_t* mdl_pages) {
    EFI_STATUS Status;
    unsigned int num_entries = 0;
    LIST_ENTRY* le;
    size_t pages;
    EFI_PHYSICAL_ADDRESS addr;

    // FIXME - ought to loop until no. of pages required for list is stable

    // count entries needed
    le = mappings->Flink;
    while (le != mappings) {
        num_entries++;
        le = le->Flink;
    }

    // add one for list itself
    num_entries++;

    // allocate pages for list
    pages = PAGE_COUNT(num_entries * sizeof(MEMORY_ALLOCATION_DESCRIPTOR));

    Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    Status = add_mapping(bs, mappings, va, (void*)(uintptr_t)addr, pages, LoaderMemoryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    *pa = (void*)(uintptr_t)addr;
    *mdl_pages = pages;

    return EFI_SUCCESS;
}

#ifdef _X86_
// Vista seems to assume that all our virtual addresses are the physical addresses
// plus 0x80000000. Here we trawl through the memory map to find an address which
// satisfies this.
static EFI_STATUS find_cr3(EFI_BOOT_SERVICES* bs, void* va, EFI_PHYSICAL_ADDRESS* addr) {
    EFI_STATUS Status;
    UINTN size = 0, key, descsize, count;
    UINT32 version;
    EFI_MEMORY_DESCRIPTOR *desc = NULL, *desc2;
    uintptr_t pa = (uintptr_t)va - MM_KSEG0_BASE;

    do {
        EFI_STATUS Status2;

        Status = bs->GetMemoryMap(&size, desc, &key, &descsize, &version);

        if (!EFI_ERROR(Status))
            break;
        else if (Status != EFI_BUFFER_TOO_SMALL) {
            print_error(L"GetMemoryMap", Status);

            if (desc)
                bs->FreePool(desc);

            return Status;
        }

        if (desc)
            bs->FreePool(desc);

        Status2 = bs->AllocatePool(EfiLoaderData, size, (void**)&desc);
        if (EFI_ERROR(Status2)) {
            print_error(L"AllocatePool", Status2);
            return Status2;
        }
    } while (Status == EFI_BUFFER_TOO_SMALL);

    if (!desc)
        return EFI_INVALID_PARAMETER;

    count = size / descsize;
    desc2 = desc;

    for (unsigned int i = 0; i < count; i++) {
        if (desc2->Type == EfiConventionalMemory) {
            if (desc2->PhysicalStart + ((desc2->NumberOfPages - 1) * EFI_PAGE_SIZE) >= pa) {
                if (pa >= desc2->PhysicalStart)
                    *addr = pa;
                else
                    *addr = desc2->PhysicalStart;

                Status = bs->AllocatePages(AllocateAddress, EfiBootServicesData, 1, addr);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePages", Status);
                    return Status;
                }

                return EFI_SUCCESS;
            }
        }

        desc2 = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)desc2 + descsize);
    }

    print(L"Unable to find address for CR3.\r\n");

    return EFI_NOT_FOUND;
}
#endif

#ifdef __x86_64__
// HAL on amd64 wants page directories set up for the last four megabytes of VA space
static EFI_STATUS add_hal_mappings(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings) {
    HARDWARE_PTE_PAE* pdpt;
    HARDWARE_PTE_PAE* pd;

    if (!pml4[(HAL_MEMORY >> 39) & 0x1ff].Valid) {
        EFI_STATUS Status;
        EFI_PHYSICAL_ADDRESS addr;

        Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            return Status;
        }

        Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)addr, 1, LoaderMemoryData);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            return Status;
        }

        memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

        pml4[(HAL_MEMORY >> 39) & 0x1ff].PageFrameNumber = addr / EFI_PAGE_SIZE;
        pml4[(HAL_MEMORY >> 39) & 0x1ff].Valid = 1;
        pml4[(HAL_MEMORY >> 39) & 0x1ff].Write = 1;

        pdpt = (HARDWARE_PTE_PAE*)(uintptr_t)addr;
    } else {
        uint64_t ptr;

        ptr = pml4[(HAL_MEMORY >> 39) & 0x1ff].PageFrameNumber;
        ptr <<= EFI_PAGE_SHIFT;

        pdpt = (HARDWARE_PTE_PAE*)(uintptr_t)ptr;
    }

    if (!pdpt[(HAL_MEMORY >> 30) & 0x1ff].Valid) {
        EFI_STATUS Status;
        EFI_PHYSICAL_ADDRESS addr;

        Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            return Status;
        }

        Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)addr, 1, LoaderMemoryData);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            return Status;
        }

        memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

        pdpt[(HAL_MEMORY >> 30) & 0x1ff].PageFrameNumber = addr / EFI_PAGE_SIZE;
        pdpt[(HAL_MEMORY >> 30) & 0x1ff].Valid = 1;
        pdpt[(HAL_MEMORY >> 30) & 0x1ff].Write = 1;

        pd = (HARDWARE_PTE_PAE*)(uintptr_t)addr;
    } else {
        uint64_t ptr;

        ptr = pdpt[(HAL_MEMORY >> 30) & 0x1ff].PageFrameNumber;
        ptr <<= EFI_PAGE_SHIFT;

        pd = (HARDWARE_PTE_PAE*)(uintptr_t)ptr;
    }

    for (unsigned int i = 0; i < 2; i++) {
        if (!pd[((HAL_MEMORY >> 21) & 0x1ff) + i].Valid) {
            EFI_STATUS Status;
            EFI_PHYSICAL_ADDRESS addr;

            Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePages", Status);
                return Status;
            }

            Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)addr, 1, LoaderMemoryData);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE);

            pd[((HAL_MEMORY >> 21) & 0x1ff) + i].PageFrameNumber = addr / EFI_PAGE_SIZE;
            pd[((HAL_MEMORY >> 21) & 0x1ff) + i].Valid = 1;
            pd[((HAL_MEMORY >> 21) & 0x1ff) + i].Write = 1;
        }
    }

    return EFI_SUCCESS;
}
#endif

EFI_STATUS map_efi_runtime(EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings, void** va, uint16_t version) {
    EFI_STATUS Status;
    unsigned int num_entries = 0;
    EFI_MEMORY_DESCRIPTOR* desc;
    EFI_MEMORY_DESCRIPTOR* desc2;
    EFI_PHYSICAL_ADDRESS addr;
    void* va2 = *va;

    desc = efi_memory_map;

    for (unsigned int i = 0; i < efi_map_size / map_desc_size; i++) {
        if (desc->Type == EfiRuntimeServicesData || desc->Type == EfiRuntimeServicesCode ||
            desc->Type == EfiMemoryMappedIO || desc->Type == EfiMemoryMappedIOPortSpace) {
            num_entries++;
        }

        desc = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)desc + map_desc_size);
    }

    efi_runtime_map_size = num_entries * map_desc_size;

    if (num_entries == 0) {
        efi_runtime_map = NULL;
        return EFI_SUCCESS;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, PAGE_COUNT(efi_runtime_map_size), &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    efi_runtime_map = (EFI_MEMORY_DESCRIPTOR*)(uintptr_t)addr;

    memset(efi_runtime_map, 0, efi_runtime_map_size);

    desc = efi_memory_map;
    desc2 = efi_runtime_map;

    for (unsigned int i = 0; i < efi_map_size / map_desc_size; i++) {
        if (desc->Type == EfiRuntimeServicesData || desc->Type == EfiRuntimeServicesCode ||
            desc->Type == EfiMemoryMappedIO || desc->Type == EfiMemoryMappedIOPortSpace) {
            desc2->Type = desc->Type;
            desc2->PhysicalStart = desc->PhysicalStart;
            desc2->VirtualStart = (EFI_VIRTUAL_ADDRESS)(uintptr_t)va2;
            desc2->NumberOfPages = desc->NumberOfPages;
            desc2->Attribute = desc->Attribute;

            Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)desc->PhysicalStart,
                                desc->NumberOfPages, LoaderFirmwarePermanent);
            if (EFI_ERROR(Status)) {
                print_error(L"add_mapping", Status);
                return Status;
            }

            va2 = (uint8_t*)va2 + (desc->NumberOfPages * EFI_PAGE_SIZE);

            desc2 = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)desc2 + map_desc_size);
        }

        desc = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)desc + map_desc_size);
    }

    if (version >= _WIN32_WINNT_WINBLUE) {
        Status = add_mapping(bs, mappings, va2, (void*)(uintptr_t)efi_runtime_map,
                             PAGE_COUNT(efi_runtime_map_size), LoaderFirmwarePermanent);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            return Status;
        }

        va2 = (uint8_t*)va2 + (PAGE_COUNT(efi_runtime_map_size) * EFI_PAGE_SIZE);
    }

    *va = va2;

    return EFI_SUCCESS;
}

EFI_STATUS enable_paging(EFI_HANDLE image_handle, EFI_BOOT_SERVICES* bs, LIST_ENTRY* mappings,
                         LOADER_BLOCK1A* block1, void* va, uintptr_t* loader_pages_spanned) {
    EFI_STATUS Status;
    UINTN size, key, descsize;
    UINT32 version;
    EFI_MEMORY_DESCRIPTOR* mapdesc;
    unsigned int num_entries, j;
    EFI_PHYSICAL_ADDRESS addr;
    LIST_ENTRY* le;
    EFI_SYSTEM_TABLE* new_ST;
    void* mdl_pa;
    size_t mdl_pages;

    size = 0;

    // mark first page as LoaderFirmwarePermanent
    Status = add_mapping(bs, mappings, 0, 0, 1, LoaderFirmwarePermanent);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

    // identity map our stack
    Status = add_mapping(bs, mappings, stack, stack, STACK_SIZE, LoaderOsloaderStack);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }

#ifdef _X86_
    if (pae) {
        EFI_PHYSICAL_ADDRESS cr3addr;

        Status = find_cr3(bs, va, &cr3addr);
        if (EFI_ERROR(Status)) {
            print_error(L"find_cr3", Status);
            return Status;
        }

        va = (void*)(uintptr_t)(cr3addr + EFI_PAGE_SIZE + MM_KSEG0_BASE);

        // Windows 8 needs these to be below 0x100000 for some reason
        addr = 0x100000;

        Status = bs->AllocatePages(AllocateMaxAddress, EfiBootServicesData, 4, &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            return Status;
        }

        pdpt = (HARDWARE_PTE_PAE*)(uintptr_t)cr3addr;

        memset(pdpt, 0, EFI_PAGE_SIZE);
        memset((void*)(uintptr_t)addr, 0, EFI_PAGE_SIZE * 4);

        for (unsigned int i = 0; i < 4; i++) {
            pdpt[i].PageFrameNumber = addr / EFI_PAGE_SIZE;
            pdpt[i].Valid = 1;

            addr += EFI_PAGE_SIZE;
        }
    } else {
        Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            return Status;
        }

        page_directory = (HARDWARE_PTE*)(uintptr_t)addr;
        memset(page_directory, 0, EFI_PAGE_SIZE);
    }
#elif defined(__x86_64__)
    Status = bs->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &addr);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePages", Status);
        return Status;
    }

    pml4 = (HARDWARE_PTE_PAE*)(uintptr_t)addr;

    memset(pml4, 0, EFI_PAGE_SIZE);

    Status = add_mapping(bs, mappings, NULL, pml4, 1, LoaderMemoryData);
    if (EFI_ERROR(Status)) {
        print_error(L"add_mapping", Status);
        return Status;
    }
#endif

    num_entries = 0;

    le = mappings->Flink;
    while (le != mappings) {
        num_entries++;
        le = le->Flink;
    }

    Status = bs->AllocatePool(EfiLoaderData, num_entries * sizeof(EFI_MEMORY_DESCRIPTOR), (void**)&mapdesc);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    j = 0;

    new_ST = systable;

    le = mappings->Flink;
    while (le != mappings) {
        mapping* m = _CR(le, mapping, list_entry);

//         mapdesc[j].Type = ?
        mapdesc[j].PhysicalStart = (uintptr_t)m->pa;
        mapdesc[j].VirtualStart = (uintptr_t)m->va;
        mapdesc[j].NumberOfPages = m->pages;
//         mapdesc[j].Attribute = ?
        // FIXME

        j++;
        le = le->Flink;
    }

    // add self-map
#ifdef _X86_
    if (pae) {
        HARDWARE_PTE_PAE* dir = (HARDWARE_PTE_PAE*)(pdpt[SELFMAP >> 30].PageFrameNumber * EFI_PAGE_SIZE);

        for (unsigned int i = 0; i < 4; i++) {
            dir[(SELFMAP >> 21 & 0x1ff) + i].PageFrameNumber = pdpt[i].PageFrameNumber;
            dir[(SELFMAP >> 21 & 0x1ff) + i].Valid = 1;
            dir[(SELFMAP >> 21 & 0x1ff) + i].Write = 1;
        }
    } else {
        page_directory[SELFMAP >> 22].PageFrameNumber = (uintptr_t)page_directory / EFI_PAGE_SIZE;
        page_directory[SELFMAP >> 22].Valid = 1;
        page_directory[SELFMAP >> 22].Write = 1;
    }
#elif defined(__x86_64__)
    pml4[(SELFMAP & 0xff8000000000) >> 39].PageFrameNumber = (uintptr_t)pml4 / EFI_PAGE_SIZE;
    pml4[(SELFMAP & 0xff8000000000) >> 39].Valid = 1;
    pml4[(SELFMAP & 0xff8000000000) >> 39].Write = 1;
#endif

    // do mappings
    {
        LIST_ENTRY* le = mappings->Flink;

        while (le != mappings) {
            mapping* m = _CR(le, mapping, list_entry);

            if (m->va) {
                if ((uint8_t*)systable >= (uint8_t*)m->pa && (uint8_t*)systable < (uint8_t*)m->pa + (m->pages * EFI_PAGE_SIZE))
                    new_ST = (EFI_SYSTEM_TABLE*)((uint8_t*)systable - (uint8_t*)m->pa + (uint8_t*)m->va);

                Status = map_memory(bs, mappings, (uintptr_t)m->va, (uintptr_t)m->pa, m->pages);
                if (EFI_ERROR(Status)) {
                    print_error(L"map_memory", Status);
                    return Status;
                }
            }

            le = le->Flink;
        }
    }

    // map first page (doesn't get mapped above because VA is 0)
    Status = map_memory(bs, mappings, 0, 0, 1);
    if (EFI_ERROR(Status)) {
        print_error(L"map_memory", Status);
        return Status;
    }

#ifdef _X86_
    if (pae) { // map cr3
        Status = map_memory(bs, mappings, ((uintptr_t)pdpt + MM_KSEG0_BASE), (uintptr_t)pdpt, 1);
        if (EFI_ERROR(Status)) {
            print_error(L"map_memory", Status);
            return Status;
        }
    }

    if (pae) {
        for (unsigned int i = 0; i < 4; i++) {
            HARDWARE_PTE_PAE* dir = (HARDWARE_PTE_PAE*)(pdpt[i].PageFrameNumber * EFI_PAGE_SIZE);

            for (unsigned int j = 0; j < EFI_PAGE_SIZE / sizeof(HARDWARE_PTE_PAE); j++) {
                if (dir[j].Valid) {
                    Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)(dir[j].PageFrameNumber * EFI_PAGE_SIZE),
                                         1, LoaderMemoryData);
                    if (EFI_ERROR(Status)) {
                        print_error(L"add_mapping", Status);
                        return Status;
                    }
                }
            }
        }

        Status = add_mapping(bs, mappings, NULL, pdpt, 1, LoaderMemoryData);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            return Status;
        }
    } else {
        for (unsigned int i = 0; i < EFI_PAGE_SIZE / sizeof(HARDWARE_PTE); i++) {
            if (page_directory[i].Valid) {
                Status = add_mapping(bs, mappings, NULL, (void*)(uintptr_t)(page_directory[i].PageFrameNumber * EFI_PAGE_SIZE),
                                     1, LoaderMemoryData);
                if (EFI_ERROR(Status)) {
                    print_error(L"add_mapping", Status);
                    return Status;
                }
            }
        }
    }
#elif defined(__x86_64__)
    Status = add_hal_mappings(bs, mappings);
    if (EFI_ERROR(Status)) {
        print_error(L"add_hal_mappings", Status);
        return Status;
    }
#endif

    if (apic) {
        Status = map_memory(bs, mappings, APIC_BASE, (uintptr_t)apic, 1);
        if (EFI_ERROR(Status)) {
            print_error(L"map_memory", Status);
            return Status;
        }
    }

    Status = allocate_mdl(bs, mappings, va, &mdl_pa, &mdl_pages);
    if (EFI_ERROR(Status)) {
        print_error(L"allocate_mdl", Status);
        return Status;
    }

    Status = map_memory(bs, mappings, (uintptr_t)va, (uintptr_t)mdl_pa, mdl_pages);
    if (EFI_ERROR(Status)) {
        print_error(L"map_memory", Status);
        return Status;
    }

    Status = setup_memory_descriptor_list(mappings, block1, mdl_pa, va);
    if (EFI_ERROR(Status)) {
        print_error(L"setup_memory_descriptor_list", Status);
        return Status;
    }

    va = (uint8_t*)va + (mdl_pages * EFI_PAGE_SIZE);

    // get new key
    Status = bs->GetMemoryMap(&size, NULL, &key, &descsize, &version);
    if (EFI_ERROR(Status) && Status != EFI_BUFFER_TOO_SMALL) {
        print_error(L"GetMemoryMap", Status);
        return Status;
    }

    size *= 2;

    Status = bs->AllocatePool(EfiLoaderData, size, (void**)&mapdesc);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    Status = bs->GetMemoryMap(&size, mapdesc, &key, &descsize, &version);
    if (EFI_ERROR(Status)) {
        print_error(L"GetMemoryMap", Status);
        return Status;
    }

    Status = bs->ExitBootServices(image_handle, key);
    if (EFI_ERROR(Status)) {
        print_error(L"ExitBootServices", Status);
        return Status;
    }

    Status = systable->RuntimeServices->SetVirtualAddressMap(efi_runtime_map_size, map_desc_size,
                                                             EFI_MEMORY_DESCRIPTOR_VERSION, efi_runtime_map);
    if (EFI_ERROR(Status)) {
        print_error(L"SetVirtualAddressMap", Status);
        return Status;
    }

    if (loader_pages_spanned)
        *loader_pages_spanned = ((uintptr_t)va - MM_KSEG0_BASE) / EFI_PAGE_SIZE;

#ifdef _X86_
    // disable paging
    __writecr0(__readcr0() & ~CR0_PG);

    // disable write-protection (Windows will set this itself)
    __writecr0(__readcr0() & ~CR0_WP);

    if (pae) {
        // enable PAE
        __writecr4(__readcr4() | CR4_PAE);

        // set cr3
        __writecr3((uintptr_t)pdpt);
    } else {
        // disable PAE
        __writecr4(__readcr4() & ~CR4_PAE);

        // set cr3
        __writecr3((uintptr_t)page_directory);
    }

    // enable paging again
    __writecr0(__readcr0() | CR0_PG);
#elif defined(__x86_64__)
    // set PGE (HalpFlushTLB won't work if this isn't set)
    __writecr4(__readcr4() | CR4_PGE);

    // enable write-protection
    __writecr0(__readcr0() | CR0_WP);

    // set alignment mask
    __writecr0(__readcr0() | CR0_AM);

    // clear MP flag
    __writecr0(__readcr0() & ~CR0_MP);

    // set cr3
    __writecr3((uintptr_t)pml4);
#endif

    systable = new_ST;

    return EFI_SUCCESS;
}
