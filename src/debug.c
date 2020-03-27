#include "quibble.h"
#include "misc.h"
#include "peload.h"

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

typedef uint32_t (__stdcall *READ_REGISTER_ULONG) (
    void* addr
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
    void* KdNetImports;
    void* KdStallExecutionProcessor;
    void* READ_REGISTER_UCHAR;
    void* READ_REGISTER_USHORT;
    READ_REGISTER_ULONG READ_REGISTER_ULONG;
    void* READ_REGISTER_ULONG64;
    void* WRITE_REGISTER_ULONG64;
    void* unknown7;
    void* unknown8;
    void* unknown9;
    void* unknown10;
    void* unknown11;
    void* unknown12;
    void* unknown13;
    void* unknown14;
    void* unknown15;
    void* unknown16;
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

    // FIXME - is this guaranteed to only operate on 32-bit units?

    while (length > 0) {
        address &= 0xffffff00;
        address |= offset;

        outl(0xcf8, address);
        *(uint32_t*)data = inl(0xcfc);

        data = (uint8_t*)data + sizeof(uint32_t);
        offset += sizeof(uint32_t);
        length -= sizeof(uint32_t);
    }

    return STATUS_SUCCESS;
}

static __stdcall uint32_t read_register_ulong(void* addr) {
    return *(uint32_t*)addr;
}

EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd, uint8_t* scratch) {
    NTSTATUS Status;
    kdnet_exports exports;
    kd_funcs funcs;
    KD_NET_DATA kd_net_data;

    debug_device_descriptor = ddd;

    exports.unknown1 = 0x1d; // FIXME - ???
    exports.funcs = &funcs;
    exports.GetDevicePciDataByOffset = get_device_pci_data_by_offset;
    exports.READ_REGISTER_ULONG = read_register_ulong;

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
