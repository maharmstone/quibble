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

#include <stddef.h>
#include <string.h>
#include "peload.h"
#include "peloaddef.h"
#include "misc.h"
#include "quibble.h"
#include "tinymt32.h"
#include "print.h"

typedef struct {
    EFI_PE_IMAGE public;
    void* va;
    uint32_t size;
    uint32_t pages;
} pe_image;

static EFI_HANDLE pe_handle = NULL;
static EFI_PE_LOADER_PROTOCOL proto;
static EFI_BOOT_SERVICES* bs;
static tinymt32_t mt;

static EFI_STATUS EFIAPI Load(EFI_FILE_HANDLE File, void* VirtualAddress, EFI_PE_IMAGE** Image);

EFI_STATUS pe_register(EFI_BOOT_SERVICES* BootServices, uint32_t seed) {
    EFI_GUID pe_guid = PE_LOADER_PROTOCOL;

    proto.Load = Load;

    tinymt32_init(&mt, seed);

    bs = BootServices;

    return bs->InstallProtocolInterface(&pe_handle, &pe_guid, EFI_NATIVE_INTERFACE, &proto);
}

EFI_STATUS pe_unregister() {
    EFI_GUID pe_guid = PE_LOADER_PROTOCOL;

    return bs->UninstallProtocolInterface(&pe_handle, &pe_guid, EFI_NATIVE_INTERFACE);
}

static bool check_header(uint8_t* data, size_t size, IMAGE_NT_HEADERS** nth) {
    IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)data;
    IMAGE_NT_HEADERS* nt_header;

    if (size < sizeof(IMAGE_DOS_HEADER)) {
        print_string("Image was shorter than IMAGE_DOS_HEADER.\n");
        return false;
    }

    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        print_string("Incorrect DOS signature.\n");
        return false;
    }

    nt_header = (IMAGE_NT_HEADERS*)(data + dos_header->e_lfanew);

    // FIXME - check no overflow

    if (nt_header->Signature != IMAGE_NT_SIGNATURE) {
        print_string("Incorrect PE signature.\n");
        return false;
    }

#ifdef _X86_
    if (nt_header->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        print_string("Unsupported architecture.\n");
        return false;
    }
#elif defined(__x86_64__)
    if (nt_header->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        print_string("Unsupported architecture.\n");
        return false;
    }
#endif

    // FIXME - make sure optional header size is at least minimum

    if (nt_header->OptionalHeader32.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        nt_header->OptionalHeader32.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        print_string("Unrecognized optional header signature.\n");
        return false;
    }

    // FIXME - check checksum

    // FIXME - check Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE

    *nth = nt_header;

    return true;
}

static EFI_STATUS EFIAPI free_image(EFI_PE_IMAGE* This) {
    pe_image* img = _CR(This, pe_image, public);

    if (img->public.Data)
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)img->public.Data, img->pages);

    bs->FreePool(img);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_entry_point(EFI_PE_IMAGE* This, void** EntryPoint) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;

    if (!img->public.Data)
        return EFI_INVALID_PARAMETER;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        *EntryPoint = (uint8_t*)img->va + nt_header->OptionalHeader64.AddressOfEntryPoint;
    else
        *EntryPoint = (uint8_t*)img->va + nt_header->OptionalHeader32.AddressOfEntryPoint;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI list_imports(EFI_PE_IMAGE* This, EFI_IMPORT_LIST* ImportList, UINTN* BufferSize) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;
    IMAGE_IMPORT_DESCRIPTOR* iid;
    unsigned int total_entries, num_entries, needed_size, next_text, pos;

    if (!img->public.Data)
        return EFI_INVALID_PARAMETER;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (nt_header->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0 ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            *BufferSize = 0;
            return EFI_SUCCESS;
        }

        // FIXME - check not out of bounds

        iid = (IMAGE_IMPORT_DESCRIPTOR*)((uint8_t*)img->public.Data + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        total_entries = nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    } else {
        if (nt_header->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0 ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            *BufferSize = 0;
            return EFI_SUCCESS;
        }

        // FIXME - check not out of bounds

        iid = (IMAGE_IMPORT_DESCRIPTOR*)((uint8_t*)img->public.Data + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        total_entries = nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }

    // calculate size necessary

    needed_size = offsetof(EFI_IMPORT_LIST, Imports[0]);
    num_entries = 0;

    for (unsigned int i = 0; i < total_entries; i++) {
        char* name;
        bool dupe = false;

        if (iid[i].Name == 0)
            break;

        for (unsigned int j = 0; j < i; j++) {
            if (!stricmp((char*)((uint8_t*)img->public.Data + iid[i].Name), (char*)((uint8_t*)img->public.Data + iid[j].Name))) {
                dupe = true;
                break;
            }
        }

        if (dupe)
            continue;

        needed_size += sizeof(UINT32);

        name = (char*)((uint8_t*)img->public.Data + iid[i].Name);

        needed_size += strlen(name) + 1;

        num_entries++;
    }

    if (num_entries == 0) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    if (*BufferSize < needed_size) {
        *BufferSize = needed_size;
        return EFI_BUFFER_TOO_SMALL;
    }

    *BufferSize = needed_size;

    // copy data

    ImportList->NumberOfImports = num_entries;

    next_text = offsetof(EFI_IMPORT_LIST, Imports[0]) + (num_entries * sizeof(UINT32));
    pos = 0;

    for (unsigned int i = 0; i < total_entries; i++) {
        char* name;
        unsigned int namelen;
        bool dupe = false;

        if (iid[i].Name == 0)
            break;

        for (unsigned int j = 0; j < i; j++) {
            if (!stricmp((char*)((uint8_t*)img->public.Data + iid[i].Name), (char*)((uint8_t*)img->public.Data + iid[j].Name))) {
                dupe = true;
                break;
            }
        }

        if (dupe)
            continue;

        name = (char*)((uint8_t*)img->public.Data + iid[i].Name);
        namelen = strlen(name);

        ImportList->Imports[pos] = next_text;

        memcpy((uint8_t*)ImportList + next_text, name, namelen + 1);

        next_text += namelen + 1;
        pos++;
    }

    return EFI_SUCCESS;
}

static EFI_PHYSICAL_ADDRESS EFIAPI get_address(EFI_PE_IMAGE* This) {
    pe_image* img = _CR(This, pe_image, public);

    return (EFI_PHYSICAL_ADDRESS)(uintptr_t)img->public.Data;
}

static UINT32 EFIAPI get_size(EFI_PE_IMAGE* This) {
    pe_image* img = _CR(This, pe_image, public);

    return img->size;
}

static UINT32 EFIAPI get_checksum(EFI_PE_IMAGE* This) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nt_header->OptionalHeader64.CheckSum;
    else
        return nt_header->OptionalHeader32.CheckSum;
}

static UINT16 EFIAPI get_dll_characteristics(EFI_PE_IMAGE* This) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nt_header->OptionalHeader64.DllCharacteristics;
    else
        return nt_header->OptionalHeader32.DllCharacteristics;
}

static EFI_STATUS resolve_imports2_64(pe_image* img, pe_image* img2, IMAGE_EXPORT_DIRECTORY* export_dir,
                                   uint64_t* orig_thunk_table, uint64_t* thunk_table,
                                   EFI_PE_IMAGE_RESOLVE_FORWARD ResolveForward) {
    EFI_STATUS Status;
    IMAGE_DOS_HEADER* dos_header2 = (IMAGE_DOS_HEADER*)img2->public.Data;
    IMAGE_NT_HEADERS* nt_header2 = (IMAGE_NT_HEADERS*)((uint8_t*)img2->public.Data + dos_header2->e_lfanew);
    uint16_t* ordinal_table = (uint16_t*)((uint8_t*)img2->public.Data + export_dir->AddressOfNameOrdinals);
    uint32_t* name_table = (uint32_t*)((uint8_t*)img2->public.Data + export_dir->AddressOfNames);
    uint32_t* function_table = (uint32_t*)((uint8_t*)img2->public.Data + export_dir->AddressOfFunctions);

    // FIXME - use hints?

    // loop through import names

    while (*orig_thunk_table) {
        char* name = (char*)((uint8_t*)img->public.Data + *orig_thunk_table + sizeof(uint16_t));
        uint32_t index;
        uint16_t ordinal;
        void* func;
        bool found = false;

        if (*orig_thunk_table & 0x8000000000000000)
            ordinal = (*orig_thunk_table & ~0x8000000000000000) - 1; // FIXME - make sure not out of bounds
        else {
            for (unsigned int i = 0; i < export_dir->NumberOfNames; i++) {
                char* export_name = (char*)((uint8_t*)img2->public.Data + name_table[i]);

                if (!strcmp(export_name, name)) {
                    index = i;
                    found = true;
                    break;
                }
            }

            if (!found) {
                char s[255], *p;

                p = stpcpy(s, "Unable to resolve function ");
                p = stpcpy(p, name);
                p = stpcpy(p, ".\n");

                print_string(s);

                return EFI_INVALID_PARAMETER;
            }

            ordinal = ordinal_table[index];
        }

        if ((nt_header2->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            function_table[ordinal] >= nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress &&
            function_table[ordinal] < nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress +
            nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size) ||
            (nt_header2->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
            function_table[ordinal] >= nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress &&
            function_table[ordinal] < nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress +
            nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size)) { // forwarded
            char* redir_name = (char*)((uint8_t*)img2->public.Data + function_table[ordinal]);

            Status = ResolveForward(redir_name, thunk_table);
            if (EFI_ERROR(Status))
                return Status;
        } else {
            func = (uint8_t*)img2->va + function_table[ordinal];

            *thunk_table = (uintptr_t)func;
        }

        orig_thunk_table++;
        thunk_table++;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS resolve_imports2_32(pe_image* img, pe_image* img2, IMAGE_EXPORT_DIRECTORY* export_dir,
                                      uint32_t* orig_thunk_table, uint32_t* thunk_table,
                                      EFI_PE_IMAGE_RESOLVE_FORWARD ResolveForward) {
    EFI_STATUS Status;
    IMAGE_DOS_HEADER* dos_header2 = (IMAGE_DOS_HEADER*)img2->public.Data;
    IMAGE_NT_HEADERS* nt_header2 = (IMAGE_NT_HEADERS*)((uint8_t*)img2->public.Data + dos_header2->e_lfanew);
    uint16_t* ordinal_table = (uint16_t*)((uint8_t*)img2->public.Data + export_dir->AddressOfNameOrdinals);
    uint32_t* name_table = (uint32_t*)((uint8_t*)img2->public.Data + export_dir->AddressOfNames);
    uint32_t* function_table = (uint32_t*)((uint8_t*)img2->public.Data + export_dir->AddressOfFunctions);

    // FIXME - use hints?

    // loop through import names

    while (*orig_thunk_table) {
        char* name = (char*)((uint8_t*)img->public.Data + *orig_thunk_table + sizeof(uint16_t));
        uint32_t index;
        uint16_t ordinal;
        void* func;
        bool found = false;

        if (*orig_thunk_table & 0x80000000)
            ordinal = (*orig_thunk_table & ~0x80000000) - 1; // FIXME - make sure not out of bounds
        else {
            for (unsigned int i = 0; i < export_dir->NumberOfNames; i++) {
                char* export_name = (char*)((uint8_t*)img2->public.Data + name_table[i]);

                if (!strcmp(export_name, name)) {
                    index = i;
                    found = true;
                    break;
                }
            }

            if (!found) {
                char s[255], *p;

                p = stpcpy(s, "Unable to resolve function ");
                p = stpcpy(p, name);
                p = stpcpy(p, ".\n");

                print_string(s);

                return EFI_INVALID_PARAMETER;
            }

            ordinal = ordinal_table[index];
        }

        if ((nt_header2->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            function_table[ordinal] >= nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress &&
            function_table[ordinal] < nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress +
            nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size) ||
            (nt_header2->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
            function_table[ordinal] >= nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress &&
            function_table[ordinal] < nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress +
            nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size)) { // forwarded
            char* redir_name = (char*)((uint8_t*)img2->public.Data + function_table[ordinal]);
            uint64_t addr;

            Status = ResolveForward(redir_name, &addr);
            if (EFI_ERROR(Status))
                return Status;

            *thunk_table = addr;
        } else {
            func = (uint8_t*)img2->va + function_table[ordinal];

            *thunk_table = (uintptr_t)func;
        }

        orig_thunk_table++;
        thunk_table++;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI resolve_imports(EFI_PE_IMAGE* This, char* LibraryName, EFI_PE_IMAGE* Library,
                                         EFI_PE_IMAGE_RESOLVE_FORWARD ResolveForward) {
    EFI_STATUS Status;
    pe_image* img = _CR(This, pe_image, public);
    pe_image* img2 = _CR(Library, pe_image, public);
    IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    IMAGE_NT_HEADERS* nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);
    IMAGE_DOS_HEADER* dos_header2 = (IMAGE_DOS_HEADER*)img2->public.Data;
    IMAGE_NT_HEADERS* nt_header2 = (IMAGE_NT_HEADERS*)((uint8_t*)img2->public.Data + dos_header2->e_lfanew);
    IMAGE_EXPORT_DIRECTORY* export_dir;
    IMAGE_IMPORT_DESCRIPTOR* iid;
    bool found = false;
    unsigned int num_entries;

    // find imports data directory

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (nt_header->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0 ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            print_string("Imports list not found.\n");
            return EFI_INVALID_PARAMETER;
        }

        iid = (IMAGE_IMPORT_DESCRIPTOR*)((uint8_t*)img->public.Data + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        num_entries = nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    } else {
        if (nt_header->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0 ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            print_string("Imports list not found.\n");
            return EFI_INVALID_PARAMETER;
        }

        iid = (IMAGE_IMPORT_DESCRIPTOR*)((uint8_t*)img->public.Data + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        num_entries = nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }

    // find exports data directory

    if (nt_header2->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (nt_header2->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
            nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0 ||
            nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
            print_string("Exports list not found.\n");
            return EFI_INVALID_PARAMETER;
        }

        export_dir = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)img2->public.Data + nt_header2->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    } else {
        if (nt_header2->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
            nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0 ||
            nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
            print_string("Exports list not found.\n");
            return EFI_INVALID_PARAMETER;
        }

        export_dir = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)img2->public.Data + nt_header2->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    }

    // find import for library name

    for (unsigned int i = 0; i < num_entries; i++) {
        const char* name = (const char*)((uint8_t*)img->public.Data + iid[i].Name);

        if (!stricmp(name, LibraryName)) {
            if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                uint64_t* orig_thunk_table = (uint64_t*)((uint8_t*)img->public.Data + iid[i].Characteristics);
                uint64_t* thunk_table = (uint64_t*)((uint8_t*)img->public.Data + iid[i].FirstThunk);

                Status = resolve_imports2_64(img, img2, export_dir, orig_thunk_table, thunk_table, ResolveForward);
                if (EFI_ERROR(Status))
                    return Status;
            } else {
                uint32_t* orig_thunk_table = (uint32_t*)((uint8_t*)img->public.Data + iid[i].Characteristics);
                uint32_t* thunk_table = (uint32_t*)((uint8_t*)img->public.Data + iid[i].FirstThunk);

                Status = resolve_imports2_32(img, img2, export_dir, orig_thunk_table, thunk_table, ResolveForward);
                if (EFI_ERROR(Status))
                    return Status;
            }

            found = true;
        }
    }

    if (!found) {
        print_string("Import not found.\n");
        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

static void do_relocations(pe_image* img, IMAGE_NT_HEADERS* nt_header) {
    IMAGE_BASE_RELOCATION* reloc;
    uint32_t size, count;
    uint16_t* addr;
    uint64_t base;

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (nt_header->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0 ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size < sizeof(IMAGE_BASE_RELOCATION)) {
            return;
        }

        size = nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

        reloc = (IMAGE_BASE_RELOCATION*)((uint8_t*)img->public.Data + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);

        base = nt_header->OptionalHeader64.ImageBase;
    } else {
        if (nt_header->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0 ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size < sizeof(IMAGE_BASE_RELOCATION)) {
            return;
        }

        size = nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

        reloc = (IMAGE_BASE_RELOCATION*)((uint8_t*)img->public.Data + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);

        base = nt_header->OptionalHeader32.ImageBase;
    }

    do {
        uint32_t* ptr;

        if (size < reloc->SizeOfBlock)
            return;

        // FIXME - check not out of bounds
        ptr = (uint32_t*)((uint8_t*)img->public.Data + reloc->VirtualAddress);

        addr = (uint16_t*)((uint8_t*)reloc + sizeof(IMAGE_BASE_RELOCATION));
        count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);

        for (unsigned int i = 0; i < count; i++) {
            uint16_t offset = addr[i] & 0xfff;
            uint16_t type = addr[i] >> 12;

            switch (type) {
                case IMAGE_REL_BASED_ABSOLUTE:
                    // nop
                break;

                case IMAGE_REL_BASED_HIGHLOW:
                {
                    uint32_t* ptr2 = (uint32_t*)((uint8_t*)ptr + offset);

                    *ptr2 = *ptr2 - base + (uintptr_t)img->va;
                    break;
                }

                case IMAGE_REL_BASED_DIR64:
                {
                    uint64_t* ptr2 = (uint64_t*)((uint8_t*)ptr + offset);

                    *ptr2 = *ptr2 - base + (uintptr_t)img->va;
                    break;
                }

                default: {
                    char s[255], *p;

                    p = stpcpy(s, "Unsupported relocation type ");
                    p = hex_to_str(p, type);
                    p = stpcpy(p, ".\n");

                    print_string(s);

                    return;
                }
            }
        }

        size -= reloc->SizeOfBlock;

        if (size < sizeof(IMAGE_BASE_RELOCATION))
            return;

        reloc = (IMAGE_BASE_RELOCATION*)((uint8_t*)reloc + reloc->SizeOfBlock);
    } while (true);
}

static void randomize_security_cookie(pe_image* img, IMAGE_NT_HEADERS* nt_header) {
    uint32_t size;

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        uint64_t* cookie;
        IMAGE_LOAD_CONFIG_DIRECTORY64* config;

        if (nt_header->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress == 0 ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size < sizeof(uint32_t)) {
            return;
        }

        size = nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size;

        config = (IMAGE_LOAD_CONFIG_DIRECTORY64*)((uint8_t*)img->public.Data + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress);

        if (config->Size < size)
            size = config->Size;

        if (size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) + sizeof(uint64_t))
            return;

        if (config->SecurityCookie == 0)
            return;

        cookie = (uint64_t*)((uint8_t*)img->public.Data + config->SecurityCookie - (uint8_t*)img->va);

        *(uint32_t*)cookie = tinymt32_generate_uint32(&mt);
        *((uint32_t*)cookie + 1) = tinymt32_generate_uint32(&mt);

        // Windows 8 wants the top 16 bits to be clear
        *cookie &= 0xffffffffffff;
    } else {
        uint32_t* cookie;
        IMAGE_LOAD_CONFIG_DIRECTORY32* config;

        if (nt_header->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress == 0 ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size < sizeof(uint32_t)) {
            return;
        }

        size = nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size;

        config = (IMAGE_LOAD_CONFIG_DIRECTORY32*)((uint8_t*)img->public.Data + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress);

        if (config->Size < size)
            size = config->Size;

        if (size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SecurityCookie) + sizeof(uint32_t))
            return;

        if (config->SecurityCookie == 0)
            return;

        cookie = (uint32_t*)((uint8_t*)img->public.Data + config->SecurityCookie - (uint8_t*)img->va);
        *cookie = tinymt32_generate_uint32(&mt);

        // XP wants the top 16 bits to be clear
        *cookie &= 0xffff;
    }
}

static EFI_STATUS EFIAPI move_address(EFI_PE_IMAGE* This, EFI_PHYSICAL_ADDRESS NewAddress) {
    pe_image* img = _CR(This, pe_image, public);
    void* newaddr = (void*)(uintptr_t)NewAddress;

    memcpy(newaddr, img->public.Data, img->size);

    bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)img->public.Data, img->pages);

    img->public.Data = newaddr;

    return EFI_SUCCESS;
}

static EFI_STATUS get_version4(VS_VERSION_INFO* ver, uint32_t size, uint32_t* version_ms, uint32_t* version_ls) {
    static const WCHAR key[] = L"VS_VERSION_INFO";

    if (ver->wLength > size) {
        char s[255], *p;

        p = stpcpy(s, "Version data had size of ");
        p = dec_to_str(p, ver->wLength);
        p = stpcpy(p, ", expected at least ");
        p = dec_to_str(p, size);
        p = stpcpy(p, ".\n");

        print_string(s);

        return EFI_INVALID_PARAMETER;
    }

    if (ver->wValueLength < sizeof(VS_FIXEDFILEINFO)) {
        print_string("Version data was shorter than VS_FIXEDFILEINFO.\n");
        return EFI_INVALID_PARAMETER;
    }

    if (memcmp(ver->szKey, key, sizeof(key))) {
        print_string("Invalid key in version data.\n");
        return EFI_INVALID_PARAMETER;
    }

    if (ver->Value.dwSignature != VS_FFI_SIGNATURE) {
        print_string("Invalid signature in version data.\n");
        return EFI_INVALID_PARAMETER;
    }

    *version_ms = ver->Value.dwFileVersionMS;
    *version_ls = ver->Value.dwFileVersionLS;

    return EFI_SUCCESS;
}

static EFI_STATUS get_version3(pe_image* img, void* res, uint32_t ressize, uint32_t offset, uint32_t* version_ms, uint32_t* version_ls) {
    IMAGE_RESOURCE_DIRECTORY* resdir;
    IMAGE_RESOURCE_DIRECTORY_ENTRY* ents;
    uint32_t size = ressize - offset;

    if (size < sizeof(IMAGE_RESOURCE_DIRECTORY)) {
        print_string("Size was too short for resource directory.\n");
        return EFI_INVALID_PARAMETER;
    }

    resdir = (IMAGE_RESOURCE_DIRECTORY*)((uint8_t*)res + offset);

    if (size < sizeof(IMAGE_RESOURCE_DIRECTORY) + ((resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY))) {
        print_string("Resource directory was truncated.\n");
        return EFI_INVALID_PARAMETER;
    }

    ents = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)((uint8_t*)resdir + sizeof(IMAGE_RESOURCE_DIRECTORY));

    for (unsigned int i = 0; i < resdir->NumberOfIdEntries; i++) {
        IMAGE_RESOURCE_DATA_ENTRY* irde;

        if (ents[resdir->NumberOfNamedEntries + i].OffsetToData > ressize) {
            print_string("Offset was after end of directory.\n");
            return EFI_INVALID_PARAMETER;
        }

        irde = (IMAGE_RESOURCE_DATA_ENTRY*)((uint8_t*)res + ents[resdir->NumberOfNamedEntries + i].OffsetToData);

        if (irde->OffsetToData + irde->Size > img->size) {
            print_string("Version data goes past end of file.\n");
            return EFI_INVALID_PARAMETER;
        }

        return get_version4((VS_VERSION_INFO*)((uint8_t*)img->public.Data + irde->OffsetToData), irde->Size, version_ms, version_ls);
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS get_version2(pe_image* img, void* res, uint32_t ressize, uint32_t offset, uint32_t* version_ms, uint32_t* version_ls) {
    EFI_STATUS Status;
    IMAGE_RESOURCE_DIRECTORY* resdir;
    IMAGE_RESOURCE_DIRECTORY_ENTRY* ents;
    uint32_t size = ressize - offset;

    if (size < sizeof(IMAGE_RESOURCE_DIRECTORY)) {
        print_string("Size was too short for resource directory.\n");
        return EFI_INVALID_PARAMETER;
    }

    resdir = (IMAGE_RESOURCE_DIRECTORY*)((uint8_t*)res + offset);

    if (size < sizeof(IMAGE_RESOURCE_DIRECTORY) + ((resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY))) {
        print_string("Resource directory was truncated.\n");
        return EFI_INVALID_PARAMETER;
    }

    ents = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)((uint8_t*)resdir + sizeof(IMAGE_RESOURCE_DIRECTORY));

    for (unsigned int i = 0; i < resdir->NumberOfIdEntries; i++) {
        if (ents[resdir->NumberOfNamedEntries + i].OffsetToDirectory > ressize) {
            print_string("Offset was after end of directory.\n");
            return EFI_INVALID_PARAMETER;
        }

        Status = get_version3(img, res, ressize, ents[resdir->NumberOfNamedEntries + i].OffsetToDirectory, version_ms, version_ls);

        if (Status != EFI_NOT_FOUND) {
            if (EFI_ERROR(Status))
                print_error("get_version3", Status);

            return Status;
        }
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI get_version(EFI_PE_IMAGE* This, UINT32* VersionMS, UINT32* VersionLS) {
    EFI_STATUS Status;
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    IMAGE_NT_HEADERS* nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);
    IMAGE_RESOURCE_DIRECTORY* resdir;
    IMAGE_RESOURCE_DIRECTORY_ENTRY* ents;
    unsigned int dirsize;

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (nt_header->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress == 0 ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size < sizeof(IMAGE_RESOURCE_DIRECTORY)) {
            print_string("Resource directory not found.\n");
            return EFI_NOT_FOUND;
        }

        resdir = (IMAGE_RESOURCE_DIRECTORY*)((uint8_t*)img->public.Data + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress);

        if (nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size <
            sizeof(IMAGE_RESOURCE_DIRECTORY) + ((resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY))) {
            print_string("Resource directory was truncated.\n");
            return EFI_INVALID_PARAMETER;
        }

        dirsize = nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size;
    } else {
        if (nt_header->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress == 0 ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size < sizeof(IMAGE_RESOURCE_DIRECTORY)) {
            print_string("Resource directory not found.\n");
            return EFI_NOT_FOUND;
        }

        resdir = (IMAGE_RESOURCE_DIRECTORY*)((uint8_t*)img->public.Data + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress);

        if (nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size <
            sizeof(IMAGE_RESOURCE_DIRECTORY) + ((resdir->NumberOfNamedEntries + resdir->NumberOfIdEntries) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY))) {
            print_string("Resource directory was truncated.\n");
            return EFI_INVALID_PARAMETER;
        }

        dirsize = nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size;
    }

    ents = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)((uint8_t*)resdir + sizeof(IMAGE_RESOURCE_DIRECTORY));

    for (unsigned int i = 0; i < resdir->NumberOfIdEntries; i++) {
        if (ents[resdir->NumberOfNamedEntries + i].Id == RT_VERSION) {
            void* addr = img->public.Data;

            if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                addr = (uint8_t*)addr + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
            else
                addr = (uint8_t*)addr + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;

            addr = (uint8_t*)addr + ents[resdir->NumberOfNamedEntries + i].OffsetToDirectory;

            if (ents[resdir->NumberOfNamedEntries + i].OffsetToDirectory > dirsize) {
                print_string("Offset was after end of directory.\n");
                return EFI_INVALID_PARAMETER;
            }

            Status = get_version2(img, resdir, dirsize, ents[resdir->NumberOfNamedEntries + i].OffsetToDirectory, VersionMS, VersionLS);

            if (Status != EFI_NOT_FOUND) {
                if (EFI_ERROR(Status))
                    print_error("get_version2", Status);

                return Status;
            }
        }
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI find_export(EFI_PE_IMAGE* This, char* Function, UINT64* Address,
                                     EFI_PE_IMAGE_RESOLVE_FORWARD ResolveForward) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    IMAGE_NT_HEADERS* nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);
    IMAGE_EXPORT_DIRECTORY* export_dir;
    uint16_t* ordinal_table;
    uint32_t* name_table;
    uint32_t* function_table;
    bool found = false;
    uint32_t index, ordinal;

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (nt_header->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0 ||
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
            print_string("Exports list not found.\n");
            return EFI_INVALID_PARAMETER;
        }

        export_dir = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)img->public.Data + nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    } else {
        if (nt_header->OptionalHeader32.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0 ||
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
            print_string("Exports list not found.\n");
            return EFI_INVALID_PARAMETER;
        }

        export_dir = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)img->public.Data + nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    }

    ordinal_table = (uint16_t*)((uint8_t*)img->public.Data + export_dir->AddressOfNameOrdinals);
    name_table = (uint32_t*)((uint8_t*)img->public.Data + export_dir->AddressOfNames);
    function_table = (uint32_t*)((uint8_t*)img->public.Data + export_dir->AddressOfFunctions);

    for (unsigned int i = 0; i < export_dir->NumberOfNames; i++) {
        char* export_name = (char*)((uint8_t*)img->public.Data + name_table[i]);

        if (!strcmp(export_name, Function)) {
            index = i;
            found = true;
            break;
        }
    }

    if (!found) {
        char s[255], *p;

        p = stpcpy(s, "Unable to resolve function ");
        p = stpcpy(p, Function);
        p = stpcpy(p, ".\n");

        print_string(s);

        return EFI_NOT_FOUND;
    }

    ordinal = ordinal_table[index];

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (function_table[ordinal] >= nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress &&
            function_table[ordinal] < nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress +
            nt_header->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size) { // forwarded
                char* redir_name = (char*)((uint8_t*)img->public.Data + function_table[ordinal]);

            return ResolveForward(redir_name, Address);
        }
    } else {
        if (function_table[ordinal] >= nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress &&
            function_table[ordinal] < nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress +
            nt_header->OptionalHeader32.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size) { // forwarded
                char* redir_name = (char*)((uint8_t*)img->public.Data + function_table[ordinal]);

            return ResolveForward(redir_name, Address);
        }
    }

    *Address = (uint64_t)(uintptr_t)((uint8_t*)img->va + function_table[ordinal]);

    return EFI_SUCCESS;
}

static UINT32 EFIAPI get_characteristics(EFI_PE_IMAGE* This) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    return nt_header->FileHeader.Characteristics;
}

static EFI_STATUS EFIAPI get_sections(EFI_PE_IMAGE* This, IMAGE_SECTION_HEADER** Sections, UINTN* NumberOfSections) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    *Sections = (IMAGE_SECTION_HEADER*)((uint8_t*)&nt_header->OptionalHeader32 + nt_header->FileHeader.SizeOfOptionalHeader);
    *NumberOfSections = nt_header->FileHeader.NumberOfSections;

    return EFI_SUCCESS;
}

static EFI_STATUS relocate(EFI_PE_IMAGE* This, EFI_VIRTUAL_ADDRESS Address) {
    pe_image* img = _CR(This, pe_image, public);
    IMAGE_DOS_HEADER* dos_header;
    IMAGE_NT_HEADERS* nt_header;
    uint64_t old_va, base;

    dos_header = (IMAGE_DOS_HEADER*)img->public.Data;
    nt_header = (IMAGE_NT_HEADERS*)((uint8_t*)img->public.Data + dos_header->e_lfanew);

    old_va = (uintptr_t)img->va;

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        base = nt_header->OptionalHeader64.ImageBase;
    else
        base = nt_header->OptionalHeader32.ImageBase;

    img->va = (void*)(uintptr_t)(Address - old_va + base); // because do_relocations works on offsets
    do_relocations(img, nt_header);

    img->va = (void*)(uintptr_t)Address;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Load(EFI_FILE_HANDLE File, void* VirtualAddress, EFI_PE_IMAGE** Image) {
    EFI_STATUS Status;
    EFI_FILE_INFO file_info;
    pe_image* img;
    size_t file_size, pages;
    EFI_PHYSICAL_ADDRESS addr;
    uint8_t* data;
    IMAGE_NT_HEADERS* nt_header;
    IMAGE_SECTION_HEADER* sections;

    Status = bs->AllocatePool(EfiLoaderData, sizeof(pe_image), (void**)&img);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    img->public.Data = NULL;

    {
        EFI_GUID guid = EFI_FILE_INFO_ID;
        UINTN size = sizeof(EFI_FILE_INFO);

        Status = File->GetInfo(File, &guid, &size, &file_info);

        if (Status == EFI_BUFFER_TOO_SMALL) {
            EFI_FILE_INFO* file_info2;

            Status = bs->AllocatePool(EfiLoaderData, size, (void**)&file_info2);
            if (EFI_ERROR(Status)) {
                print_error("AllocatePool", Status);
                bs->FreePool(img);
                return Status;
            }

            Status = File->GetInfo(File, &guid, &size, file_info2);
            if (EFI_ERROR(Status)) {
                print_error("File->GetInfo", Status);
                bs->FreePool(file_info2);
                bs->FreePool(img);
                return Status;
            }

            file_size = file_info2->FileSize;

            bs->FreePool(file_info2);
        } else if (EFI_ERROR(Status)) {
            print_error("File->GetInfo", Status);
            bs->FreePool(img);
            return Status;
        } else
            file_size = file_info.FileSize;
    }

    pages = file_size / EFI_PAGE_SIZE;
    if (file_size % EFI_PAGE_SIZE != 0)
        pages++;

    if (pages == 0) {
        bs->FreePool(img);
        return EFI_INVALID_PARAMETER;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePages", Status);
        bs->FreePool(img);
        return Status;
    }

    data = (uint8_t*)(uintptr_t)addr;

    {
        UINTN read_size = pages * EFI_PAGE_SIZE;

        Status = File->Read(File, &read_size, data);
        if (EFI_ERROR(Status)) {
            print_error("File->Read", Status);
            bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)data, pages);
            bs->FreePool(img);
            return Status;
        }
    }

    if (!check_header(data, file_size, &nt_header)) {
        print_string("Header check failed.\n");
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)data, pages);
        bs->FreePool(img);
        return EFI_INVALID_PARAMETER;
    }

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        img->size = nt_header->OptionalHeader64.SizeOfImage;
    else
        img->size = nt_header->OptionalHeader32.SizeOfImage;

    img->pages = img->size / EFI_PAGE_SIZE;
    if ((img->size % EFI_PAGE_SIZE) != 0)
        img->pages++;

    if (img->pages == 0) {
        print_string("Image size was 0.\n");
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)data, pages);
        bs->FreePool(img);
        return EFI_INVALID_PARAMETER;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, img->pages, &addr);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePages", Status);
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)data, pages);
        bs->FreePool(img);
        return Status;
    }

    img->public.Data = (uint8_t*)(uintptr_t)addr;

    if (VirtualAddress)
        img->va = VirtualAddress;
    else // if VirtualAddress not set, use physical address
        img->va = (void*)(uintptr_t)addr;

    if (nt_header->OptionalHeader32.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        sections = (IMAGE_SECTION_HEADER*)((uint8_t*)&nt_header->OptionalHeader64 + nt_header->FileHeader.SizeOfOptionalHeader);

        // copy header
        memcpy(img->public.Data, data, nt_header->OptionalHeader64.SizeOfHeaders);
    } else {
        sections = (IMAGE_SECTION_HEADER*)((uint8_t*)&nt_header->OptionalHeader32 + nt_header->FileHeader.SizeOfOptionalHeader);

        // copy header
        memcpy(img->public.Data, data, nt_header->OptionalHeader32.SizeOfHeaders);
    }

    for (unsigned int i = 0; i < nt_header->FileHeader.NumberOfSections; i++) {
        uint32_t section_size;

        // FIXME - check no overruns

        section_size = sections[i].VirtualSize;

        if (sections[i].SizeOfRawData < section_size)
            section_size = sections[i].SizeOfRawData;

        if (section_size > 0 && sections[i].PointerToRawData != 0)
            memcpy((uint8_t*)img->public.Data + sections[i].VirtualAddress, data + sections[i].PointerToRawData, section_size);

        if (section_size < sections[i].VirtualSize) // if short, pad with zeroes
            memset((uint8_t*)img->public.Data + sections[i].VirtualAddress + section_size, 0, sections[i].VirtualSize - section_size);
    }

    do_relocations(img, nt_header);

    randomize_security_cookie(img, nt_header);

    bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)data, pages);

    img->public.Free = free_image;
    img->public.GetEntryPoint = get_entry_point;
    img->public.ListImports = list_imports;
    img->public.GetAddress = get_address;
    img->public.GetSize = get_size;
    img->public.ResolveImports = resolve_imports;
    img->public.GetCheckSum = get_checksum;
    img->public.GetDllCharacteristics = get_dll_characteristics;
    img->public.MoveAddress = move_address;
    img->public.GetVersion = get_version;
    img->public.FindExport = find_export;
    img->public.GetCharacteristics = get_characteristics;
    img->public.GetSections = get_sections;
    img->public.Relocate = relocate;

    *Image = &img->public;

    return EFI_SUCCESS;
}
