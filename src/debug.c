#include <string.h>
#include <intrin.h>
#include "quibble.h"
#include "misc.h"
#include "peload.h"
#include "x86.h"

typedef int32_t NTSTATUS;
#define NT_SUCCESS(Status) ((Status)>=0)

#define STATUS_SUCCESS              (NTSTATUS)0
#define STATUS_INVALID_PARAMETER    (NTSTATUS)0xc000000d

typedef struct {
    void* scratch;
    DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor;
    // FIXME - Windows 8.1 wants some more stuff here
} KD_NET_DATA;

typedef NTSTATUS (__stdcall *KD_INITIALIZE_CONTROLLER) (
    KD_NET_DATA* kd_net_data
);

typedef uint64_t (__stdcall *KD_GET_HARDWARE_CONTEXT_SIZE) (
    DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor
);

typedef NTSTATUS (__stdcall *GET_DEVICE_PCI_DATA_BY_OFFSET) (
    uint32_t bus,
    uint32_t slot,
    void* data,
    uint32_t offset,
    uint32_t length
);

typedef NTSTATUS (__stdcall *SET_DEVICE_PCI_DATA_BY_OFFSET) (
    uint32_t bus,
    uint32_t slot,
    void* data,
    uint32_t offset,
    uint32_t length
);

typedef void (__stdcall *STALL_EXECUTION_PROCESSOR) (
    int microseconds
);

typedef uint8_t (__stdcall *READ_REGISTER_UCHAR) (
    void* addr
);

typedef uint16_t (__stdcall *READ_REGISTER_USHORT) (
    void* addr
);

typedef uint32_t (__stdcall *READ_REGISTER_ULONG) (
    void* addr
);

typedef NTSTATUS (__stdcall *WRITE_REGISTER_UCHAR) (
    void* addr,
    uint8_t value
);

typedef NTSTATUS (__stdcall *WRITE_REGISTER_USHORT) (
    void* addr,
    uint16_t value
);

typedef NTSTATUS (__stdcall *WRITE_REGISTER_ULONG) (
    void* addr,
    uint32_t value
);

typedef void* (__stdcall *GET_PHYSICAL_ADDRESS) (
    void* va
);

typedef NTSTATUS (__stdcall *WRITE_PORT_ULONG) (
    uint16_t port,
    uint32_t value
);

typedef struct {
    uint32_t count;
    KD_INITIALIZE_CONTROLLER KdInitializeController;
    void* KdShutdownController;
    void* KdSetHibernateRange;
    void* KdGetRxPacket;
    void* KdReleaseRxPacket;
    void* KdGetTxPacket;
    void* KdSendTxPacket;
    void* KdGetPacketAddress;
    void* KdGetPacketLength;
    KD_GET_HARDWARE_CONTEXT_SIZE KdGetHardwareContextSize;
    void* unknown1;
    void* unknown2;
    void* unknown3;
} kd_funcs;

#pragma pack(push,1)

typedef struct {
    GET_DEVICE_PCI_DATA_BY_OFFSET GetDevicePciDataByOffset;
    SET_DEVICE_PCI_DATA_BY_OFFSET SetDevicePciDataByOffset;
    GET_PHYSICAL_ADDRESS GetPhysicalAddress;
    STALL_EXECUTION_PROCESSOR KdStallExecutionProcessor;
    READ_REGISTER_UCHAR READ_REGISTER_UCHAR;
    READ_REGISTER_USHORT READ_REGISTER_USHORT;
    READ_REGISTER_ULONG READ_REGISTER_ULONG;
    void* READ_REGISTER_ULONG64;
    WRITE_REGISTER_UCHAR WRITE_REGISTER_UCHAR;
    WRITE_REGISTER_USHORT WRITE_REGISTER_USHORT;
    WRITE_REGISTER_ULONG WRITE_REGISTER_ULONG;
    void* WRITE_REGISTER_ULONG64;
    void* READ_PORT_UCHAR;
    void* READ_PORT_USHORT;
    void* READ_PORT_ULONG;
    void* unknown1; // READ_PORT_ULONG64?
    void* WRITE_PORT_UCHAR;
    void* WRITE_PORT_USHORT;
    WRITE_PORT_ULONG WRITE_PORT_ULONG;
    void* unknown2; // WRITE_PORT_ULONG64?
    void* PoSetHiberRange;
    void* KeBugCheckEx;
    void* MapPhysicalMemory;
    void* UnmapVirtualAddress;
    void* KdReadCycleCounter;
    void* KdNetPringDbgLog;
    NTSTATUS* KdNetErrorStatus;
    WCHAR** KdNetErrorString;
    uint32_t* KdNetHardwareId;
} kdnet_exports2;

typedef struct {
    uint32_t version;
#ifdef __x86_64__
    uint32_t padding;
#endif
    union {
        kdnet_exports2 exports_win81;

        struct {
            kd_funcs* funcs;
            kdnet_exports2 exports;
        } win10;
    };
} kdnet_exports;

#pragma pack(pop)

typedef NTSTATUS (__stdcall *KD_INITIALIZE_LIBRARY) (
    kdnet_exports* exports,
    void* param1,
    DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor
);

NTSTATUS net_error_status = STATUS_SUCCESS;
WCHAR* net_error_string = NULL;
uint32_t net_hardware_id = 0;

static NTSTATUS call_KdInitializeLibrary(DEBUG_DEVICE_DESCRIPTOR* ddd, kdnet_exports* exports,
                                         kd_funcs* funcs, uint16_t build);

KD_INITIALIZE_LIBRARY KdInitializeLibrary = NULL;
KD_INITIALIZE_CONTROLLER KdInitializeController = NULL;
static DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor;
void* kdnet_scratch = NULL;

EFI_STATUS find_kd_export(EFI_PE_IMAGE* kdstub, uint16_t build) {
    UINT64 addr;
    EFI_STATUS Status;

    Status = kdstub->FindExport(kdstub, "KdInitializeLibrary", &addr, NULL);

    if (EFI_ERROR(Status)) {
        print_error(L"FindExport", Status);
        return Status;
    }

    KdInitializeLibrary = (KD_INITIALIZE_LIBRARY)(uintptr_t)addr;

    if (build < WIN10_BUILD_1507) {
        Status = kdstub->FindExport(kdstub, "KdInitializeController", &addr, NULL);

        if (EFI_ERROR(Status)) {
            print_error(L"FindExport", Status);
            return Status;
        }

        KdInitializeController = (KD_INITIALIZE_CONTROLLER)(uintptr_t)addr;
    }

    return EFI_SUCCESS;
}

EFI_STATUS allocate_kdnet_hw_context(EFI_PE_IMAGE* kdstub, DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build) {
    EFI_STATUS Status;
    NTSTATUS nt_Status;
    kdnet_exports exports;
    kd_funcs funcs;
    EFI_PHYSICAL_ADDRESS addr;

    Status = find_kd_export(kdstub, build);
    if (EFI_ERROR(Status)) {
        print_error(L"find_kd_export", Status);
        return Status;
    }

    nt_Status = call_KdInitializeLibrary(ddd, &exports, &funcs, build);
    if (!NT_SUCCESS(nt_Status)) {
        print(L"KdInitializeLibrary returned ");
        print_hex((uint32_t)nt_Status);
        print(L".\r\n");
        return EFI_INVALID_PARAMETER;
    }

    if (build >= WIN10_BUILD_1507)
        ddd->TransportData.HwContextSize = funcs.KdGetHardwareContextSize(ddd);
    else
        ddd->TransportData.HwContextSize = ddd->Memory.Length; // set by KdInitializeLibrary

    if (ddd->TransportData.HwContextSize != 0) {
        Status = systable->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, PAGE_COUNT(ddd->TransportData.HwContextSize), &addr);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePages", Status);
            return Status;
        }

        kdnet_scratch = (void*)(uintptr_t)addr;
    }

    return EFI_SUCCESS;
}

static NTSTATUS __stdcall get_device_pci_data_by_offset(uint32_t bus, uint32_t slot, void* data, uint32_t offset, uint32_t length) {
    uint32_t address = 0x80000000 | ((bus & 0xff) << 16) | ((slot & 0x1f) << 11);

    if (offset % sizeof(uint32_t) == 0 && length % sizeof(uint32_t) == 0) {
        while (length > 0) {
            address &= 0xffffff00;
            address |= offset;

            __outdword(0xcf8, address);
            *(uint32_t*)data = __indword(0xcfc);

            data = (uint8_t*)data + sizeof(uint32_t);
            offset += sizeof(uint32_t);
            length -= sizeof(uint32_t);
        }
    } else if (offset % sizeof(uint16_t) == 0 && length % sizeof(uint16_t) == 0) {
        while (length > 0) {
            uint32_t val;

            address &= 0xffffff00;
            address |= offset & 0xfc;

            __outdword(0xcf8, address);
            val = __indword(0xcfc);

            if (offset % sizeof(uint32_t) != 0)
                *(uint16_t*)data = val >> 16;
            else
                *(uint16_t*)data = val & 0xff;

            data = (uint8_t*)data + sizeof(uint16_t);
            offset += sizeof(uint16_t);
            length -= sizeof(uint16_t);
        }
    } else {
        // FIXME
    }

    return STATUS_SUCCESS;
}

static NTSTATUS __stdcall set_device_pci_data_by_offset(uint32_t bus, uint32_t slot, void* data, uint32_t offset, uint32_t length) {
    uint32_t address = 0x80000000 | ((bus & 0xff) << 16) | ((slot & 0x1f) << 11);

    if (offset % sizeof(uint32_t) == 0 && length % sizeof(uint32_t) == 0) {
        while (length > 0) {
            address &= 0xffffff00;
            address |= offset;

            __outdword(0xcf8, address);
            __outdword(0xcfc, *(uint32_t*)data);

            data = (uint8_t*)data + sizeof(uint32_t);
            offset += sizeof(uint32_t);
            length -= sizeof(uint32_t);
        }
    } else if (offset % sizeof(uint16_t) == 0 && length % sizeof(uint16_t) == 0) {
        while (length > 0) {
            uint32_t val;

            address &= 0xffffff00;
            address |= offset & 0xfc;

            __outdword(0xcf8, address);
            val = __indword(0xcfc);

            if (offset % sizeof(uint32_t) != 0)
                val = (val & 0xff00) | *(uint16_t*)data;
            else
                val = (*(uint16_t*)data << 16) | (val & 0xff);

            __outdword(0xcfc, val);

            data = (uint8_t*)data + sizeof(uint16_t);
            offset += sizeof(uint16_t);
            length -= sizeof(uint16_t);
        }
    } else {
        // FIXME
    }

    return STATUS_SUCCESS;
}

static uint8_t __stdcall read_register_uchar(void* addr) {
    return *(uint8_t*)addr;
}

static uint16_t __stdcall read_register_ushort(void* addr) {
    return *(uint16_t*)addr;
}

static uint32_t __stdcall read_register_ulong(void* addr) {
    return *(uint32_t*)addr;
}

static NTSTATUS __stdcall write_register_uchar(void* addr, uint8_t value) {
    *(uint8_t*)addr = value;

    return STATUS_SUCCESS;
}

static NTSTATUS __stdcall write_register_ushort(void* addr, uint16_t value) {
    *(uint16_t*)addr = value;

    return STATUS_SUCCESS;
}

static NTSTATUS __stdcall write_register_ulong(void* addr, uint32_t value) {
    *(uint32_t*)addr = value;

    return STATUS_SUCCESS;
}

static void __stdcall stall_cpu(int microseconds) {
    uint64_t tsc;

    tsc = __rdtsc();
    tsc += (cpu_frequency / 1000000ull) * microseconds;

    while (true) {
        if (__rdtsc() >= tsc)
            return;
    }
}

static NTSTATUS __stdcall write_port_ulong(uint16_t port, uint32_t value) {
    __outdword(port, value);

    return STATUS_SUCCESS;
}

static void* __stdcall get_physical_address(void* va) {
#ifdef __x86_64__
    uintptr_t addr = (uintptr_t)va, ret;
    uint64_t off1, off2, off3, off4;
    HARDWARE_PTE_PAE* map = (HARDWARE_PTE_PAE*)SELFMAP_PML4;

    off1 = (addr & 0xff8000000000) >> 39;

    if (!map[off1].Valid)
        return NULL;

    map = (HARDWARE_PTE_PAE*)(SELFMAP_PDP | (off1 << 12));

    off2 = (addr & 0x7fc0000000) >> 30;

    if (!map[off2].Valid)
        return NULL;

    map = (HARDWARE_PTE_PAE*)(SELFMAP_PD | (off1 << 21) | (off2 << 12));

    off3 = (addr & 0x3fe00000) >> 21;

    if (!map[off3].Valid)
        return NULL;

    map = (HARDWARE_PTE_PAE*)(SELFMAP | (off1 << 30) | (off2 << 21) | (off3 << 12));

    off4 = (addr & 0x1ff000) >> 12;

    if (!map[off4].Valid)
        return NULL;

    ret = (map[off4].PageFrameNumber << 12) | (addr & 0xfff);

    return (void*)ret;
#else
    uintptr_t addr = (uintptr_t)va, ret;
    uint32_t off1, off2;
    HARDWARE_PTE_PAE* map = (HARDWARE_PTE_PAE*)SELFMAP2;

    // Assume PAE - it's mandatory on all the OSes which will call this function

    off1 = (addr & 0xffe00000) >> 21;

    if (!map[off1].Valid)
        return NULL;

    map = (HARDWARE_PTE_PAE*)(SELFMAP | (off1 << 12));

    off2 = (addr & 0x1ff000) >> 12;

    if (!map[off2].Valid)
        return NULL;

    ret = (map[off2].PageFrameNumber << 12) | (addr & 0xfff);

    return (void*)ret;
#endif
}

static NTSTATUS call_KdInitializeLibrary(DEBUG_DEVICE_DESCRIPTOR* ddd, kdnet_exports* exports,
                                         kd_funcs* funcs, uint16_t build) {
    kdnet_exports2* exp2;
    debug_device_descriptor = ddd;

    memset(exports, 0, sizeof(*exports));

    if (build >= WIN10_BUILD_1607)
        exports->version = 0x1e;
    else if (build >= WIN10_BUILD_1507)
        exports->version = 0x1d;
    else
        exports->version = 0x18;

    if (build >= WIN10_BUILD_1507) {
        exports->win10.funcs = funcs;
        exp2 = &exports->win10.exports;

        funcs->count = 13; // number of functions
    } else
        exp2 = &exports->exports_win81;

    exp2->GetDevicePciDataByOffset = get_device_pci_data_by_offset;
    exp2->SetDevicePciDataByOffset = set_device_pci_data_by_offset;
    exp2->KdStallExecutionProcessor = stall_cpu;
    exp2->READ_REGISTER_UCHAR = read_register_uchar;
    exp2->READ_REGISTER_USHORT = read_register_ushort;
    exp2->READ_REGISTER_ULONG = read_register_ulong;
    exp2->WRITE_REGISTER_UCHAR = write_register_uchar;
    exp2->WRITE_REGISTER_USHORT = write_register_ushort;
    exp2->WRITE_REGISTER_ULONG = write_register_ulong;
    exp2->WRITE_PORT_ULONG = write_port_ulong;
    exp2->GetPhysicalAddress = get_physical_address;
    exp2->KdNetErrorStatus = &net_error_status;
    exp2->KdNetErrorString = &net_error_string;
    exp2->KdNetHardwareId = &net_hardware_id;

    return KdInitializeLibrary(exports, NULL, ddd);
}

EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build) {
    NTSTATUS Status;
    kdnet_exports exports;
    kd_funcs funcs;
    KD_NET_DATA kd_net_data;

    debug_device_descriptor = ddd;

    Status = call_KdInitializeLibrary(ddd, &exports, &funcs, build);
    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    kd_net_data.scratch = kdnet_scratch;
    kd_net_data.debug_device_descriptor = ddd;

    if (build >= WIN10_BUILD_1507)
        Status = funcs.KdInitializeController(&kd_net_data);
    else
        Status = KdInitializeController(&kd_net_data);

    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    return EFI_SUCCESS;
}
