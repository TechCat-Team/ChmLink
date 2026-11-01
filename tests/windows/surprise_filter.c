/* chmlink-surprise.sys: a test-only upper filter that makes Windows surprise-
 * remove the adapter below it, as when a device fails. Never shipped.
 *
 * tools/test-surprise.ps1 puts it in the UpperFilters of one test adapter.
 * The IOCTL on \\.\ChmLinkSurprise makes every filtered adapter answer the
 * next IRP_MN_QUERY_PNP_DEVICE_STATE with PNP_DEVICE_FAILED; Windows then
 * sends IRP_MN_SURPRISE_REMOVAL (NDIS: NdisDevicePnPEventSurpriseRemoved),
 * pauses and halts the miniport, and removes the stack once its handles are
 * closed. Everything else passes straight through. */
#include <ntddk.h>
#include <wdmsec.h>

#define SURPRISE_IOCTL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define MAX_FILTERS 16

typedef struct FILTER {
    BOOLEAN Control;
    PDEVICE_OBJECT Self, Lower, Pdo;
    IO_REMOVE_LOCK RemoveLock;
    volatile LONG Fail;
} FILTER;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE AddDevice;
static DRIVER_UNLOAD Unload;
_Dispatch_type_(IRP_MJ_PNP) static DRIVER_DISPATCH DispatchPnp;
_Dispatch_type_(IRP_MJ_POWER) static DRIVER_DISPATCH DispatchPower;
static DRIVER_DISPATCH DispatchPass;

/* An unsafe fast mutex inside a critical region keeps IRQL at PASSIVE_LEVEL,
 * which the device and symbolic-link calls under it need. */
static FAST_MUTEX Lock;
#define LOCK() (KeEnterCriticalRegion(), ExAcquireFastMutexUnsafe(&Lock))
#define UNLOCK() (ExReleaseFastMutexUnsafe(&Lock), KeLeaveCriticalRegion())
static PDEVICE_OBJECT ControlDevice;
static FILTER *Filters[MAX_FILTERS];

static NTSTATUS Complete(PIRP irp, NTSTATUS status, ULONG_PTR information)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS CreateControl(PDRIVER_OBJECT driver)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\ChmLinkSurprise");
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\ChmLinkSurprise");
    PDEVICE_OBJECT device;
    NTSTATUS status = IoCreateDeviceSecure(driver, sizeof(FILTER), &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                           FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, NULL, &device);
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(device->DeviceExtension, sizeof(FILTER));
    ((FILTER *)device->DeviceExtension)->Control = TRUE;
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(device); return status; }
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    ControlDevice = device;
    return STATUS_SUCCESS;
}

static VOID DeleteControl(VOID)
{
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\ChmLinkSurprise");
    if (ControlDevice == NULL) return;
    IoDeleteSymbolicLink(&link);
    IoDeleteDevice(ControlDevice);
    ControlDevice = NULL;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT device, lower;
    FILTER *filter;
    ULONG i;
    NTSTATUS status = IoCreateDevice(driver, sizeof(FILTER), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &device);
    if (!NT_SUCCESS(status)) return status;
    filter = device->DeviceExtension;
    RtlZeroMemory(filter, sizeof(*filter));
    filter->Self = device;
    filter->Pdo = pdo;
    IoInitializeRemoveLock(&filter->RemoveLock, 'fsLC', 0, 0);
    lower = IoAttachDeviceToDeviceStack(device, pdo);
    if (lower == NULL) { IoDeleteDevice(device); return STATUS_NO_SUCH_DEVICE; }
    filter->Lower = lower;
    device->DeviceType = lower->DeviceType;
    device->Characteristics = lower->Characteristics;
    device->Flags |= lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    LOCK();
    for (i = 0; i < MAX_FILTERS && Filters[i] != NULL; ++i) {}
    if (i < MAX_FILTERS) Filters[i] = filter;
    if (ControlDevice == NULL) (void)CreateControl(driver);
    UNLOCK();
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static NTSTATUS Forward(FILTER *filter, PIRP irp)
{
    NTSTATUS status = IoAcquireRemoveLock(&filter->RemoveLock, irp);
    if (!NT_SUCCESS(status)) return Complete(irp, status, 0);
    IoSkipCurrentIrpStackLocation(irp);
    status = IoCallDriver(filter->Lower, irp);
    IoReleaseRemoveLock(&filter->RemoveLock, irp);
    return status;
}

static NTSTATUS DispatchControl(PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG i, failed = 0;
    switch (stack->MajorFunction) {
    case IRP_MJ_CREATE: case IRP_MJ_CLOSE: case IRP_MJ_CLEANUP:
        return Complete(irp, STATUS_SUCCESS, 0);
    case IRP_MJ_DEVICE_CONTROL:
        if (stack->Parameters.DeviceIoControl.IoControlCode != SURPRISE_IOCTL) break;
        LOCK();
        for (i = 0; i < MAX_FILTERS; ++i) {
            if (Filters[i] == NULL) continue;
            InterlockedExchange(&Filters[i]->Fail, 1);
            IoInvalidateDeviceState(Filters[i]->Pdo);
            ++failed;
        }
        UNLOCK();
        return Complete(irp, failed ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE, 0);
    default:
        break;
    }
    return Complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS DispatchPass(PDEVICE_OBJECT device, PIRP irp)
{
    FILTER *filter = device->DeviceExtension;
    if (filter->Control) return DispatchControl(irp);
    return Forward(filter, irp);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT device, PIRP irp)
{
    FILTER *filter = device->DeviceExtension;
    NTSTATUS status;
    if (filter->Control) return Complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    status = IoAcquireRemoveLock(&filter->RemoveLock, irp);
    if (!NT_SUCCESS(status)) { PoStartNextPowerIrp(irp); return Complete(irp, status, 0); }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    status = PoCallDriver(filter->Lower, irp);
    IoReleaseRemoveLock(&filter->RemoveLock, irp);
    return status;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT device, PIRP irp)
{
    FILTER *filter = device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;
    ULONG i;
    BOOLEAN last = TRUE;
    if (filter->Control) return Complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    status = IoAcquireRemoveLock(&filter->RemoveLock, irp);
    if (!NT_SUCCESS(status)) return Complete(irp, status, 0);
    switch (stack->MinorFunction) {
    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        /* The state is filled in on the way up: ask the stack below first. */
        status = IoForwardIrpSynchronously(filter->Lower, irp) ? irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
        if (filter->Fail) {
            if (!NT_SUCCESS(status)) {
                irp->IoStatus.Information = 0;
                status = STATUS_SUCCESS;
            }
            irp->IoStatus.Information |= PNP_DEVICE_FAILED;
        }
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&filter->RemoveLock, irp);
        return status;
    case IRP_MN_REMOVE_DEVICE:
        IoReleaseRemoveLockAndWait(&filter->RemoveLock, irp);
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(filter->Lower, irp);
        LOCK();
        for (i = 0; i < MAX_FILTERS; ++i) {
            if (Filters[i] == filter) Filters[i] = NULL;
            else if (Filters[i] != NULL) last = FALSE;
        }
        if (last) DeleteControl();
        UNLOCK();
        IoDetachDevice(filter->Lower);
        IoDeleteDevice(device);
        return status;
    default:
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(filter->Lower, irp);
        IoReleaseRemoveLock(&filter->RemoveLock, irp);
        return status;
    }
}

static VOID Unload(PDRIVER_OBJECT driver)
{
    UNREFERENCED_PARAMETER(driver);
    LOCK();
    DeleteControl();
    UNLOCK();
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    ULONG i;
    UNREFERENCED_PARAMETER(registry);
    ExInitializeFastMutex(&Lock);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) driver->MajorFunction[i] = DispatchPass;
    driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
    driver->DriverExtension->AddDevice = AddDevice;
    driver->DriverUnload = Unload;
    return STATUS_SUCCESS;
}
