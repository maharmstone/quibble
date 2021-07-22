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
#include <efibind.h>
#include <efidef.h>
#include <efidevp.h>
#include <efiprot.h>
#include <eficon.h>
#include <efiapi.h>
#include <efigpt.h>
#include <efilink.h>
#include <efipciio.h>
#include <pci22.h>
#include "x86.h"
#include "misc.h"
#include "quibble.h"
#include "print.h"

#pragma pack(push,1)

typedef struct {
    uint8_t space_descriptor;
    uint16_t length;
    uint8_t resource_type;
    uint8_t general_flags;
    uint8_t type_specific_flags;
    uint64_t granularity;
    uint64_t address_minimum;
    uint64_t address_maximum;
    uint64_t translation_offset;
    uint64_t address_length;
} pci_bar_info;

#pragma pack(pop)

LIST_ENTRY block_devices;

static EFI_STATUS add_ccd(EFI_BOOT_SERVICES* bs, CONFIGURATION_COMPONENT_DATA* parent, CONFIGURATION_CLASS class,
                          CONFIGURATION_TYPE type, IDENTIFIER_FLAG flags, uint32_t key, uint32_t affinity,
                          char* identifier_string, CM_PARTIAL_RESOURCE_LIST* resource_list, uint32_t resource_list_size,
                          void** va, LIST_ENTRY* mappings, CONFIGURATION_COMPONENT_DATA** pccd) {
    EFI_STATUS Status;
    CONFIGURATION_COMPONENT_DATA* ccd;
    size_t size, identifier_length;
    EFI_PHYSICAL_ADDRESS addr;

    size = sizeof(CONFIGURATION_COMPONENT_DATA);

    if (identifier_string) {
        identifier_length = strlen(identifier_string) + 1;
        size += identifier_length;
    } else
        identifier_length = 0;

    if (resource_list)
        size += resource_list_size;

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(size), &addr);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePages", Status);
        return Status;
    }

    ccd = (CONFIGURATION_COMPONENT_DATA*)(uintptr_t)addr;

    memset(ccd, 0, sizeof(CONFIGURATION_COMPONENT_DATA));

    ccd->Parent = parent;
    ccd->ComponentEntry.Class = class;
    ccd->ComponentEntry.Type = type;

    if (identifier_string) {
        ccd->ComponentEntry.IdentifierLength = identifier_length;
        ccd->ComponentEntry.Identifier = (char*)((uint8_t*)ccd + sizeof(CONFIGURATION_COMPONENT_DATA));

        memcpy(ccd->ComponentEntry.Identifier, identifier_string, ccd->ComponentEntry.IdentifierLength);
    }

    if (resource_list) {
        ccd->ConfigurationData = (uint8_t*)ccd + sizeof(CONFIGURATION_COMPONENT_DATA) + identifier_length;
        ccd->ComponentEntry.ConfigurationDataLength = resource_list_size;

        memcpy(ccd->ConfigurationData, resource_list, resource_list_size);
    }

    ccd->ComponentEntry.Flags = flags;
    ccd->ComponentEntry.Key = key;
    ccd->ComponentEntry.AffinityMask = affinity;

    if (parent) {
        if (parent->Child)
            ccd->Sibling = parent->Child;

        parent->Child = ccd;
    }

    Status = add_mapping(bs, mappings, *va, ccd, PAGE_COUNT(size), LoaderSystemBlock);
    if (EFI_ERROR(Status)) {
        print_error("add_mapping", Status);
        return Status;
    }

    *va = (uint8_t*)*va + (PAGE_COUNT(size) * EFI_PAGE_SIZE);

    if (pccd)
        *pccd = ccd;

    return EFI_SUCCESS;
}

static EFI_STATUS add_acpi_config_data(EFI_BOOT_SERVICES* bs, CONFIGURATION_COMPONENT_DATA* parent, void** va,
                                       LIST_ENTRY* mappings, uint16_t version) {
    EFI_STATUS Status;
    EFI_GUID acpi1_guid = ACPI_TABLE_GUID;
    EFI_GUID acpi2_guid = ACPI_20_TABLE_GUID;
    RSDP_DESCRIPTOR* rsdp = NULL;
    EFI_PHYSICAL_ADDRESS addr;
    unsigned int map_count;
    size_t table_size;
    CM_PARTIAL_RESOURCE_LIST* prl;
    ACPI_BIOS_DATA* abd;

    for (unsigned int i = 0; i < systable->NumberOfTableEntries; i++) {
        if (!memcmp(&systable->ConfigurationTable[i].VendorGuid, &acpi2_guid, sizeof(EFI_GUID))) {
            rsdp = systable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    if (!rsdp) {
        for (unsigned int i = 0; i < systable->NumberOfTableEntries; i++) {
            if (!memcmp(&systable->ConfigurationTable[i].VendorGuid, &acpi1_guid, sizeof(EFI_GUID))) {
                rsdp = systable->ConfigurationTable[i].VendorTable;
                break;
            }
        }
    }

    if (!rsdp)
        return EFI_SUCCESS;

    if (rsdp->revision == 0 || (rsdp->revision == 2 && version < _WIN32_WINNT_WINXP)) // ACPI 1.0
        addr = rsdp->rsdt_physical_address;
    else if (rsdp->revision == 2) // ACPI 2.0
        addr = rsdp->xsdt_physical_address;
    else {
        char s[255], *p;

        p = stpcpy(s, "Unrecognized ACPI revision ");
        p = hex_to_str(p, rsdp->revision);
        p = stpcpy(p, "\n");

        print_string(s);

        return EFI_SUCCESS;
    }

    {
        char s[255], *p;

        p = stpcpy(s, "ACPI table at ");
        p = hex_to_str(p, addr);
        p = stpcpy(p, "\n");

        print_string(s);
    }

    // FIXME - do we need to add table to memory descriptor list?

    map_count = 0; // FIXME

    // FIXME - get EFI memory map

    table_size = offsetof(ACPI_BIOS_DATA, MemoryMap[0]) + (map_count * sizeof(BIOS_MEMORY_MAP));

    Status = bs->AllocatePool(EfiLoaderData, sizeof(CM_PARTIAL_RESOURCE_LIST) + table_size, (void**)&prl);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    memset(prl, 0, sizeof(CM_PARTIAL_RESOURCE_LIST) + table_size);

    prl->Version = 0;
    prl->Revision = 0;
    prl->Count = 1;

    prl->PartialDescriptors[0].Type = CmResourceTypeDeviceSpecific;
    prl->PartialDescriptors[0].ShareDisposition = CmResourceShareUndetermined;
    prl->PartialDescriptors[0].u.DeviceSpecificData.DataSize = table_size;

    abd = (ACPI_BIOS_DATA*)&prl->PartialDescriptors[1];
    abd->RSDTAddress = addr;
    abd->Count = map_count;

    // FIXME - copy memory map into abd->MemoryMap

    Status = add_ccd(bs, parent, AdapterClass, MultiFunctionAdapter, 0, 0, 0xffffffff, "ACPI BIOS",
                     prl, sizeof(CM_PARTIAL_RESOURCE_LIST) + table_size, va, mappings, NULL);

    bs->FreePool(prl);

    return Status;
}

static EFI_STATUS create_system_key(EFI_BOOT_SERVICES* bs, CONFIGURATION_COMPONENT_DATA** system_key, void** va,
                                    LIST_ENTRY* mappings, EFI_HANDLE image_handle) {
    EFI_STATUS Status;
    EFI_GUID guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE* handles = NULL;
    unsigned int disk_count = 0;
    size_t size;
    UINTN count;
    CM_PARTIAL_RESOURCE_LIST* prl;
    CM_INT13_DRIVE_PARAMETER* params;

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);

    if (EFI_ERROR(Status))
        return Status;

    for (unsigned int i = 0; i < count; i++) {
        EFI_BLOCK_IO* io = NULL;

        Status = bs->OpenProtocol(handles[i], &guid, (void**)&io, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status))
            continue;

        if (!io->Media->LogicalPartition)
            disk_count++;

        bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
    }

    bs->FreePool(handles);

    size = sizeof(CM_PARTIAL_RESOURCE_LIST) + (sizeof(CM_INT13_DRIVE_PARAMETER) * disk_count);

    Status = bs->AllocatePool(EfiLoaderData, size, (void**)&prl);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    prl->Version = 0;
    prl->Revision = 0;
    prl->Count = 1;
    prl->PartialDescriptors[0].Type = CmResourceTypeDeviceSpecific;
    prl->PartialDescriptors[0].ShareDisposition = CmResourceShareUndetermined;
    prl->PartialDescriptors[0].Flags = 0;
    prl->PartialDescriptors[0].u.DeviceSpecificData.DataSize = sizeof(CM_INT13_DRIVE_PARAMETER) * disk_count;
    prl->PartialDescriptors[0].u.DeviceSpecificData.Reserved1 = 0;
    prl->PartialDescriptors[0].u.DeviceSpecificData.Reserved2 = 0;

    params = (CM_INT13_DRIVE_PARAMETER*)&prl->PartialDescriptors[1];

    for (unsigned int i = 0; i < disk_count; i++) {
        params[i].DriveSelect = 0;
        params[i].MaxCylinders = 0xffffffff;
        params[i].SectorsPerTrack = 0;
        params[i].MaxHeads = 0xffff;
        params[i].NumberDrives = disk_count;
    }

    Status = add_ccd(bs, NULL, SystemClass, MaximumType, 0, 0, 0xffffffff, NULL, prl, size,
                     va, mappings, system_key);
    if (EFI_ERROR(Status)) {
        print_error("add_ccd", Status);
        goto end;
    }

end:
    bs->FreePool(prl);

    return Status;
}

#ifdef _X86_
typedef struct {
    CM_PARTIAL_RESOURCE_LIST prl;
    PCI_REGISTRY_INFO reg_info;
} pci_resource_list;

static EFI_STATUS add_pci_config(EFI_BOOT_SERVICES* bs, CONFIGURATION_COMPONENT_DATA* parent, void** va, LIST_ENTRY* mappings) {
    EFI_STATUS Status;
    EFI_GUID guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count;
    pci_resource_list reslist;

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);

    if (EFI_ERROR(Status))
        return Status;

    bs->FreePool(handles);

    if (count == 0) {
        print_string("No PCI buses found (is this right?)\n");
        return EFI_SUCCESS;
    }

    reslist.prl.Version = 0;
    reslist.prl.Revision = 0;
    reslist.prl.Count = 1;

    reslist.prl.PartialDescriptors[0].Type = CmResourceTypeDeviceSpecific;
    reslist.prl.PartialDescriptors[0].ShareDisposition = CmResourceShareUndetermined;
    reslist.prl.PartialDescriptors[0].Flags = 0;
    reslist.prl.PartialDescriptors[0].u.DeviceSpecificData.DataSize = sizeof(PCI_REGISTRY_INFO);
    reslist.prl.PartialDescriptors[0].u.DeviceSpecificData.Reserved1 = 0;
    reslist.prl.PartialDescriptors[0].u.DeviceSpecificData.Reserved2 = 0;

    // FIXME - is it possible to get these values from EFI? These constants are taken from handle_1ab101 in seabios
    reslist.reg_info.MajorRevision = 0x02;
    reslist.reg_info.MinorRevision = 0x10;
    reslist.reg_info.NoBuses = count;
    reslist.reg_info.HardwareMechanism = 1;

    Status = add_ccd(bs, parent, AdapterClass, MultiFunctionAdapter, 0, 0, 0xffffffff, "PCI", &reslist.prl,
                     sizeof(reslist), va, mappings, NULL);
    if (EFI_ERROR(Status)) {
        print_error("add_ccd", Status);
        return Status;
    }

    return EFI_SUCCESS;
}
#endif

EFI_STATUS find_hardware(EFI_BOOT_SERVICES* bs, LOADER_BLOCK1C* block1, void** va, LIST_ENTRY* mappings,
                         EFI_HANDLE image_handle, uint16_t version) {
    EFI_STATUS Status;
    CONFIGURATION_COMPONENT_DATA* system_key;

    UNUSED(version);

    Status = create_system_key(bs, &system_key, va, mappings, image_handle);
    if (EFI_ERROR(Status)) {
        print_error("create_system_key", Status);
        return Status;
    }

    Status = add_acpi_config_data(bs, system_key, va, mappings, version);
    if (EFI_ERROR(Status)) {
        print_error("add_acpi_config_data", Status);
        return Status;
    }

#ifdef _X86_
    if (version < _WIN32_WINNT_WIN8) {
        Status = add_pci_config(bs, system_key, va, mappings);
        if (EFI_ERROR(Status)) {
            print_error("add_pci_config", Status);
            return Status;
        }
    }
#endif

    block1->ConfigurationRoot = system_key;

    return EFI_SUCCESS;
}

static EFI_STATUS found_block_device(EFI_BOOT_SERVICES* bs, EFI_BLOCK_IO* io, unsigned int disk_num, unsigned int part_num,
                                     EFI_DEVICE_PATH_PROTOCOL* device_path) {
    EFI_STATUS Status;
    MASTER_BOOT_RECORD* mbr;
    size_t mbr_size = sizeof(MASTER_BOOT_RECORD);
    block_device* bd;

    if (io->Media->BlockSize != 0 && (mbr_size % io->Media->BlockSize) != 0)
        mbr_size += io->Media->BlockSize - (mbr_size % io->Media->BlockSize);

    Status = bs->AllocatePool(EfiLoaderData, mbr_size, (void**)&mbr);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    Status = io->ReadBlocks(io, io->Media->MediaId, 0, mbr_size, mbr);
    if (EFI_ERROR(Status)) {
        print_error("io->ReadBlocks", Status);
        goto end;
    }

    Status = bs->AllocatePool(EfiLoaderData, sizeof(block_device), (void**)&bd);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        goto end;
    }

    memset(bd, 0, sizeof(block_device));

    bd->disk_num = disk_num;
    bd->part_num = part_num;
    bd->device_path = device_path;

    if (part_num == 0) {
        // FIXME - if neither MBR nor GPT, what should the values of Signature and CheckSum be? Does it matter?

        if (mbr->Signature == MBR_SIGNATURE) {
            bd->arc.Signature = *(uint32_t*)mbr->UniqueMbrSignature;
            bd->arc.ValidPartitionTable = true;

            for (unsigned int i = 0; i < 512 / sizeof(uint32_t); i++) {
                bd->arc.CheckSum += ((uint32_t*)mbr)[i];
            }

            bd->arc.CheckSum = ~bd->arc.CheckSum;
            bd->arc.CheckSum++;

            if (mbr->Partition[0].OSIndicator == 0xEE) { // GPT
                EFI_PARTITION_TABLE_HEADER* gpt;
                size_t gpt_size = sizeof(EFI_PARTITION_TABLE_HEADER);

                if (io->Media->BlockSize != 0 && (gpt_size % io->Media->BlockSize) != 0)
                    gpt_size += io->Media->BlockSize - (gpt_size % io->Media->BlockSize);

                Status = bs->AllocatePool(EfiLoaderData, gpt_size, (void**)&gpt);
                if (EFI_ERROR(Status)) {
                    print_error("AllocatePool", Status);
                    return Status;
                }

                Status = io->ReadBlocks(io, io->Media->MediaId, PRIMARY_PART_HEADER_LBA, gpt_size, gpt);
                if (EFI_ERROR(Status)) {
                    print_error("io->ReadBlocks", Status);
                    bs->FreePool(gpt);
                    goto end;
                }

                if (memcmp(&gpt->Header.Signature, EFI_PTAB_HEADER_ID, sizeof(EFI_PTAB_HEADER_ID) - 1)) {
                    print_string("GPT has invalid signature (expected \"EFI PART\")\n");
                    bs->FreePool(gpt);
                    Status = EFI_INVALID_PARAMETER;
                    goto end;
                }

                // FIXME - check gpt->Header.CRC32

                bd->arc.IsGpt = true;
                memcpy(bd->arc.GptSignature, &gpt->DiskGUID, sizeof(EFI_GUID));

                bs->FreePool(gpt);
            }
        }
    }

    InsertTailList(&block_devices, &bd->list_entry);

    Status = EFI_SUCCESS;

end:
    bs->FreePool(mbr);

    return Status;
}

static void int_to_string(char** addr, unsigned int n) {
    unsigned int digits;
    char* c;

    if (n == 0) {
        char* buf = *addr;

        *buf = '0';
        buf++;

        *addr = buf;
        return;
    }

    digits = 0;

    {
        unsigned int d = n;

        while (d != 0) {
            digits++;
            d /= 10;
        }
    }

    c = *addr + digits;
    *c = 0;
    c--;

    while (n != 0) {
        *c = (n % 10) + '0';
        c--;

        n /= 10;
    }

    *addr = *addr + digits;
}

static EFI_STATUS add_isa_key(EFI_BOOT_SERVICES* bs, CONFIGURATION_COMPONENT_DATA* parent, void** va,
                              LIST_ENTRY* mappings, CONFIGURATION_COMPONENT_DATA** ret) {
    return add_ccd(bs, parent, AdapterClass, MultiFunctionAdapter, 0, 0, 0xffffffff, "ISA", NULL, 0,
                   va, mappings, ret);
}

static EFI_STATUS add_disk_controller(EFI_BOOT_SERVICES* bs, CONFIGURATION_COMPONENT_DATA* parent, void** va,
                                      LIST_ENTRY* mappings, CONFIGURATION_COMPONENT_DATA** ret) {
    return add_ccd(bs, parent, ControllerClass, DiskController, 0, 0, 0xffffffff, NULL, NULL, 0,
                   va, mappings, ret);
}

EFI_STATUS find_disks(EFI_BOOT_SERVICES* bs, LIST_ENTRY* disk_sig_list, void** va, LIST_ENTRY* mappings,
                      CONFIGURATION_COMPONENT_DATA* system_key, bool new_disk_format) {
    EFI_STATUS Status;
    size_t disk_list_size;
    LIST_ENTRY* le;
    EFI_PHYSICAL_ADDRESS addr;
    void* pa;
    CONFIGURATION_COMPONENT_DATA* isakey;
    CONFIGURATION_COMPONENT_DATA* diskcon;

    static const char arc_prefix[] = "multi(0)disk(0)rdisk(";

    Status = add_isa_key(bs, system_key, va, mappings, &isakey);
    if (EFI_ERROR(Status)) {
        print_error("add_isa_key", Status);
        return Status;
    }

    Status = add_disk_controller(bs, isakey, va, mappings, &diskcon);
    if (EFI_ERROR(Status)) {
        print_error("add_disk_controller", Status);
        return Status;
    }

    if (IsListEmpty(&block_devices))
        return EFI_SUCCESS;

    disk_list_size = 0;

    le = block_devices.Flink;
    while (le != &block_devices) {
        block_device* bd = _CR(le, block_device, list_entry);

        if (bd->part_num == 0) {
            if (new_disk_format)
                disk_list_size += sizeof(ARC_DISK_SIGNATURE_WIN7);
            else
                disk_list_size += sizeof(ARC_DISK_SIGNATURE);

            disk_list_size += sizeof(arc_prefix) - 1 + 3 + 1 + 1;
        }

        le = le->Flink;
    }

    Status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(disk_list_size), &addr);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePages", Status);
        return Status;
    }

    pa = (void*)(uintptr_t)addr;

    le = block_devices.Flink;
    while (le != &block_devices) {
        block_device* bd = _CR(le, block_device, list_entry);

        if (bd->part_num == 0) {
            if (new_disk_format) {
                ARC_DISK_SIGNATURE_WIN7* arc = (ARC_DISK_SIGNATURE_WIN7*)pa;

                memset(arc, 0, sizeof(ARC_DISK_SIGNATURE_WIN7));

                arc->Signature = bd->arc.Signature;
                arc->CheckSum = bd->arc.CheckSum;
                arc->ValidPartitionTable = bd->arc.ValidPartitionTable;
                arc->xInt13 = bd->arc.xInt13;
                arc->IsGpt = bd->arc.IsGpt;
                arc->Reserved = 0;
                memcpy(arc->GptSignature, bd->arc.GptSignature, sizeof(arc->GptSignature));
                arc->unknown = 0;

                pa = (uint8_t*)pa + sizeof(ARC_DISK_SIGNATURE_WIN7);

                arc->ArcName = pa;

                memcpy(pa, arc_prefix, sizeof(arc_prefix) - 1);
                pa = (uint8_t*)pa + sizeof(arc_prefix) - 1;

                int_to_string((char**)&pa, bd->disk_num);

                *(char*)pa = ')';
                pa = (uint8_t*)pa + 1;

                *(char*)pa = 0;
                pa = (uint8_t*)pa + 1;

                InsertTailList(disk_sig_list, &arc->ListEntry);
            } else {
                ARC_DISK_SIGNATURE* arc = (ARC_DISK_SIGNATURE*)pa;

                memcpy(arc, &bd->arc, sizeof(ARC_DISK_SIGNATURE));
                pa = (uint8_t*)pa + sizeof(ARC_DISK_SIGNATURE);

                arc->ArcName = pa;

                memcpy(pa, arc_prefix, sizeof(arc_prefix) - 1);
                pa = (uint8_t*)pa + sizeof(arc_prefix) - 1;

                int_to_string((char**)&pa, bd->disk_num);

                *(char*)pa = ')';
                pa = (uint8_t*)pa + 1;

                *(char*)pa = 0;
                pa = (uint8_t*)pa + 1;

                InsertTailList(disk_sig_list, &arc->ListEntry);
            }
        }

        le = le->Flink;
    }

    Status = add_mapping(bs, mappings, *va, (void*)(uintptr_t)addr, PAGE_COUNT(disk_list_size), LoaderSystemBlock);
    if (EFI_ERROR(Status)) {
        print_error("add_mapping", Status);
        return Status;
    }

    *va = (uint8_t*)*va + (PAGE_COUNT(disk_list_size) * EFI_PAGE_SIZE);

    le = block_devices.Flink;
    while (le != &block_devices) {
        block_device* bd = _CR(le, block_device, list_entry);

        if (bd->part_num == 0) {
            uint32_t csum = bd->arc.CheckSum;
            uint32_t sig = bd->arc.Signature;
            char identifier[20];

            for (unsigned int i = 0; i < 4; i++) {
                if ((csum & 0xf0) >= 0xa0)
                    identifier[6 - (2*i)] = ((csum & 0xf0) >> 4) - 0xa + 'a';
                else
                    identifier[6 - (2*i)] = ((csum & 0xf0) >> 4) + '0';

                if ((csum & 0xf) >= 0xa)
                    identifier[7 - (2*i)] = (csum & 0xf) - 0xa + 'a';
                else
                    identifier[7 - (2*i)] = (csum & 0xf) + '0';

                csum >>= 8;
            }

            identifier[8] = '-';

            for (unsigned int i = 0; i < 4; i++) {
                if ((sig & 0xf0) >= 0xa0)
                    identifier[15 - (2*i)] = ((sig & 0xf0) >> 4) - 0xa + 'a';
                else
                    identifier[15 - (2*i)] = ((sig & 0xf0) >> 4) + '0';

                if ((sig & 0xf) >= 0xa)
                    identifier[16 - (2*i)] = (sig & 0xf) - 0xa + 'a';
                else
                    identifier[16 - (2*i)] = (sig & 0xf) + '0';

                sig >>= 8;
            }

            identifier[17] = '-';
            identifier[18] = bd->arc.ValidPartitionTable ? 'A' : 'X';
            identifier[19] = 0;

            // FIXME - put "geometry" into partial resource list?

            Status = add_ccd(bs, diskcon, PeripheralClass, DiskPeripheral, IdentifierFlag_Input | IdentifierFlag_Output,
                            0, 0xffffffff, identifier, NULL, 0, va, mappings, NULL);
            if (EFI_ERROR(Status)) {
                print_error("add_ccd", Status);
                return Status;
            }
        }

        le = le->Flink;
    }

    return EFI_SUCCESS;
}

static bool device_path_is_child(EFI_DEVICE_PATH_PROTOCOL* parent, EFI_DEVICE_PATH_PROTOCOL* child) {
    EFI_DEVICE_PATH_PROTOCOL* pbit = parent;
    EFI_DEVICE_PATH_PROTOCOL* cbit = child;

    do {
        unsigned int plen, clen;

        if (pbit->Type == END_DEVICE_PATH_TYPE)
            return true;

        plen = *(uint16_t*)pbit->Length;
        clen = *(uint16_t*)cbit->Length;

        if (plen != clen)
            return false;

        if (memcmp(pbit, cbit, plen))
            return false;

        pbit = (EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)pbit + *(uint16_t*)pbit->Length);
        cbit = (EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)cbit + *(uint16_t*)cbit->Length);
    } while (true);
}

static EFI_DEVICE_PATH_PROTOCOL* duplicate_device_path(EFI_DEVICE_PATH_PROTOCOL* device_path) {
    EFI_STATUS Status;
    unsigned int len = 0;
    EFI_DEVICE_PATH_PROTOCOL* dpbit = device_path;
    EFI_DEVICE_PATH_PROTOCOL* dp;

    do {
        len += *(uint16_t*)dpbit->Length;

        if (dpbit->Type == END_DEVICE_PATH_TYPE)
            break;

        dpbit = (EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)dpbit + *(uint16_t*)dpbit->Length);
    } while (true);

    if (len == 0)
        return NULL;

    Status = systable->BootServices->AllocatePool(EfiLoaderData, len, (void**)&dp);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return NULL;
    }

    memcpy(dp, device_path, len);

    return dp;
}

static unsigned int get_partition_number(EFI_DEVICE_PATH_PROTOCOL* device_path) {
    EFI_DEVICE_PATH_PROTOCOL* dpbit = device_path;

    do {
        if (dpbit->Type == END_DEVICE_PATH_TYPE)
            return 0;

        if (dpbit->Type == MEDIA_DEVICE_PATH && dpbit->SubType == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH* hddp = (HARDDRIVE_DEVICE_PATH*)dpbit;

            return hddp->PartitionNumber;
        }

        dpbit = (EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)dpbit + *(uint16_t*)dpbit->Length);
    } while (true);

    return 0;
}

EFI_STATUS look_for_block_devices(EFI_BOOT_SERVICES* bs) {
    EFI_STATUS Status;
    EFI_GUID guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE* handles = NULL;
    UINTN count;
    unsigned int next_disk_num = 0;

    InitializeListHead(&block_devices);

    // do twice - first time for disks, second time for partitions
    for (unsigned int time = 0; time < 2; time++) {
        Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);

        if (EFI_ERROR(Status)) {
            print_error("LocateHandleBuffer", Status);
            return Status;
        }

        for (unsigned int i = 0; i < count; i++) {
            EFI_GUID guid2 = EFI_DEVICE_PATH_PROTOCOL_GUID;
            EFI_DEVICE_PATH_PROTOCOL* device_path;
            EFI_BLOCK_IO* io = NULL;
            unsigned int disk_num, part_num;

            Status = bs->OpenProtocol(handles[i], &guid, (void**)&io, image_handle, NULL,
                                    EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
            if (EFI_ERROR(Status))
                continue;

            if (io->Media->LastBlock == 0) {
                bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                continue;
            }

            Status = bs->HandleProtocol(handles[i], &guid2, (void**)&device_path);
            if (EFI_ERROR(Status)) {
                print_error("HandleProtocol", Status);
                bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                continue;
            }

            if (time == 0) {
                if (!io->Media->LogicalPartition) { // disk
                    disk_num = next_disk_num;
                    part_num = 0;

                    next_disk_num++;
                } else {
                    bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                    bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
                    continue;
                }
            } else {
                if (io->Media->LogicalPartition) { // partition
                    LIST_ENTRY* le = block_devices.Flink;
                    bool found = false;

                    while (le != &block_devices) {
                        block_device* bd = _CR(le, block_device, list_entry);

                        if (bd->part_num == 0 && device_path_is_child(bd->device_path, device_path)) {
                            disk_num = bd->disk_num;
                            found = true;
                            break;
                        }

                        le = le->Flink;
                    }

                    if (!found) {
                        print_string("error - partition found without disk\n");

                        bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                        bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);

                        continue;
                    }

                    part_num = get_partition_number(device_path);

                    if (part_num == 0) {
                        print_string("Could not get partition number.\n");

                        bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                        bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);

                        continue;
                    }
                } else {
                    bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                    bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
                    continue;
                }
            }

            Status = found_block_device(bs, io, disk_num, part_num, duplicate_device_path(device_path));

            if (EFI_ERROR(Status))
                print_error("found_block_device", Status);

            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
        }

        bs->FreePool(handles);
    }

    return EFI_SUCCESS;
}

static inline WCHAR hex_digit(uint8_t v) {
    if (v >= 0xa)
        return v + 'a' - 0xa;
    else
        return v + '0';
}

EFI_STATUS kdnet_init(EFI_BOOT_SERVICES* bs, EFI_FILE_HANDLE dir, EFI_FILE_HANDLE* file, DEBUG_DEVICE_DESCRIPTOR* ddd) {
    EFI_STATUS Status;
    EFI_GUID guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count;

    static const WCHAR dll_prefix[] = L"kd_02_";
    static const WCHAR dll_suffix[] = L".dll";

    WCHAR dll[(sizeof(dll_prefix) / sizeof(WCHAR)) - 1 + (sizeof(dll_suffix) / sizeof(WCHAR)) - 1 + 4 + 1];

    memcpy(dll, dll_prefix, sizeof(dll_prefix) - sizeof(WCHAR));
    memcpy(dll + (sizeof(dll_prefix) / sizeof(WCHAR)) - 1 + 4, dll_suffix, sizeof(dll_suffix));

    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);

    if (EFI_ERROR(Status)) {
        print_error("LocateHandleBuffer", Status);
        return Status;
    }

    for (unsigned int i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL* io;
        PCI_TYPE00 pci;
        WCHAR* ptr;
        EFI_GUID guid2 = EFI_DEVICE_PATH_PROTOCOL_GUID;
        EFI_DEVICE_PATH_PROTOCOL* device_path;
        ACPI_HID_DEVICE_PATH* acpi_dp;
        PCI_DEVICE_PATH* pci_dp;
        unsigned int k;

        Status = bs->OpenProtocol(handles[i], &guid, (void**)&io, image_handle, NULL,
                                  EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
        if (EFI_ERROR(Status))
            continue;

        Status = io->Pci.Read(io, EfiPciIoWidthUint32, 0, sizeof(pci) / sizeof(UINT32), &pci);

        if (EFI_ERROR(Status)) {
            print_error("Pci.Read", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            continue;
        }

        if (pci.Hdr.ClassCode[2] != PCI_CLASS_NETWORK) {
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            continue;
        }

        {
            char s[255], *p;

            p = stpcpy(s, "Found Ethernet card ");
            p = hex_to_str(p, pci.Hdr.VendorId);
            p = stpcpy(p, ":");
            p = hex_to_str(p, pci.Hdr.DeviceId);
            p = stpcpy(p, ".\n");

            print_string(s);
        }

        Status = bs->HandleProtocol(handles[i], &guid2, (void**)&device_path);
        if (EFI_ERROR(Status)) {
            print_error("HandleProtocol", Status);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            continue;
        }

        acpi_dp = (ACPI_HID_DEVICE_PATH*)device_path;

        if (acpi_dp->Header.Type != ACPI_DEVICE_PATH || acpi_dp->Header.SubType != ACPI_DP || (acpi_dp->HID & PNP_EISA_ID_MASK) != PNP_EISA_ID_CONST) {
            print_string("Top of device path was not PciRoot().\n");
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
            continue;
        }

        pci_dp = (PCI_DEVICE_PATH*)((uint8_t*)device_path + *(uint16_t*)acpi_dp->Header.Length);

        if (pci_dp->Header.Type != HARDWARE_DEVICE_PATH || pci_dp->Header.SubType != HW_PCI_DP) {
            print_string("Device path does not refer to PCI device.\n");
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
            continue;
        }

        ptr = &dll[(sizeof(dll_prefix) / sizeof(WCHAR)) - 1];

        *ptr = hex_digit(pci.Hdr.VendorId >> 12); ptr++;
        *ptr = hex_digit((pci.Hdr.VendorId >> 8) & 0xf); ptr++;
        *ptr = hex_digit((pci.Hdr.VendorId >> 4) & 0xf); ptr++;
        *ptr = hex_digit(pci.Hdr.VendorId & 0xf); ptr++;

        {
            char s[255], *p;

            p = stpcpy(s, "Opening ");
            p = stpcpy_utf16(p, dll);
            p = stpcpy(p, " instead of kdstub.dll.\n");

            print_string(s);
        }

        Status = open_file(dir, file, dll);

        if (EFI_ERROR(Status)) {
            if (Status != EFI_NOT_FOUND) {
                print_error("open_file", Status);
                bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
                bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
                goto end;
            }

            print_string("Not found, continuing.\n");

            bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
            bs->CloseProtocol(handles[i], &guid, image_handle, NULL);
            continue;
        }

        // setup debug descriptor

        memset(ddd, 0, sizeof(DEBUG_DEVICE_DESCRIPTOR));

        ddd->Bus = acpi_dp->UID;
        ddd->Slot = pci_dp->Device;
        ddd->Segment = pci_dp->Function;
        ddd->VendorID = pci.Hdr.VendorId;
        ddd->DeviceID = pci.Hdr.DeviceId;
        ddd->BaseClass = pci.Hdr.ClassCode[2];
        ddd->SubClass = pci.Hdr.ClassCode[1];
        ddd->ProgIf = pci.Hdr.ClassCode[0];
        ddd->Flags = DBG_DEVICE_FLAG_BARS_MAPPED;
        ddd->Initialized = 0;
        ddd->Configured = 1;
        // FIXME - Memory
        ddd->PortType = 0x8003; // Ethernet
        ddd->PortSubtype = 0xffff;
        ddd->NameSpace = KdNameSpacePCI;
        // FIXME - TransportType, TransportData

        k = 0;

        for (unsigned int j = 0; j < MAXIMUM_DEBUG_BARS; j++) {
            void* res;

            Status = io->GetBarAttributes(io, j, NULL, &res);

            if (EFI_ERROR(Status)) {
                if (Status != EFI_UNSUPPORTED) // index not valid for this controller
                    print_error("GetBarAttributes", Status);
            } else {
                pci_bar_info* info = (pci_bar_info*)res;

                if (info->space_descriptor != 0x8a) { // QWORD address space descriptor
                    if (info->space_descriptor != 0x79) { // end tag
                        char s[255], *p;

                        p = stpcpy(s, "First byte of pci_bar_info was not 8a (");
                        p = hex_to_str(p, info->space_descriptor);
                        p = stpcpy(p, ").\n");

                        print_string(s);
                    }
                } else if (info->resource_type != 0 && info->resource_type != 1) {
                    char s[255], *p;

                    p = stpcpy(s, "Unsupported resource type ");
                    p = hex_to_str(p, info->resource_type);
                    p = stpcpy(p, ".\n");

                    print_string(s);
                } else {
                    if (info->resource_type == 0)
                        ddd->BaseAddress[k].Type = CmResourceTypeMemory;
                    else
                        ddd->BaseAddress[k].Type = CmResourceTypePort;

                    ddd->BaseAddress[k].Valid = 1;
                    ddd->BaseAddress[k].TranslatedAddress = (uint8_t*)(uintptr_t)info->address_minimum;
                    ddd->BaseAddress[k].Length = info->address_length;

                    k++;
                }

                bs->FreePool(res);
            }
        }

        Status = EFI_SUCCESS;

        bs->CloseProtocol(handles[i], &guid2, image_handle, NULL);
        bs->CloseProtocol(handles[i], &guid, image_handle, NULL);

        goto end;
    }

    Status = EFI_NOT_FOUND;

end:
    bs->FreePool(handles);

    return Status;
}
