#include <string.h>
#include <intrin.h>
#include "quibble.h"
#include "misc.h"
#include "peload.h"
#include "x86.h"
#include "print.h"

typedef int32_t NTSTATUS;
#define NT_SUCCESS(Status) ((Status)>=0)

#define STATUS_SUCCESS              (NTSTATUS)0
#define STATUS_INVALID_PARAMETER    (NTSTATUS)0xc000000d

// documented in Debuggers/ddk/samples/kdnet/inc/kdnetshareddata.h in Win 10 kit
struct KDNET_SHARED_DATA {
    void* Hardware;
    DEBUG_DEVICE_DESCRIPTOR* Device;
    uint8_t* TargetMacAddress;
    uint32_t LinkSpeed;
    uint32_t LinkDuplex;
    uint8_t* LinkState;
    uint32_t SerialBaudRate;
    uint32_t Flags;
    uint8_t RestartKdnet;
    uint8_t Reserved[3];
};

typedef NTSTATUS (__stdcall *KD_INITIALIZE_CONTROLLER) (
    KDNET_SHARED_DATA* KdNet
);

typedef void (__stdcall *KD_SHUTDOWN_CONTROLLER) (
    void* Adapter
);

typedef void (__stdcall *KD_SET_HIBERNATE_RANGE) (void);

typedef NTSTATUS (__stdcall *KD_DEVICE_CONTROL) (
    void* Adapter,
    uint32_t RequestCode,
    void* InputBuffer,
    uint32_t InputBufferLength,
    void* OutputBuffer,
    uint32_t OutputBufferLength
);

typedef NTSTATUS (__stdcall *KD_GET_RX_PACKET) (
    void* Adapter,
    uint32_t* Handle,
    void** Packet,
    uint32_t* Length
);

typedef void (__stdcall *KD_RELEASE_RX_PACKET) (
    void* Adapter,
    uint32_t Handle
);

typedef NTSTATUS (__stdcall *KD_GET_TX_PACKET) (
    void* Adapter,
    uint32_t* Handle
);

typedef NTSTATUS (__stdcall *KD_SEND_TX_PACKET) (
    void* Adapter,
    uint32_t Handle,
    uint32_t Length
);

typedef void* (__stdcall *KD_GET_PACKET_ADDRESS) (
    void* Adapter,
    uint32_t Handle
);

typedef uint32_t (__stdcall *KD_GET_PACKET_LENGTH) (
    void* Adapter,
    uint32_t Handle
);

typedef uint32_t (__stdcall *KD_GET_HARDWARE_CONTEXT_SIZE) (
    DEBUG_DEVICE_DESCRIPTOR* Device
);

typedef NTSTATUS (__stdcall *KD_READ_SERIAL_BYTE) (
    void* Adapter,
    uint8_t* Byte
);

typedef NTSTATUS (__stdcall *KD_WRITE_SERIAL_BYTE) (
    void* Adapter,
    const uint8_t Byte
);

typedef NTSTATUS (__stdcall *DEBUG_SERIAL_OUTPUT_INIT) (
    DEBUG_DEVICE_DESCRIPTOR* pDevice,
    uint64_t* PAddress
);

typedef void (*DEBUG_SERIAL_OUTPUT_BYTE) (
    const uint8_t Byte
);

typedef uint32_t (__stdcall *KDNET_GET_PCI_DATA_BY_OFFSET) (
    uint32_t BusNumber,
    uint32_t SlotNumber,
    void* Buffer,
    uint32_t Offset,
    uint32_t Length
);

typedef uint32_t (__stdcall *KDNET_SET_PCI_DATA_BY_OFFSET) (
    uint32_t BusNumber,
    uint32_t SlotNumber,
    void* Buffer,
    uint32_t Offset,
    uint32_t Length
);

typedef void (__stdcall *KDNET_STALL_EXECUTION_PROCESSOR) (
    uint32_t Microseconds
);

typedef uint8_t (__stdcall *KDNET_READ_REGISTER_UCHAR) (
    uint8_t* Register
);

typedef uint16_t (__stdcall *KDNET_READ_REGISTER_USHORT) (
    uint16_t* Register
);

typedef uint32_t (__stdcall *KDNET_READ_REGISTER_ULONG) (
    uint32_t* Register
);

typedef uint64_t (__stdcall *KDNET_READ_REGISTER_ULONG64) (
    uint64_t* Register
);

typedef void (__stdcall *KDNET_WRITE_REGISTER_UCHAR) (
    uint8_t* Register,
    uint8_t Value
);

typedef void (__stdcall *KDNET_WRITE_REGISTER_USHORT) (
    uint16_t* Register,
    uint16_t Value
);

typedef void (__stdcall *KDNET_WRITE_REGISTER_ULONG) (
    uint32_t* Register,
    uint32_t Value
);

typedef void (__stdcall *KDNET_WRITE_REGISTER_ULONG64) (
    uint64_t* Register,
    uint64_t Value
);

typedef void* (__stdcall *KDNET_GET_PHYSICAL_ADDRESS) (
    void* Va
);

typedef uint8_t (__stdcall *KDNET_READ_PORT_UCHAR) (
    uint8_t* Port
);

typedef uint16_t (__stdcall *KDNET_READ_PORT_USHORT) (
    uint16_t* Port
);

typedef uint32_t (__stdcall *KDNET_READ_PORT_ULONG) (
    uint32_t* Port
);

typedef uint64_t (__stdcall *KDNET_READ_PORT_ULONG64) (
    uint64_t* Port
);

typedef void (__stdcall *KDNET_WRITE_PORT_UCHAR) (
    uint16_t Port,
    uint8_t Value
);

typedef void (__stdcall *KDNET_WRITE_PORT_USHORT) (
    uint16_t Port,
    uint16_t Value
);

typedef void (__stdcall *KDNET_WRITE_PORT_ULONG) (
    uint16_t Port,
    uint32_t Value
);

typedef void (__stdcall *KDNET_WRITE_PORT_ULONG64) (
    uint16_t Port,
    uint64_t Value
);

typedef void (__stdcall *KDNET_SET_HIBER_RANGE) (
    void* MemoryMap,
    uint32_t Flags,
    void* Address,
    uintptr_t Length,
    uint32_t Tag
);

typedef void (__stdcall *KDNET_BUGCHECK_EX) (
    uint32_t BugCheckCode,
    uintptr_t BugCheckParameter1,
    uintptr_t BugCheckParameter2,
    uintptr_t BugCheckParameter3,
    uintptr_t BugCheckParameter4
);

typedef void* (__stdcall *KDNET_MAP_PHYSICAL_MEMORY_64) (
    uint64_t PhysicalAddress,
    uint32_t NumberPages,
    bool FlushCurrentTLB
);

typedef void (__stdcall *KDNET_UNMAP_VIRTUAL_ADDRESS) (
    void* VirtualAddress,
    uint32_t NumberPages,
    bool FlushCurrentTLB
);

typedef uint64_t (__stdcall *KDNET_READ_CYCLE_COUNTER) (
    uint64_t* Frequency
);

typedef void (__stdcall *KDNET_DBGPRINT) (
    char* pFmt,
    ...
);

// documented in Debuggers/ddk/samples/kdnet/inc/kdnetextensibility.h in Win 10 kit
struct KDNET_EXTENSIBILITY_EXPORTS {
    uint32_t FunctionCount;
    KD_INITIALIZE_CONTROLLER KdInitializeController;
    KD_SHUTDOWN_CONTROLLER KdShutdownController;
    KD_SET_HIBERNATE_RANGE KdSetHibernateRange;
    KD_GET_RX_PACKET KdGetRxPacket;
    KD_RELEASE_RX_PACKET KdReleaseRxPacket;
    KD_GET_TX_PACKET KdGetTxPacket;
    KD_SEND_TX_PACKET KdSendTxPacket;
    KD_GET_PACKET_ADDRESS KdGetPacketAddress;
    KD_GET_PACKET_LENGTH KdGetPacketLength;
    KD_GET_HARDWARE_CONTEXT_SIZE KdGetHardwareContextSize;
    KD_DEVICE_CONTROL KdDeviceControl;
    KD_READ_SERIAL_BYTE KdReadSerialByte;
    KD_WRITE_SERIAL_BYTE KdWriteSerialByte;
    DEBUG_SERIAL_OUTPUT_INIT DebugSerialOutputInit;
    DEBUG_SERIAL_OUTPUT_BYTE DebugSerialOutputByte;
};

#pragma pack(push,1)

struct kdnet_imports2 {
    KDNET_GET_PCI_DATA_BY_OFFSET GetDevicePciDataByOffset;
    KDNET_SET_PCI_DATA_BY_OFFSET SetDevicePciDataByOffset;
    KDNET_GET_PHYSICAL_ADDRESS GetPhysicalAddress;
    KDNET_STALL_EXECUTION_PROCESSOR StallExecutionProcessor;
    KDNET_READ_REGISTER_UCHAR ReadRegisterUChar;
    KDNET_READ_REGISTER_USHORT ReadRegisterUShort;
    KDNET_READ_REGISTER_ULONG ReadRegisterULong;
    KDNET_READ_REGISTER_ULONG64 ReadRegisterULong64;
    KDNET_WRITE_REGISTER_UCHAR WriteRegisterUChar;
    KDNET_WRITE_REGISTER_USHORT WriteRegisterUShort;
    KDNET_WRITE_REGISTER_ULONG WriteRegisterULong;
    KDNET_WRITE_REGISTER_ULONG64 WriteRegisterULong64;
    KDNET_READ_PORT_UCHAR ReadPortUChar;
    KDNET_READ_PORT_USHORT ReadPortUShort;
    KDNET_READ_PORT_ULONG ReadPortULong;
    KDNET_READ_PORT_ULONG64 ReadPortULong64;
    KDNET_WRITE_PORT_UCHAR WritePortUChar;
    KDNET_WRITE_PORT_USHORT WritePortUShort;
    KDNET_WRITE_PORT_ULONG WritePortULong;
    KDNET_WRITE_PORT_ULONG64 WritePortULong64;
    KDNET_SET_HIBER_RANGE SetHiberRange;
    KDNET_BUGCHECK_EX BugCheckEx;
    KDNET_MAP_PHYSICAL_MEMORY_64 MapPhysicalMemory64;
    KDNET_UNMAP_VIRTUAL_ADDRESS UnmapVirtualAddress;
    KDNET_READ_CYCLE_COUNTER ReadCycleCounter;
    KDNET_DBGPRINT KdNetDbgPrintf;
    NTSTATUS* KdNetErrorStatus;
    wchar_t** KdNetErrorString;
    uint32_t* KdNetHardwareID;
};

// documented in Debuggers/ddk/samples/kdnet/inc/kdnetextensibility.h in Win 10 kit
struct KDNET_EXTENSIBILITY_IMPORTS {
    uint32_t FunctionCount;
#ifdef __x86_64__
    uint32_t padding;
#endif
    union {
        kdnet_imports2 imports_win81;

        struct {
            KDNET_EXTENSIBILITY_EXPORTS* exports;
            kdnet_imports2 imports;
        } win10;
    };
};

#pragma pack(pop)

typedef NTSTATUS (__stdcall *KD_INITIALIZE_LIBRARY) (
    KDNET_EXTENSIBILITY_IMPORTS* ImportTable,
    char* LoaderOptions,
    DEBUG_DEVICE_DESCRIPTOR* Device
);

NTSTATUS net_error_status = STATUS_SUCCESS;
wchar_t* net_error_string = NULL;
uint32_t net_hardware_id = 0;

static NTSTATUS call_KdInitializeLibrary(DEBUG_DEVICE_DESCRIPTOR* ddd, KDNET_EXTENSIBILITY_IMPORTS* imports,
                                         KDNET_EXTENSIBILITY_EXPORTS* exports, uint16_t build);

KD_INITIALIZE_LIBRARY KdInitializeLibrary = NULL;
KD_INITIALIZE_CONTROLLER KdInitializeController = NULL;
static DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor;
void* kdnet_scratch = NULL;
static uint8_t mac_address[6];

EFI_STATUS find_kd_export(EFI_PE_IMAGE* kdstub, uint16_t build) {
    UINT64 addr;
    EFI_STATUS Status;

    Status = kdstub->FindExport(kdstub, "KdInitializeLibrary", &addr, NULL);

    if (EFI_ERROR(Status)) {
        print_error("FindExport", Status);
        return Status;
    }

    KdInitializeLibrary = (KD_INITIALIZE_LIBRARY)(uintptr_t)addr;

    if (build < WIN10_BUILD_1507) {
        Status = kdstub->FindExport(kdstub, "KdInitializeController", &addr, NULL);

        if (EFI_ERROR(Status)) {
            print_error("FindExport", Status);
            return Status;
        }

        KdInitializeController = (KD_INITIALIZE_CONTROLLER)(uintptr_t)addr;
    }

    return EFI_SUCCESS;
}

EFI_STATUS allocate_kdnet_hw_context(EFI_PE_IMAGE* kdstub, DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build) {
    EFI_STATUS Status;
    NTSTATUS nt_Status;
    KDNET_EXTENSIBILITY_IMPORTS imports;
    KDNET_EXTENSIBILITY_EXPORTS exports;
    EFI_PHYSICAL_ADDRESS addr;

    Status = find_kd_export(kdstub, build);
    if (EFI_ERROR(Status)) {
        print_error("find_kd_export", Status);
        return Status;
    }

    nt_Status = call_KdInitializeLibrary(ddd, &imports, &exports, build);
    if (!NT_SUCCESS(nt_Status)) {
        char s[255], *p;

        p = stpcpy(s, "KdInitializeLibrary returned ");
        p = hex_to_str(p, (uint32_t)nt_Status);
        p = stpcpy(p, ".\n");

        print_string(s);

        return EFI_INVALID_PARAMETER;
    }

    if (build >= WIN10_BUILD_1507)
        ddd->TransportData.HwContextSize = exports.KdGetHardwareContextSize(ddd);
    else
        ddd->TransportData.HwContextSize = ddd->Memory.Length; // set by KdInitializeLibrary

    if (ddd->TransportData.HwContextSize != 0) {
        Status = systable->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, page_count(ddd->TransportData.HwContextSize), &addr);
        if (EFI_ERROR(Status)) {
            print_error("AllocatePages", Status);
            return Status;
        }

        kdnet_scratch = (void*)(uintptr_t)addr;
    }

    return EFI_SUCCESS;
}

static uint32_t __stdcall get_device_pci_data_by_offset(uint32_t bus, uint32_t slot, void* data, uint32_t offset, uint32_t length) {
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

    return 0;
}

static uint32_t __stdcall set_device_pci_data_by_offset(uint32_t bus, uint32_t slot, void* data, uint32_t offset, uint32_t length) {
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

    return 0;
}

static uint8_t __stdcall read_register_uchar(uint8_t* addr) {
    return *addr;
}

static uint16_t __stdcall read_register_ushort(uint16_t* addr) {
    return *addr;
}

static uint32_t __stdcall read_register_ulong(uint32_t* addr) {
    return *addr;
}

static void __stdcall write_register_uchar(uint8_t* addr, uint8_t value) {
    *addr = value;
}

static void __stdcall write_register_ushort(uint16_t* addr, uint16_t value) {
    *addr = value;
}

static void __stdcall write_register_ulong(uint32_t* addr, uint32_t value) {
    *addr = value;
}

static void __stdcall stall_cpu(uint32_t microseconds) {
    uint64_t tsc;

    tsc = __rdtsc();
    tsc += (cpu_frequency / 1000000ull) * microseconds;

    while (true) {
        if (__rdtsc() >= tsc)
            return;
    }
}

static void __stdcall write_port_ulong(uint16_t port, uint32_t value) {
    __outdword(port, value);
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

static NTSTATUS call_KdInitializeLibrary(DEBUG_DEVICE_DESCRIPTOR* ddd, KDNET_EXTENSIBILITY_IMPORTS* imports,
                                         KDNET_EXTENSIBILITY_EXPORTS* exports, uint16_t build) {
    kdnet_imports2* imp2;
    debug_device_descriptor = ddd;

    memset(imports, 0, sizeof(*imports));

    if (build >= WIN10_BUILD_1607)
        imports->FunctionCount = 0x1e;
    else if (build >= WIN10_BUILD_1507)
        imports->FunctionCount = 0x1d;
    else
        imports->FunctionCount = 0x18;

    if (build >= WIN10_BUILD_1507) {
        imports->win10.exports = exports;
        imp2 = &imports->win10.imports;

        exports->FunctionCount = 13;
    } else
        imp2 = &imports->imports_win81;

    imp2->GetDevicePciDataByOffset = get_device_pci_data_by_offset;
    imp2->SetDevicePciDataByOffset = set_device_pci_data_by_offset;
    imp2->StallExecutionProcessor = stall_cpu;
    imp2->ReadRegisterUChar = read_register_uchar;
    imp2->ReadRegisterUShort = read_register_ushort;
    imp2->ReadRegisterULong = read_register_ulong;
    imp2->WriteRegisterUChar = write_register_uchar;
    imp2->WriteRegisterUShort = write_register_ushort;
    imp2->WriteRegisterULong = write_register_ulong;
    imp2->WritePortULong = write_port_ulong;
    imp2->GetPhysicalAddress = get_physical_address;
    imp2->KdNetErrorStatus = &net_error_status;
    imp2->KdNetErrorString = &net_error_string;
    imp2->KdNetHardwareID = &net_hardware_id;

    return KdInitializeLibrary(imports, NULL, ddd);
}

EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build) {
    NTSTATUS Status;
    KDNET_EXTENSIBILITY_IMPORTS imports;
    KDNET_EXTENSIBILITY_EXPORTS exports;
    KDNET_SHARED_DATA kd_net_data;

    debug_device_descriptor = ddd;

    Status = call_KdInitializeLibrary(ddd, &imports, &exports, build);
    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    kd_net_data.Hardware = kdnet_scratch;
    kd_net_data.Device = ddd;
    kd_net_data.TargetMacAddress = mac_address;

    if (build >= WIN10_BUILD_1507)
        Status = exports.KdInitializeController(&kd_net_data);
    else
        Status = KdInitializeController(&kd_net_data);

    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    return EFI_SUCCESS;
}
