#include "quibble.h"
#include "misc.h"
#include "peload.h"

typedef int32_t NTSTATUS;
#define NT_SUCCESS(Status) ((Status)>=0)

typedef struct {
    uint32_t unknown;
    void* KdInitializeController;
    void* KdShutdownController;
    void* KdSetHibernateRange;
    void* KdGetRxPacket;
    void* KdReleaseRxPacket;
    void* KdGetTxPacket;
    void* KdSendTxPacket;
    void* KdGetPacketAddress;
    void* KdGetPacketLength;
    void* KdGetHardwareContextSize;
} kd_funcs;

typedef struct {
    uint32_t unknown1;
    uint32_t unknown2;
    kd_funcs* funcs;
} kd_struct;

typedef NTSTATUS (__stdcall KD_INITIALIZE_LIBRARY) ( // FIXME
    kd_struct* param1,
    void* param2,
    DEBUG_DEVICE_DESCRIPTOR* debug_device_descriptor
);

KD_INITIALIZE_LIBRARY* KdInitializeLibrary = NULL;

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

EFI_STATUS kdstub_init(DEBUG_DEVICE_DESCRIPTOR* ddd) {
    NTSTATUS Status;
    kd_struct kds;
    kd_funcs funcs;

    kds.unknown1 = 0x1d; // FIXME - ???
    kds.funcs = &funcs;
    funcs.unknown = 13; // FIXME - ???

    Status = KdInitializeLibrary(&kds, NULL, ddd); // FIXME
    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    // FIXME - call KdGetHardwareContextSize and allocate data (needs to be before paging enabled?)
    // FIXME - call KdInitializeController?

    return EFI_SUCCESS;
}
