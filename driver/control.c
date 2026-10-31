#include "chmlink.h"

NDIS_STATUS ClRegisterControl(CL_ADAPTER *a, NET_IFINDEX index)
{
    NDIS_DEVICE_OBJECT_ATTRIBUTES attributes;
    PDRIVER_DISPATCH dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
    WCHAR nameBuffer[64], linkBuffer[64];
    UNICODE_STRING name, link, security;
    NTSTATUS status;
    status = RtlStringCchPrintfW(nameBuffer, RTL_NUMBER_OF(nameBuffer), L"\\Device\\ChmLink%lu", index);
    if (!NT_SUCCESS(status)) return status;
    status = RtlStringCchPrintfW(linkBuffer, RTL_NUMBER_OF(linkBuffer), L"\\DosDevices\\ChmLink%lu", index);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, nameBuffer);
    RtlInitUnicodeString(&link, linkBuffer);
    RtlInitUnicodeString(&security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NdisZeroMemory(dispatch, sizeof(dispatch));
    dispatch[IRP_MJ_CREATE] = ClDispatch;
    dispatch[IRP_MJ_CLEANUP] = ClDispatch;
    dispatch[IRP_MJ_CLOSE] = ClDispatch;
    dispatch[IRP_MJ_DEVICE_CONTROL] = ClDispatch;
    NdisZeroMemory(&attributes, sizeof(attributes));
    attributes.Header.Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
    attributes.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    attributes.Header.Size = NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    attributes.DeviceName = &name;
    attributes.SymbolicName = &link;
    attributes.MajorFunctions = dispatch;
    attributes.DefaultSDDLString = &security;
    status = NdisRegisterDeviceEx(ClDriverHandle, &attributes, &a->ControlObject, &a->ControlHandle);
    if (NT_SUCCESS(status)) a->ControlObject->Characteristics |= FILE_DEVICE_SECURE_OPEN;
    return status;
}

static uint32_t get_le32(const UCHAR *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(UCHAR *p, uint32_t value)
{
    p[0] = (UCHAR)value;
    p[1] = (UCHAR)(value >> 8);
    p[2] = (UCHAR)(value >> 16);
    p[3] = (UCHAR)(value >> 24);
}

/* SET_OFFLOAD, ControlMutex held and IoRundown held (indication needs valid
 * NDIS handles). */
static NTSTATUS set_offload(CL_ADAPTER *a, PIRP irp, ULONG input, ULONG output)
{
    UCHAR *bytes = irp->AssociatedIrp.SystemBuffer;
    ULONG granted;
    KIRQL irql;
    if (input != 4 || output != 4) return STATUS_INVALID_PARAMETER;
    if (get_le32(bytes) & ~(uint32_t)CHMLINK_OFFLOAD_ALL) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&a->Lock, &irql);
    a->OffloadSession = get_le32(bytes);
    granted = ClOffloadGrantedLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    ClIndicateOffload(a);
    put_le32(bytes, granted);
    irp->IoStatus.Information = 4;
    return STATUS_SUCCESS;
}

static ULONG ClStateFlagsLocked(const CL_ADAPTER *a)
{
    ULONG state = 0;
    switch (a->State) {
    case ClAdapterRunning: state = CHMLINK_STATE_RUNNING; break;
    case ClAdapterPaused: state = CHMLINK_STATE_PAUSED; break;
    case ClAdapterPausing: state = CHMLINK_STATE_PAUSING; break;
    case ClAdapterRemoving: state = CHMLINK_STATE_REMOVING; break;
    default: break;
    }
    if (a->SessionState == ClSessionOpen) state |= CHMLINK_STATE_SESSION;
    if (a->PowerState != NdisDeviceStateD0) state |= CHMLINK_STATE_LOW_POWER;
    if (a->ShuttingDown) state |= CHMLINK_STATE_SHUTDOWN;
    if (a->SurpriseRemoved) state = (state & ~(ULONG)CHMLINK_STATE_RUNNING) | CHMLINK_STATE_REMOVING;
    return state;
}

static NTSTATUS query(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    chmlink_query_info info;
    KIRQL irql;
    if (size < CHMLINK_INFO_SIZE) return STATUS_BUFFER_TOO_SMALL;
    NdisZeroMemory(&info, sizeof(info));
    info.capabilities = (a->Mode == CHMLINK_MODE_IP ? CHMLINK_CAP_IP : CHMLINK_CAP_ETHERNET) |
        CHMLINK_CAP_BATCH | CHMLINK_CAP_STATISTICS | CHMLINK_CAP_WAIT |
        (a->Mode == CHMLINK_MODE_IP ? 0 : CHMLINK_CAP_OFFLOAD);
    info.mode = (uint32_t)a->Mode;
    info.interface_index = a->InterfaceIndex;
    info.mtu = CHMLINK_ETHERNET_MTU;
    info.packet_limit = ClPacketLimit(a);
    if (a->Mode != CHMLINK_MODE_IP) NdisMoveMemory(info.mac, a->Mac, sizeof(info.mac));
    info.luid = a->Luid.Value;
    KeAcquireSpinLock(&a->Lock, &irql);
    info.state = ClStateFlagsLocked(a);
    info.offload = ClOffloadGrantedLocked(a);
    if (a->Session != NULL) {
        uint32_t head = chmlink_ring_load(&a->Session->From.View, CHMLINK_RING_HEAD_OFFSET);
        uint32_t tail = chmlink_ring_load(&a->Session->From.View, CHMLINK_RING_TAIL_OFFSET);
        uint32_t capacity = a->Session->From.Capacity;
        info.ring_capacity = capacity;
        /* head is written by the application; an invalid value reports nothing pending. */
        if (head < capacity && tail < capacity)
            info.ring_pending_bytes = tail >= head ? tail - head : capacity - head + tail;
    }
    info.packet_filter = a->PacketFilter;
    info.tx_packets = a->TxPackets;
    info.tx_bytes = a->TxBytes;
    info.tx_errors = a->TxErrors;
    info.rx_packets = a->RxPackets;
    info.rx_bytes = a->RxBytes;
    info.rx_discards = a->RxDiscards;
    info.tx_queue_drops = a->TxQueueDrops;
    KeReleaseSpinLock(&a->Lock, irql);
    chmlink_query_encode(irp->AssociatedIrp.SystemBuffer, &info);
    irp->IoStatus.Information = CHMLINK_INFO_SIZE;
    return STATUS_SUCCESS;
}

/* Resolve the adapter for an IRP under the global lock, which orders lookup
 * against cleanup and halt. CREATE finds the adapter owning the control
 * device. CLEANUP detaches the file context and inherits the file's own
 * reference. Every other IRP takes a dispatch reference. ClDispatch releases
 * the returned reference in all cases. */
static CL_ADAPTER *ClReferenceIrpAdapter(PDEVICE_OBJECT device, PIO_STACK_LOCATION stack)
{
    CL_ADAPTER *a = NULL;
    PLIST_ENTRY entry;
    KIRQL irql;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        for (entry = ClAdapters.Flink; entry != &ClAdapters; entry = entry->Flink) {
            CL_ADAPTER *candidate = CONTAINING_RECORD(entry, CL_ADAPTER, Link);
            if (candidate->ControlObject == device) { a = candidate; break; }
        }
    } else if (stack->FileObject != NULL) {
        a = stack->FileObject->FsContext;
        if (stack->MajorFunction == IRP_MJ_CLEANUP) stack->FileObject->FsContext = NULL;
    }
    if (a != NULL && stack->MajorFunction != IRP_MJ_CLEANUP) InterlockedIncrement(&a->References);
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    return a;
}

static BOOLEAN ClIsRemoved(CL_ADAPTER *a)
{
    BOOLEAN removed;
    KIRQL irql;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    removed = a->Removed || a->SurpriseRemoved;
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    return removed;
}

/* The file context owns a reference independently of the NDIS adapter.
 * FsContext2 marks the exclusive data session; NULL is inspection only. */
static void ClAttachFile(CL_ADAPTER *a, PFILE_OBJECT file, BOOLEAN session)
{
    KIRQL irql;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    file->FsContext = a;
    file->FsContext2 = session ? (PVOID)1 : NULL;
    InterlockedIncrement(&a->References);
    KeReleaseSpinLock(&ClAdaptersLock, irql);
}

/* ControlMutex held. Ends the data session owned by this file, if any.
 * After halt the rundown is complete and NDIS handles are gone, so the link
 * indication is skipped. */
static void ClCleanupFile(CL_ADAPTER *a, PFILE_OBJECT file)
{
    CL_SESSION *session;
    BOOLEAN rundown, offload_changed = FALSE;
    KIRQL irql;
    if (file->FsContext2 == NULL) return;
    rundown = ExAcquireRundownProtection(&a->IoRundown);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->SessionState = ClSessionClosing;
    session = a->Session;
    ClSessionStateLocked(a);
    if (a->OffloadSession != 0) { a->OffloadSession = 0; offload_changed = TRUE; }
    KeReleaseSpinLock(&a->Lock, irql);
    if (session != NULL) ClSessionTeardown(a, session);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->SessionState = ClSessionClosed;
    KeReleaseSpinLock(&a->Lock, irql);
    if (rundown) {
        ClIndicateLink(a);
        if (offload_changed) ClIndicateOffload(a);
        ExReleaseRundownProtection(&a->IoRundown);
    }
}

/* Checks shared by CREATE and DEVICE_CONTROL. A non-CREATE request must come
 * from a file whose context still names this adapter. */
static NTSTATUS ClCheckRequest(CL_ADAPTER *a, PIO_STACK_LOCATION stack)
{
    BOOLEAN owns;
    KIRQL irql;
    if (a == NULL) return STATUS_DEVICE_REMOVED;
    if (stack->MajorFunction != IRP_MJ_CREATE) {
        KeAcquireSpinLock(&ClAdaptersLock, &irql);
        owns = stack->FileObject != NULL && stack->FileObject->FsContext == a;
        KeReleaseSpinLock(&ClAdaptersLock, irql);
        if (!owns) return STATUS_FILE_CLOSED;
    }
    return ClIsRemoved(a) ? STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
}

/* ControlMutex held. Read-only opens are inspection handles. Read/write opens
 * claim the single data session and connect the link. */
static NTSTATUS ClCreate(CL_ADAPTER *a, PIO_STACK_LOCATION stack)
{
    PFILE_OBJECT file = stack->FileObject;
    NTSTATUS status;
    KIRQL irql;
    if (!(stack->Parameters.Create.SecurityContext->DesiredAccess & FILE_WRITE_DATA)) {
        if (file->FileName.Length != 0) return STATUS_OBJECT_NAME_NOT_FOUND;
        ClAttachFile(a, file, FALSE);
        return STATUS_SUCCESS;
    }
    if (!ExAcquireRundownProtection(&a->IoRundown)) return STATUS_DEVICE_NOT_READY;
    if (ClIsRemoved(a)) status = STATUS_DEVICE_REMOVED;
    else if (file->FileName.Length != 0) status = STATUS_OBJECT_NAME_NOT_FOUND;
    else {
        KeAcquireSpinLock(&a->Lock, &irql);
        if (a->SessionState != ClSessionClosed) status = STATUS_SHARING_VIOLATION;
        else { a->SessionState = ClSessionOpen; status = STATUS_SUCCESS; }
        KeReleaseSpinLock(&a->Lock, irql);
        if (NT_SUCCESS(status)) {
            ClAttachFile(a, file, TRUE);
            ClIndicateLink(a);
        }
    }
    ExReleaseRundownProtection(&a->IoRundown);
    return status;
}

/* Rundown held. Only REGISTER_RINGS and SET_OFFLOAD remain; both need the
 * caller's process context and NDIS handles, and must not race session
 * cleanup, so ClNeedsControlMutex serializes them. */
static NTSTATUS ClDataControl(CL_ADAPTER *a, PIRP irp, PIO_STACK_LOCATION stack)
{
    ULONG input = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output = stack->Parameters.DeviceIoControl.OutputBufferLength;
    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case CHMLINK_IOCTL_REGISTER_RINGS:
        return output != 0 ? STATUS_INVALID_PARAMETER : ClSessionRegister(a, irp, input);
    case CHMLINK_IOCTL_SET_OFFLOAD:
        return set_offload(a, irp, input, output);
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/* Metadata never touches NDIS handles or pools; the adapter reference keeps
 * the snapshot storage alive even when the data path is paused, so QUERY works
 * on any handle without rundown protection. */
static NTSTATUS ClControl(CL_ADAPTER *a, PIRP irp, PIO_STACK_LOCATION stack)
{
    NTSTATUS status;
    if (stack->Parameters.DeviceIoControl.IoControlCode == CHMLINK_IOCTL_QUERY) {
        if (stack->Parameters.DeviceIoControl.InputBufferLength != 0) return STATUS_INVALID_PARAMETER;
        return query(a, irp, stack->Parameters.DeviceIoControl.OutputBufferLength);
    }
    if (stack->FileObject->FsContext2 == NULL) return STATUS_ACCESS_DENIED;
    if (!ExAcquireRundownProtection(&a->IoRundown)) return STATUS_DEVICE_NOT_READY;
    status = ClIsRemoved(a) ? STATUS_DEVICE_REMOVED : ClDataControl(a, irp, stack);
    ExReleaseRundownProtection(&a->IoRundown);
    return status;
}

/* ControlMutex orders session open and close, ring registration, offload
 * configuration and link-state indications for one adapter. QUERY touches
 * only state guarded by the adapter spin lock and runs concurrently. */
static BOOLEAN ClNeedsControlMutex(PIO_STACK_LOCATION stack)
{
    if (stack->MajorFunction != IRP_MJ_DEVICE_CONTROL) return TRUE;
    return stack->Parameters.DeviceIoControl.IoControlCode != CHMLINK_IOCTL_QUERY;
}

_Use_decl_annotations_
NTSTATUS ClDispatch(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    CL_ADAPTER *a;
    NTSTATUS status;
    BOOLEAN serialized;
    irp->IoStatus.Information = 0;
    if (stack->MajorFunction == IRP_MJ_CREATE && stack->FileObject != NULL) {
        stack->FileObject->FsContext = NULL;
        stack->FileObject->FsContext2 = NULL;
    }
    a = ClReferenceIrpAdapter(device, stack);
    serialized = a != NULL && ClNeedsControlMutex(stack);
    if (serialized) KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
    switch (stack->MajorFunction) {
    case IRP_MJ_CLEANUP:
        if (a != NULL) ClCleanupFile(a, stack->FileObject);
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_CREATE:
        status = ClCheckRequest(a, stack);
        if (NT_SUCCESS(status)) status = ClCreate(a, stack);
        break;
    case IRP_MJ_DEVICE_CONTROL:
        status = ClCheckRequest(a, stack);
        if (NT_SUCCESS(status)) status = ClControl(a, irp, stack);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    if (serialized) KeReleaseMutex(&a->ControlMutex, FALSE);
    if (a != NULL) ClRelease(a);
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
