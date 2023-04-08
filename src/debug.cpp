#include <string.h>
#include <intrin.h>
#include <variant>
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

typedef bool (__stdcall *KDNET_VMBUS_INITIALIZE) (
    void* Context,
    void* Parameters,
    bool UnreserveChannels,
    void* ArrivalRoutine,
    void* ArrivalRoutineContext,
    uint32_t RequestedVersion
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

// documented in Debuggers/ddk/samples/kdnet/inc/kdnetextensibility.h in Win 10 kit

struct KDNET_EXTENSIBILITY_IMPORTS_81 {
    static constexpr unsigned int num_functions = 0x18;

    uint32_t FunctionCount;
#ifdef __x86_64__
    uint32_t padding;
#endif
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
    NTSTATUS* KdNetErrorStatus;
    char16_t** KdNetErrorString;
    uint32_t* KdNetHardwareID;
};

struct KDNET_EXTENSIBILITY_IMPORTS_10_1507 {
    static constexpr unsigned int num_functions = 0x1d;

    uint32_t FunctionCount;
#ifdef __x86_64__
    uint32_t padding;
#endif
    KDNET_EXTENSIBILITY_EXPORTS* Exports;
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
    NTSTATUS* KdNetErrorStatus;
    char16_t** KdNetErrorString;
    uint32_t* KdNetHardwareID;
};

struct KDNET_EXTENSIBILITY_IMPORTS_10_1607 {
    static constexpr unsigned int num_functions = 0x1e;

    uint32_t FunctionCount;
#ifdef __x86_64__
    uint32_t padding;
#endif
    KDNET_EXTENSIBILITY_EXPORTS* Exports;
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
    char16_t** KdNetErrorString;
    uint32_t* KdNetHardwareID;
};

struct KDNET_EXTENSIBILITY_IMPORTS_10_22H2 {
    static constexpr unsigned int num_functions = 0x1f;

    uint32_t FunctionCount;
#ifdef __x86_64__
    uint32_t padding;
#endif
    KDNET_EXTENSIBILITY_EXPORTS* Exports;
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
    KDNET_VMBUS_INITIALIZE VmbusInitialize;
    NTSTATUS* KdNetErrorStatus;
    char16_t** KdNetErrorString;
    uint32_t* KdNetHardwareID;
};

using kdnet_imports_variant = std::variant<KDNET_EXTENSIBILITY_IMPORTS_81,
                                           KDNET_EXTENSIBILITY_IMPORTS_10_1507,
                                           KDNET_EXTENSIBILITY_IMPORTS_10_1607,
                                           KDNET_EXTENSIBILITY_IMPORTS_10_22H2>;

#pragma pack(pop)

typedef NTSTATUS (__stdcall *KD_INITIALIZE_LIBRARY) (
    void* ImportTable,
    char* LoaderOptions,
    DEBUG_DEVICE_DESCRIPTOR* Device
);

static NTSTATUS net_error_status = STATUS_SUCCESS;
static char16_t* net_error_string = nullptr;
static uint32_t net_hardware_id = 0;
static KD_INITIALIZE_LIBRARY KdInitializeLibrary = nullptr;
static KD_INITIALIZE_CONTROLLER KdInitializeController = nullptr;
void* kdnet_scratch = nullptr;
static uint8_t mac_address[6];

static NTSTATUS call_KdInitializeLibrary(DEBUG_DEVICE_DESCRIPTOR* ddd, kdnet_imports_variant& imports,
                                         KDNET_EXTENSIBILITY_EXPORTS* exports, uint16_t build);

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
    kdnet_imports_variant imports;
    KDNET_EXTENSIBILITY_EXPORTS exports;
    EFI_PHYSICAL_ADDRESS addr;

    Status = find_kd_export(kdstub, build);
    if (EFI_ERROR(Status)) {
        print_error("find_kd_export", Status);
        return Status;
    }

    nt_Status = call_KdInitializeLibrary(ddd, imports, &exports, build);
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

static NTSTATUS call_KdInitializeLibrary(DEBUG_DEVICE_DESCRIPTOR* ddd, kdnet_imports_variant& imports,
                                         KDNET_EXTENSIBILITY_EXPORTS* exports, uint16_t build) {
    NTSTATUS Status;

    if (build >= WIN10_BUILD_22H2)
        imports = KDNET_EXTENSIBILITY_IMPORTS_10_22H2{};
    else if (build >= WIN10_BUILD_1607)
        imports = KDNET_EXTENSIBILITY_IMPORTS_10_1607{};
    else if (build >= WIN10_BUILD_1507)
        imports = KDNET_EXTENSIBILITY_IMPORTS_10_1507{};
    else
        imports = KDNET_EXTENSIBILITY_IMPORTS_81{};

    std::visit([&](auto&& i) {
        memset(&i, 0, sizeof(i));

        if constexpr (requires { i.Exports; }) {
            i.Exports = exports;
            exports->FunctionCount = 13;
        }

        i.FunctionCount = i.num_functions;

        i.GetDevicePciDataByOffset = get_device_pci_data_by_offset;
        i.SetDevicePciDataByOffset = set_device_pci_data_by_offset;
        i.StallExecutionProcessor = stall_cpu;
        i.ReadRegisterUChar = read_register_uchar;
        i.ReadRegisterUShort = read_register_ushort;
        i.ReadRegisterULong = read_register_ulong;
        i.WriteRegisterUChar = write_register_uchar;
        i.WriteRegisterUShort = write_register_ushort;
        i.WriteRegisterULong = write_register_ulong;
        i.WritePortULong = write_port_ulong;
        i.GetPhysicalAddress = get_physical_address;
        i.KdNetErrorStatus = &net_error_status;
        i.KdNetErrorString = &net_error_string;
        i.KdNetHardwareID = &net_hardware_id;

        Status = KdInitializeLibrary(&i, nullptr, ddd);
    }, imports);

    return Status;
}

EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd, uint16_t build) {
    NTSTATUS Status;
    kdnet_imports_variant imports;
    KDNET_EXTENSIBILITY_EXPORTS exports;
    KDNET_SHARED_DATA kd_net_data;

    Status = call_KdInitializeLibrary(ddd, imports, &exports, build);
    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    memset(mac_address, 0, sizeof(mac_address));

    kd_net_data.Hardware = kdnet_scratch;
    kd_net_data.Device = ddd;
    kd_net_data.TargetMacAddress = mac_address;

#ifdef DEBUG
    print_string("Calling KdInitializeController...\n");
#endif

    if (build >= WIN10_BUILD_1507)
        Status = exports.KdInitializeController(&kd_net_data);
    else
        Status = KdInitializeController(&kd_net_data);

    if (!NT_SUCCESS(Status)) {
        char s[255], *p;

        p = stpcpy(s, "KdInitializeController returned ");
        p = hex_to_str(p, (uint32_t)Status);
        p = stpcpy(p, ".\n");

        print_string(s);

        if (net_error_string) {
            unsigned int dest_len = sizeof(s);

            if (!EFI_ERROR(utf16_to_utf8(s, sizeof(s), &dest_len, (wchar_t*)net_error_string, wcslen((wchar_t*)net_error_string) * sizeof(char16_t)))) {
                s[dest_len] = 0;

                print_string(s);
                print_string("\n");
            }
        }

        return EFI_INVALID_PARAMETER;
    }

    if (mac_address[0] != 0 || mac_address[1] != 0 || mac_address[2] != 0 ||
        mac_address[3] != 0 || mac_address[4] != 0 || mac_address[5] != 0) {
        char s[255], *p;

        p = stpcpy(s, "MAC address is ");
        p = hex_to_str(p, mac_address[0], 2);
        p = stpcpy(p, ":");
        p = hex_to_str(p, mac_address[1], 2);
        p = stpcpy(p, ":");
        p = hex_to_str(p, mac_address[2], 2);
        p = stpcpy(p, ":");
        p = hex_to_str(p, mac_address[3], 2);
        p = stpcpy(p, ":");
        p = hex_to_str(p, mac_address[4], 2);
        p = stpcpy(p, ":");
        p = hex_to_str(p, mac_address[5], 2);
        p = stpcpy(p, ".\n");

        print_string(s);
    }

    return EFI_SUCCESS;
}
