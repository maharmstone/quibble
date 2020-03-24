#include "quibble.h"
#include "misc.h"
#include "peload.h"

typedef int32_t NTSTATUS;
#define NT_SUCCESS(Status) ((Status)>=0)

typedef NTSTATUS (__stdcall KD_INITIALIZE_LIBRARY) ( // FIXME
    void* param1,
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

    Status = KdInitializeLibrary(NULL, NULL, ddd); // FIXME
    if (!NT_SUCCESS(Status))
        return EFI_INVALID_PARAMETER;

    return EFI_SUCCESS;
}
