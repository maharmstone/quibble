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

#include <stdbool.h>
#include <string.h>
#include "reg.h"
#include "misc.h"
#include "winreg.h"
#include "print.h"

typedef struct {
    EFI_REGISTRY_HIVE public;
    size_t size;
    UINTN pages;
    void* data;
} hive;

static EFI_HANDLE reg_handle = NULL;
static EFI_REGISTRY_PROTOCOL proto;
static EFI_BOOT_SERVICES* bs;

static EFI_STATUS EFIAPI OpenHive(EFI_FILE_HANDLE File, EFI_REGISTRY_HIVE** Hive);

EFI_STATUS reg_register(EFI_BOOT_SERVICES* BootServices) {
    EFI_GUID reg_guid = WINDOWS_REGISTRY_PROTOCOL;

    proto.OpenHive = OpenHive;

    bs = BootServices;

    return bs->InstallProtocolInterface(&reg_handle, &reg_guid, EFI_NATIVE_INTERFACE, &proto);
}

EFI_STATUS reg_unregister() {
    EFI_GUID reg_guid = WINDOWS_REGISTRY_PROTOCOL;

    return bs->UninstallProtocolInterface(&reg_handle, &reg_guid, EFI_NATIVE_INTERFACE);
}

static bool check_header(hive* h) {
    HBASE_BLOCK* base_block = (HBASE_BLOCK*)h->data;
    uint32_t csum;
    bool dirty = false;

    if (base_block->Signature != HV_HBLOCK_SIGNATURE) {
        print_string("Invalid signature.\n");
        return false;
    }

    if (base_block->Major != HSYS_MAJOR) {
        print_string("Invalid major value.\n");
        return false;
    }

    if (base_block->Minor < HSYS_MINOR) {
        print_string("Invalid minor value.\n");
        return false;
    }

    if (base_block->Type != HFILE_TYPE_PRIMARY) {
        print_string("Type was not HFILE_TYPE_PRIMARY.\n");
        return false;
    }

    if (base_block->Format != HBASE_FORMAT_MEMORY) {
        print_string("Format was not HBASE_FORMAT_MEMORY.\n");
        return false;
    }

    if (base_block->Cluster != 1) {
        print_string("Cluster was not 1.\n");
        return false;
    }

    if (base_block->Sequence1 != base_block->Sequence2) {
        print_string("Sequence1 != Sequence2.\n");
        base_block->Sequence2 = base_block->Sequence1;
        dirty = true;
    }

    // check checksum

    csum = 0;

    for (unsigned int i = 0; i < 127; i++) {
        csum ^= ((uint32_t*)h->data)[i];
    }

    if (csum == 0xffffffff)
        csum = 0xfffffffe;
    else if (csum == 0)
        csum = 1;

    if (csum != base_block->CheckSum) {
        print_string("Invalid checksum.\n");
        base_block->CheckSum = csum;
        dirty = true;
    }

    if (dirty) {
        print_string("Hive is dirty.\n");

        // FIXME - recover by processing LOG files (old style, < Windows 8.1)
        // FIXME - recover by processing LOG files (new style, >= Windows 8.1)
    }

    return true;
}

static EFI_STATUS EFIAPI close_hive(EFI_REGISTRY_HIVE* This) {
    hive* h = _CR(This, hive, public);

    if (h->data)
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)h->data, h->pages);

    bs->FreePool(h);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI find_root(EFI_REGISTRY_HIVE* This, HKEY* Key) {
    hive* h = _CR(This, hive, public);
    HBASE_BLOCK* base_block = (HBASE_BLOCK*)h->data;

    *Key = 0x1000 + base_block->RootCell;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI enum_keys(EFI_REGISTRY_HIVE* This, HKEY Key, UINT32 Index, WCHAR* Name, UINT32 NameLength) {
    hive* h = _CR(This, hive, public);
    int32_t size;
    CM_KEY_NODE* nk;
    CM_KEY_FAST_INDEX* lh;
    CM_KEY_NODE* nk2;
    bool overflow = false;

    // FIXME - make sure no buffer overruns (here and elsewhere)

    // find parent key node

    size = -*(int32_t*)((uint8_t*)h->data + Key);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
        return EFI_INVALID_PARAMETER;

    nk = (CM_KEY_NODE*)((uint8_t*)h->data + Key + sizeof(int32_t));

    if (nk->Signature != CM_KEY_NODE_SIGNATURE)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]) + nk->NameLength)
        return EFI_INVALID_PARAMETER;

    // FIXME - volatile keys?

    if (Index >= nk->SubKeyCount || nk->SubKeyList == 0xffffffff)
        return EFI_NOT_FOUND;

    // go to key index

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_FAST_INDEX, List[0]))
        return EFI_INVALID_PARAMETER;

    lh = (CM_KEY_FAST_INDEX*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList + sizeof(int32_t));

    if (lh->Signature != CM_KEY_HASH_LEAF && lh->Signature != CM_KEY_FAST_LEAF)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_FAST_INDEX, List[0]) + (lh->Count * sizeof(CM_INDEX)))
        return EFI_INVALID_PARAMETER;

    if (Index >= lh->Count)
        return EFI_INVALID_PARAMETER;

    // find child key node

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + lh->List[Index].Cell);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
        return EFI_INVALID_PARAMETER;

    nk2 = (CM_KEY_NODE*)((uint8_t*)h->data + 0x1000 + lh->List[Index].Cell + sizeof(int32_t));

    if (nk2->Signature != CM_KEY_NODE_SIGNATURE)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]) + nk2->NameLength)
        return EFI_INVALID_PARAMETER;

    if (nk2->Flags & KEY_COMP_NAME) {
        unsigned int i = 0;
        char* nkname = (char*)nk2->Name;

        for (i = 0; i < nk2->NameLength; i++) {
            if (i >= NameLength) {
                overflow = true;
                break;
            }

            Name[i] = nkname[i];
        }

        Name[i] = 0;
    } else {
        unsigned int i = 0;

        for (i = 0; i < nk2->NameLength / sizeof(WCHAR); i++) {
            if (i >= NameLength) {
                overflow = true;
                break;
            }

            Name[i] = nk2->Name[i];
        }

        Name[i] = 0;
    }

    return overflow ? EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
}

static EFI_STATUS find_child_key(hive* h, HKEY parent, const WCHAR* namebit, UINTN nblen, HKEY* key) {
    int32_t size;
    CM_KEY_NODE* nk;
    CM_KEY_FAST_INDEX* lh;

    // find parent key node

    size = -*(int32_t*)((uint8_t*)h->data + parent);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
        return EFI_INVALID_PARAMETER;

    nk = (CM_KEY_NODE*)((uint8_t*)h->data + parent + sizeof(int32_t));

    if (nk->Signature != CM_KEY_NODE_SIGNATURE)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]) + nk->NameLength)
        return EFI_INVALID_PARAMETER;

    if (nk->SubKeyCount == 0 || nk->SubKeyList == 0xffffffff)
        return EFI_NOT_FOUND;

    // go to key index

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_FAST_INDEX, List[0]))
        return EFI_INVALID_PARAMETER;

    lh = (CM_KEY_FAST_INDEX*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList + sizeof(int32_t));

    if (lh->Signature != CM_KEY_HASH_LEAF && lh->Signature != CM_KEY_FAST_LEAF)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_FAST_INDEX, List[0]) + (lh->Count * sizeof(CM_INDEX)))
        return EFI_INVALID_PARAMETER;

    // FIXME - check against hashes if CM_KEY_HASH_LEAF

    for (unsigned int i = 0; i < lh->Count; i++) {
        CM_KEY_NODE* nk2;

        size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + lh->List[i].Cell);

        if (size < 0)
            continue;

        if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
            continue;

        nk2 = (CM_KEY_NODE*)((uint8_t*)h->data + 0x1000 + lh->List[i].Cell + sizeof(int32_t));

        if (nk2->Signature != CM_KEY_NODE_SIGNATURE)
            continue;

        if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]) + nk2->NameLength)
            continue;

        // FIXME - use string protocol here to do comparison properly?

        if (nk2->Flags & KEY_COMP_NAME) {
            unsigned int j;
            char* name = (char*)nk2->Name;

            if (nk2->NameLength != nblen)
                continue;

            for (j = 0; j < nk2->NameLength; j++) {
                WCHAR c1 = name[j];
                WCHAR c2 = namebit[j];

                if (c1 >= 'A' && c1 <= 'Z')
                    c1 = c1 - 'A' + 'a';

                if (c2 >= 'A' && c2 <= 'Z')
                    c2 = c2 - 'A' + 'a';

                if (c1 != c2)
                    break;
            }

            if (j != nk2->NameLength)
                continue;

            *key = 0x1000 + lh->List[i].Cell;

            return EFI_SUCCESS;
        } else {
            unsigned int j;

            if (nk2->NameLength / sizeof(WCHAR) != nblen)
                continue;

            for (j = 0; j < nk2->NameLength / sizeof(WCHAR); j++) {
                WCHAR c1 = nk2->Name[j];
                WCHAR c2 = namebit[j];

                if (c1 >= 'A' && c1 <= 'Z')
                    c1 = c1 - 'A' + 'a';

                if (c2 >= 'A' && c2 <= 'Z')
                    c2 = c2 - 'A' + 'a';

                if (c1 != c2)
                    break;
            }

            if (j != nk2->NameLength / sizeof(WCHAR))
                continue;

            *key = 0x1000 + lh->List[i].Cell;

            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI find_key(EFI_REGISTRY_HIVE* This, HKEY Parent, const WCHAR* Path, HKEY* Key) {
    EFI_STATUS Status;
    hive* h = _CR(This, hive, public);
    UINTN nblen;
    HKEY k;

    do {
        nblen = 0;
        while (Path[nblen] != '\\' && Path[nblen] != 0) {
            nblen++;
        }

        Status = find_child_key(h, Parent, Path, nblen, &k);
        if (EFI_ERROR(Status))
            return Status;

        if (Path[nblen] == 0 || (Path[nblen] == '\\' && Path[nblen + 1] == 0)) {
            *Key = k;
            return Status;
        }

        Parent = k;
        Path = &Path[nblen + 1];
    } while (true);
}

static EFI_STATUS EFIAPI enum_values(EFI_REGISTRY_HIVE* This, HKEY Key, UINT32 Index, WCHAR* Name, UINT32 NameLength, UINT32* Type) {
    hive* h = _CR(This, hive, public);
    int32_t size;
    CM_KEY_NODE* nk;
    uint32_t* list;
    CM_KEY_VALUE* vk;
    bool overflow = false;

    // find key node

    size = -*(int32_t*)((uint8_t*)h->data + Key);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
        return EFI_INVALID_PARAMETER;

    nk = (CM_KEY_NODE*)((uint8_t*)h->data + Key + sizeof(int32_t));

    if (nk->Signature != CM_KEY_NODE_SIGNATURE)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]) + nk->NameLength)
        return EFI_INVALID_PARAMETER;

    if (Index >= nk->ValuesCount || nk->Values == 0xffffffff)
        return EFI_NOT_FOUND;

    // go to key index

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + nk->Values);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + (sizeof(uint32_t) * nk->ValuesCount))
        return EFI_INVALID_PARAMETER;

    list = (uint32_t*)((uint8_t*)h->data + 0x1000 + nk->Values + sizeof(int32_t));

    // find value node

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + list[Index]);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_VALUE, Name[0]))
        return EFI_INVALID_PARAMETER;

    vk = (CM_KEY_VALUE*)((uint8_t*)h->data + 0x1000 + list[Index] + sizeof(int32_t));

    if (vk->Signature != CM_KEY_VALUE_SIGNATURE)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_VALUE, Name[0]) + vk->NameLength)
        return EFI_INVALID_PARAMETER;

    if (vk->Flags & VALUE_COMP_NAME) {
        unsigned int i = 0;
        char* nkname = (char*)vk->Name;

        for (i = 0; i < vk->NameLength; i++) {
            if (i >= NameLength) {
                overflow = true;
                break;
            }

            Name[i] = nkname[i];
        }

        Name[i] = 0;
    } else {
        unsigned int i = 0;

        for (i = 0; i < vk->NameLength / sizeof(WCHAR); i++) {
            if (i >= NameLength) {
                overflow = true;
                break;
            }

            Name[i] = vk->Name[i];
        }

        Name[i] = 0;
    }

    *Type = vk->Type;

    return overflow ? EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI query_value_no_copy(EFI_REGISTRY_HIVE* This, HKEY Key, const WCHAR* Name, void** Data,
                                             UINT32* DataLength, UINT32* Type) {
    hive* h = _CR(This, hive, public);
    int32_t size;
    CM_KEY_NODE* nk;
    uint32_t* list;
    unsigned int namelen = wcslen(Name);

    // find key node

    size = -*(int32_t*)((uint8_t*)h->data + Key);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
        return EFI_INVALID_PARAMETER;

    nk = (CM_KEY_NODE*)((uint8_t*)h->data + Key + sizeof(int32_t));

    if (nk->Signature != CM_KEY_NODE_SIGNATURE)
        return EFI_INVALID_PARAMETER;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]) + nk->NameLength)
        return EFI_INVALID_PARAMETER;

    if (nk->ValuesCount == 0 || nk->Values == 0xffffffff)
        return EFI_NOT_FOUND;

    // go to key index

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + nk->Values);

    if (size < 0)
        return EFI_NOT_FOUND;

    if ((uint32_t)size < sizeof(int32_t) + (sizeof(uint32_t) * nk->ValuesCount))
        return EFI_INVALID_PARAMETER;

    list = (uint32_t*)((uint8_t*)h->data + 0x1000 + nk->Values + sizeof(int32_t));

    // find value node

    for (unsigned int i = 0; i < nk->ValuesCount; i++) {
        CM_KEY_VALUE* vk;

        size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + list[i]);

        if (size < 0)
            continue;

        if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_VALUE, Name[0]))
            continue;

        vk = (CM_KEY_VALUE*)((uint8_t*)h->data + 0x1000 + list[i] + sizeof(int32_t));

        if (vk->Signature != CM_KEY_VALUE_SIGNATURE)
            continue;

        if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_VALUE, Name[0]) + vk->NameLength)
            continue;

        if (vk->Flags & VALUE_COMP_NAME) {
            unsigned int j;
            char* valname = (char*)vk->Name;

            if (vk->NameLength != namelen)
                continue;

            for (j = 0; j < vk->NameLength; j++) {
                WCHAR c1 = valname[j];
                WCHAR c2 = Name[j];

                if (c1 >= 'A' && c1 <= 'Z')
                    c1 = c1 - 'A' + 'a';

                if (c2 >= 'A' && c2 <= 'Z')
                    c2 = c2 - 'A' + 'a';

                if (c1 != c2)
                    break;
            }

            if (j != vk->NameLength)
                continue;
        } else {
            unsigned int j;

            if (vk->NameLength / sizeof(WCHAR) != namelen)
                continue;

            for (j = 0; j < vk->NameLength / sizeof(WCHAR); j++) {
                WCHAR c1 = vk->Name[j];
                WCHAR c2 = Name[j];

                if (c1 >= 'A' && c1 <= 'Z')
                    c1 = c1 - 'A' + 'a';

                if (c2 >= 'A' && c2 <= 'Z')
                    c2 = c2 - 'A' + 'a';

                if (c1 != c2)
                    break;
            }

            if (j != vk->NameLength / sizeof(WCHAR))
                continue;
        }

        if (vk->DataLength & CM_KEY_VALUE_SPECIAL_SIZE) { // data stored as data offset
            size_t datalen = vk->DataLength & ~CM_KEY_VALUE_SPECIAL_SIZE;
            uint8_t* ptr;

            if (datalen == 4)
                ptr = (uint8_t*)&vk->Data;
            else if (datalen == 2)
                ptr = (uint8_t*)&vk->Data + 2;
            else if (datalen == 1)
                ptr = (uint8_t*)&vk->Data + 3;
            else if (datalen != 0)
                return EFI_INVALID_PARAMETER;

            *Data = ptr;
        } else {
            size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + vk->Data);

            if ((uint32_t)size < vk->DataLength)
                return EFI_INVALID_PARAMETER;

            *Data = (uint8_t*)h->data + 0x1000 + vk->Data + sizeof(int32_t);
        }

        // FIXME - handle long "data block" values

        *DataLength = vk->DataLength & ~CM_KEY_VALUE_SPECIAL_SIZE;
        *Type = vk->Type;

        return EFI_SUCCESS;
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI query_value(EFI_REGISTRY_HIVE* This, HKEY Key, const WCHAR* Name, void* Data,
                                     UINT32* DataLength, UINT32* Type) {
    EFI_STATUS Status;
    void* out;
    UINT32 len;

    Status = query_value_no_copy(This, Key, Name, &out, &len, Type);
    if (EFI_ERROR(Status))
        return Status;

    if (len > *DataLength) {
        memcpy(Data, out, *DataLength);
        *DataLength = len;
        return EFI_BUFFER_TOO_SMALL;
    }

    memcpy(Data, out, len);
    *DataLength = len;

    return EFI_SUCCESS;
}

static EFI_STATUS steal_data(EFI_REGISTRY_HIVE* This, void** Data, UINT32* Size) {
    hive* h = _CR(This, hive, public);

    *Data = h->data;
    *Size = h->size;

    h->data = NULL;
    h->size = 0;

    return EFI_SUCCESS;
}

static void clear_volatile(hive* h, HKEY key) {
    int32_t size;
    CM_KEY_NODE* nk;
    uint16_t sig;

    size = -*(int32_t*)((uint8_t*)h->data + key);

    if (size < 0)
        return;

    if ((uint32_t)size < sizeof(int32_t) + offsetof(CM_KEY_NODE, Name[0]))
        return;

    nk = (CM_KEY_NODE*)((uint8_t*)h->data + key + sizeof(int32_t));

    if (nk->Signature != CM_KEY_NODE_SIGNATURE)
        return;

    nk->VolatileSubKeyList = 0xbaadf00d;
    nk->VolatileSubKeyCount = 0;

    if (nk->SubKeyCount == 0 || nk->SubKeyList == 0xffffffff)
        return;

    size = -*(int32_t*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList);

    sig = *(uint16_t*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList + sizeof(int32_t));

    if (sig == CM_KEY_HASH_LEAF || sig == CM_KEY_FAST_LEAF) {
        CM_KEY_FAST_INDEX* lh = (CM_KEY_FAST_INDEX*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList + sizeof(int32_t));

        for (unsigned int i = 0; i < lh->Count; i++) {
            clear_volatile(h, 0x1000 + lh->List[i].Cell);
        }
    } else if (sig == CM_KEY_INDEX_ROOT) {
        CM_KEY_INDEX* ri = (CM_KEY_INDEX*)((uint8_t*)h->data + 0x1000 + nk->SubKeyList + sizeof(int32_t));

        for (unsigned int i = 0; i < ri->Count; i++) {
            clear_volatile(h, 0x1000 + ri->List[i]);
        }
    } else {
        char s[255], *p;

        p = stpcpy(s, "Unhandled registry signature ");
        p = hex_to_str(p, sig);
        p = stpcpy(p, ".\n");

        print_string(s);
    }
}

static EFI_STATUS EFIAPI OpenHive(EFI_FILE_HANDLE File, EFI_REGISTRY_HIVE** Hive) {
    EFI_STATUS Status;
    EFI_FILE_INFO file_info;
    hive* h;
    EFI_PHYSICAL_ADDRESS addr;

    Status = bs->AllocatePool(EfiLoaderData, sizeof(hive), (void**)&h);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    {
        EFI_GUID guid = EFI_FILE_INFO_ID;
        UINTN size = sizeof(EFI_FILE_INFO);

        Status = File->GetInfo(File, &guid, &size, &file_info);

        if (Status == EFI_BUFFER_TOO_SMALL) {
            EFI_FILE_INFO* file_info2;

            Status = bs->AllocatePool(EfiLoaderData, size, (void**)&file_info2);
            if (EFI_ERROR(Status)) {
                print_error("AllocatePool", Status);
                bs->FreePool(h);
                return Status;
            }

            Status = File->GetInfo(File, &guid, &size, file_info2);
            if (EFI_ERROR(Status)) {
                print_error("File->GetInfo", Status);
                bs->FreePool(file_info2);
                bs->FreePool(h);
                return Status;
            }

            h->size = file_info2->FileSize;

            bs->FreePool(file_info2);
        } else if (EFI_ERROR(Status)) {
            print_error("File->GetInfo", Status);
            bs->FreePool(h);
            return Status;
        } else
            h->size = file_info.FileSize;
    }

    h->pages = h->size / EFI_PAGE_SIZE;
    if (h->size % EFI_PAGE_SIZE != 0)
        h->pages++;

    if (h->pages == 0) {
        bs->FreePool(h);
        return EFI_INVALID_PARAMETER;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, h->pages, &addr);

    h->data = (void*)(uintptr_t)addr;

    {
        UINTN read_size = h->pages * EFI_PAGE_SIZE;

        Status = File->Read(File, &read_size, h->data);
        if (EFI_ERROR(Status)) {
            print_error("File->Read", Status);
            bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)h->data, h->pages);
            bs->FreePool(h);
            return Status;
        }
    }

    if (!check_header(h)) {
        print_string("Header check failed.\n");
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)h->data, h->pages);
        bs->FreePool(h);
        return EFI_INVALID_PARAMETER;
    }

    clear_volatile(h, 0x1000 + ((HBASE_BLOCK*)h->data)->RootCell);

    h->public.Close = close_hive;
    h->public.FindRoot = find_root;
    h->public.EnumKeys = enum_keys;
    h->public.FindKey = find_key;
    h->public.EnumValues = enum_values;
    h->public.QueryValue = query_value;
    h->public.StealData = steal_data;
    h->public.QueryValueNoCopy = query_value_no_copy;

    *Hive = &h->public;

    return EFI_SUCCESS;
}
