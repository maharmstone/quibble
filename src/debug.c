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
} KD_NET_DATA;

typedef NTSTATUS (__stdcall *KD_INITIALIZE_CONTROLLER) (
    KD_NET_DATA* kd_net_data
);

typedef NTSTATUS (__stdcall *GET_DEVICE_PCI_DATA_BY_OFFSET) (
    uint32_t bus,
    uint32_t slot,
    void* data,
    uint32_t offset,
    uint32_t length
);

typedef void (__stdcall *STALL_EXECUTION_PROCESSOR) (
    int microseconds
);

typedef uint32_t (__stdcall *READ_REGISTER_ULONG) (
    void* addr
);

typedef NTSTATUS (__stdcall *WRITE_REGISTER_ULONG) (
    void* addr,
    uint32_t value
);

typedef void* (__stdcall *GET_PHYSICAL_ADDRESS) (
    void* va
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
    void* KdGetHardwareContextSize;
    void* unknown1;
    void* unknown2;
    void* unknown3;
} kd_funcs;

typedef struct {
    uint32_t unknown1;
    uint32_t padding;
    kd_funcs* funcs;
    GET_DEVICE_PCI_DATA_BY_OFFSET GetDevicePciDataByOffset;
    void* unknown3;
    GET_PHYSICAL_ADDRESS GetPhysicalAddress;
    STALL_EXECUTION_PROCESSOR KdStallExecutionProcessor;
    void* READ_REGISTER_UCHAR;
    void* READ_REGISTER_USHORT;
    READ_REGISTER_ULONG READ_REGISTER_ULONG;
    void* READ_REGISTER_ULONG64;
    void* WRITE_REGISTER_UCHAR;
    void* WRITE_REGISTER_USHORT;
    WRITE_REGISTER_ULONG WRITE_REGISTER_ULONG;
    void* WRITE_REGISTER_ULONG64;
    void* unknown10;
    void* unknown11;
    void* unknown12;
    void* unknown13;
    void* WRITE_PORT_UCHAR;
    void* WRITE_PORT_USHORT;
    void* WRITE_PORT_ULONG;
    void* unknown17;
    void* unknown18;
    void* unknown19;
    void* unknown20;
    void* unknown21;
    void* KdReadCycleCounter;
    void* KdNetErrorStatus;
    void* KdNetErrorString;
    void* KdNetHardwareId;
} kdnet_exports;

typedef NTSTATUS (__stdcall KD_INITIALIZE_LIBRARY) (
    kdnet_exports* exports,
    void* param1,
    DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor
);

KD_INITIALIZE_LIBRARY* KdInitializeLibrary = NULL;
static DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor;

EFI_STATUS find_kd_export(image* kdstub) {
    UINT64 addr;
    EFI_STATUS Status;

    Status = kdstub->img->FindExport(kdstub->img, "KdInitializeLibrary", &addr, NULL);

    if (EFI_ERROR(Status)) {
        print_error(L"FindExport", Status);
        return Status;
    }

    KdInitializeLibrary = (KD_INITIALIZE_LIBRARY*)(uintptr_t)addr;

    return EFI_SUCCESS;
}

#if defined(_X86_) || defined(__x86_64__)
__inline static void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__ (
        "mov dx, %0\n\t"
        "mov eax, %1\n\t"
        "out dx, eax\n\t"
        :
        : "" (port), "" (val)
        : "dx", "eax"
    );
}

__inline static uint32_t inl(uint16_t port) {
    uint32_t ret;

    __asm__ __volatile__ (
        "mov dx, %1\n\t"
        "in eax, dx\n\t"
        "mov %0, eax\n\t"
        : "=m" (ret)
        : "" (port)
        : "dx", "eax"
    );

    return ret;
}

#endif

static __stdcall NTSTATUS get_device_pci_data_by_offset(uint32_t bus, uint32_t slot, void* data, uint32_t offset, uint32_t length) {
    uint32_t address = 0x80000000 | ((bus & 0xff) << 16) | ((slot & 0x1f) << 11);

    if (offset % sizeof(uint32_t) == 0 && length % sizeof(uint32_t) == 0) {
        while (length > 0) {
            address &= 0xffffff00;
            address |= offset;

            outl(0xcf8, address);
            *(uint32_t*)data = inl(0xcfc);

            data = (uint8_t*)data + sizeof(uint32_t);
            offset += sizeof(uint32_t);
            length -= sizeof(uint32_t);
        }
    } else if (offset % sizeof(uint16_t) == 0 && length % sizeof(uint16_t) == 0) {
        while (length > 0) {
            uint32_t val;

            address &= 0xffffff00;
            address |= offset & 0xfc;

            outl(0xcf8, address);
            val = inl(0xcfc);

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

static __stdcall uint32_t read_register_ulong(void* addr) {
    return *(uint32_t*)addr;
}

static __stdcall NTSTATUS write_register_ulong(void* addr, uint32_t value) {
    *(uint32_t*)addr = value;

    return STATUS_SUCCESS;
}

static __stdcall void stall_cpu(int microseconds) {
    // FIXME - use RDTSC?
    //halt();
}

static __stdcall NTSTATUS write_port_ulong(uint16_t port, uint32_t value) {
    outl(port, value);

    return STATUS_SUCCESS;
}

static __stdcall void* get_physical_address(void* va) {
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
    UNUSED(va);

    // FIXME
    halt();

    return NULL;
#endif
}

EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd, uint8_t* scratch) {
    NTSTATUS Status;
    kdnet_exports exports;
    kd_funcs funcs;
    KD_NET_DATA kd_net_data;

    debug_device_descriptor = ddd;

    memset(&exports, 0, sizeof(exports));

    exports.unknown1 = 0x1d; // FIXME - ???
    exports.funcs = &funcs;
    exports.GetDevicePciDataByOffset = get_device_pci_data_by_offset;
    exports.KdStallExecutionProcessor = stall_cpu;
    exports.READ_REGISTER_ULONG = read_register_ulong;
    exports.WRITE_REGISTER_ULONG = write_register_ulong;
    exports.WRITE_PORT_ULONG = write_port_ulong;
    exports.GetPhysicalAddress = get_physical_address;

    funcs.count = 13; // number of functions

    Status = KdInitializeLibrary(&exports, NULL, ddd);
    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    // FIXME - call KdGetHardwareContextSize and allocate data (needs to be before paging enabled?)

    kd_net_data.scratch = scratch;
    kd_net_data.debug_device_descriptor = ddd;

    halt();

    Status = funcs.KdInitializeController(&kd_net_data);

    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    return EFI_SUCCESS;
}
