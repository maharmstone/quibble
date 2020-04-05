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

#include <string.h>
#include "quibble.h"
#include "misc.h"
#include "peload.h"
#include "x86.h"

void* apiset;
unsigned int apisetsize;
void* apisetva;

EFI_STATUS load_api_set(EFI_BOOT_SERVICES* bs, LIST_ENTRY* images, EFI_PE_LOADER_PROTOCOL* pe, EFI_FILE_HANDLE dir,
                        void** va, uint16_t version, LIST_ENTRY* mappings, command_line* cmdline) {
    EFI_STATUS Status;
    IMAGE_SECTION_HEADER* sections;
    UINTN num_sections;
    EFI_PE_IMAGE* dll;

    if (version == _WIN32_WINNT_WIN8) {
        image* img;
        uint32_t size;

        Status = add_image(bs, images, L"ApiSetSchema.dll", LoaderSystemCode, L"system32", false, NULL, 0, false);
        if (EFI_ERROR(Status)) {
            print_error(L"add_image", Status);
            return Status;
        }

        img = _CR(images->Blink, image, list_entry); // get last item

        Status = load_image(img, L"ApiSetSchema.dll", pe, *va, dir, cmdline, 0);
        if (EFI_ERROR(Status)) {
            print_error(L"load_image", Status);
            return Status;
        }

        dll = img->img;

        size = dll->GetSize(dll);

        if ((size % EFI_PAGE_SIZE) != 0)
            size = ((size / EFI_PAGE_SIZE) + 1) * EFI_PAGE_SIZE;

        *va = (uint8_t*)*va + size;
    } else { // only passed to NT as an image on Windows 8
        EFI_FILE_HANDLE file;

        Status = open_file(dir, &file, L"ApiSetSchema.dll");
        if (EFI_ERROR(Status)) {
            print(L"Loading of ApiSetSchema.dll failed.\r\n");
            print_error(L"file open", Status);
            return Status;
        }

        Status = pe->Load(file, NULL, &dll);
        if (EFI_ERROR(Status)) {
            print_error(L"PE load", Status);
            file->Close(file);
            return Status;
        }

        file->Close(file);
    }

    Status = dll->GetSections(dll, &sections, &num_sections);
    if (EFI_ERROR(Status)) {
        print_error(L"GetSections", Status);
        return Status;
    }

    apiset = NULL;

    for (unsigned int i = 0; i < num_sections; i++) {
        if (!strcmp(sections[i].Name, ".apiset")) {
            if (sections[i].VirtualSize == 0) {
                print(L".apiset section size was 0.\r\n");
                return EFI_INVALID_PARAMETER;
            }

            apiset = (uint8_t*)dll->Data + sections[i].VirtualAddress;
            apisetsize = sections[i].VirtualSize;

            break;
        }
    }

    if (!apiset) {
        print(L"Could not find .apiset section in ApiSetSchema.dll.\r\n");
        return EFI_NOT_FOUND;
    }

    if (version >= _WIN32_WINNT_WINBLUE) {
        EFI_PHYSICAL_ADDRESS addr;
        void* newapiset;

        Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(apisetsize), &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            return Status;
        }

        newapiset = (void*)(uintptr_t)addr;

        memcpy(newapiset, apiset, apisetsize);

        apiset = newapiset;

        apisetva = *va;

        Status = add_mapping(bs, mappings, *va, apiset, PAGE_COUNT(apisetsize), LoaderSystemBlock);
        if (EFI_ERROR(Status)) {
            print_error(L"add_mapping", Status);
            return Status;
        }

        *va = (uint8_t*)*va + (PAGE_COUNT(apisetsize) * EFI_PAGE_SIZE);

        dll->Free(dll);
    }

    return EFI_SUCCESS;
}

static bool search_api_set_80(WCHAR* dll, WCHAR* newname) {
    API_SET_NAMESPACE_ARRAY_80* arr = (API_SET_NAMESPACE_ARRAY_80*)apiset;
    WCHAR n[MAX_PATH];
    unsigned int len = 0;

    {
        WCHAR* s = &dll[4];
        WCHAR* o = n;

        while (s) {
            if (*s == '.')
                break;
            else if (*s >= 'A' && *s <= 'Z')
                *o = *s - 'A' + 'a';
            else
                *o = *s;

            s++;
            o++;
            len++;
        }
    }

    for (unsigned int i = 0; i < arr->Count; i++) {
        WCHAR* name = (WCHAR*)((uint8_t*)apiset + arr->Array[i].NameOffset);
        bool found = true;
        API_SET_VALUE_ARRAY_80* val;

        if (arr->Array[i].NameLength != len * sizeof(WCHAR))
            continue;

        for (unsigned int j = 0; j < len; j++) {
            if (name[j] >= 'A' && name[j] <= 'Z') {
                if (name[j] - 'A' + 'a' != n[j]) {
                    found = false;
                    break;
                }
            } else if (name[j] != n[j]) {
                found = false;
                break;
            }
        }

        if (!found)
            continue;

        val = (API_SET_VALUE_ARRAY_80*)((uint8_t*)apiset + arr->Array[i].DataOffset);

        if (val->Count == 0)
            return false;

        for (unsigned int j = 0; j < val->Count; j++) {
            if (val->Array[j].ValueLength > 0) {
                memcpy(newname, (uint8_t*)apiset + val->Array[j].ValueOffset, val->Array[j].ValueLength);
                newname[val->Array[j].ValueLength / sizeof(WCHAR)] = 0;
                return true;
            }
        }

        return false;
    }

    print(dll);
    print(L" not found in API set array.\r\n");

    return false;
}

static bool search_api_set_81(WCHAR* dll, WCHAR* newname) {
    API_SET_NAMESPACE_ARRAY_81* arr = (API_SET_NAMESPACE_ARRAY_81*)apiset;
    WCHAR n[MAX_PATH];
    unsigned int len = 0;

    {
        WCHAR* s = &dll[4];
        WCHAR* o = n;

        while (s) {
            if (*s == '.')
                break;
            else if (*s >= 'A' && *s <= 'Z')
                *o = *s - 'A' + 'a';
            else
                *o = *s;

            s++;
            o++;
            len++;
        }
    }

    for (unsigned int i = 0; i < arr->Count; i++) {
        WCHAR* name = (WCHAR*)((uint8_t*)apiset + arr->Array[i].NameOffset);
        bool found = true;
        API_SET_VALUE_ARRAY_81* val;

        if (arr->Array[i].NameLength != len * sizeof(WCHAR))
            continue;

        for (unsigned int j = 0; j < len; j++) {
            if (name[j] >= 'A' && name[j] <= 'Z') {
                if (name[j] - 'A' + 'a' != n[j]) {
                    found = false;
                    break;
                }
            } else if (name[j] != n[j]) {
                found = false;
                break;
            }
        }

        if (!found)
            continue;

        val = (API_SET_VALUE_ARRAY_81*)((uint8_t*)apiset + arr->Array[i].DataOffset);

        if (val->Count == 0)
            return false;

        for (unsigned int j = 0; j < val->Count; j++) {
            if (val->Array[j].ValueLength > 0) {
                memcpy(newname, (uint8_t*)apiset + val->Array[j].ValueOffset, val->Array[j].ValueLength);
                newname[val->Array[j].ValueLength / sizeof(WCHAR)] = 0;
                return true;
            }
        }

        return false;
    }

    print(dll);
    print(L" not found in API set array.\r\n");

    return false;
}

static bool search_api_set_10(WCHAR* dll, WCHAR* newname) {
    API_SET_NAMESPACE_HEADER_10* header = (API_SET_NAMESPACE_HEADER_10*)apiset;
    API_SET_NAMESPACE_ENTRY_10* arr = (API_SET_NAMESPACE_ENTRY_10*)((uint8_t*)apiset + header->ArrayOffset);
    WCHAR n[MAX_PATH];
    unsigned int len = 0;

    {
        WCHAR* s = dll;
        WCHAR* o = n;

        while (s) {
            if (*s == '.')
                break;
            else if (*s >= 'A' && *s <= 'Z')
                *o = *s - 'A' + 'a';
            else
                *o = *s;

            s++;
            o++;
            len++;
        }
    }

    for (unsigned int i = 0; i < header->Count; i++) {
        WCHAR* name = (WCHAR*)((uint8_t*)apiset + arr[i].NameOffset);
        bool found = true;
        API_SET_VALUE_ENTRY_81* val;

        if (arr[i].NameLength != len * sizeof(WCHAR))
            continue;

        for (unsigned int j = 0; j < len; j++) {
            if (name[j] >= 'A' && name[j] <= 'Z') {
                if (name[j] - 'A' + 'a' != n[j]) {
                    found = false;
                    break;
                }
            } else if (name[j] != n[j]) {
                found = false;
                break;
            }
        }

        if (!found)
            continue;

        if (arr[i].NumberOfHosts == 0)
            return false;

        val = (API_SET_VALUE_ENTRY_81*)((uint8_t*)apiset + arr[i].HostsOffset);

        for (unsigned int j = 0; j < arr[i].NumberOfHosts; j++) {
            if (val[j].ValueLength > 0) {
                memcpy(newname, (uint8_t*)apiset + val[j].ValueOffset, val[j].ValueLength);
                newname[val[j].ValueLength / sizeof(WCHAR)] = 0;
                return true;
            }
        }

        return false;
    }

    print(dll);
    print(L" not found in API set array.\r\n");

    return false;
}

bool search_api_set(WCHAR* dll, WCHAR* newname, uint16_t version) {
    if (version == _WIN32_WINNT_WIN8)
        return search_api_set_80(dll, newname);
    else if (version == _WIN32_WINNT_WINBLUE)
        return search_api_set_81(dll, newname);
    else if (version == _WIN32_WINNT_WIN10)
        return search_api_set_10(dll, newname);
    else
        return false;
}
