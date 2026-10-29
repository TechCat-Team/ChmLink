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

static NTSTATUS receive(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    chmlink_batch_writer writer;
    chmlink_result result;
    KIRQL irql;
    if (size > CHMLINK_BATCH_MAX_SIZE) return STATUS_INVALID_BUFFER_SIZE;
    result = chmlink_batch_begin(&writer, irp->AssociatedIrp.SystemBuffer, size,
                                CHMLINK_MODE_ETHERNET, CHMLINK_ETHERNET_LIMIT);
    if (result != CHMLINK_OK) return STATUS_BUFFER_TOO_SMALL;
    KeAcquireSpinLock(&a->Lock, &irql);
    while (a->Count != 0) {
        CL_PACKET *packet = &a->Queue[a->Head];
        result = chmlink_batch_append(&writer, packet->Bytes, packet->Length);
        if (result != CHMLINK_OK) break;
        a->Head = (a->Head + 1) % CHMLINK_QUEUE_PACKETS;
        a->Count--;
    }
    KeReleaseSpinLock(&a->Lock, irql);
    if (writer.count == 0) return result == CHMLINK_BUFFER_TOO_SMALL ?
        STATUS_BUFFER_TOO_SMALL : STATUS_NO_MORE_ENTRIES;
    irp->IoStatus.Information = writer.length;
    return STATUS_SUCCESS;
}

static BOOLEAN accepts(CL_ADAPTER *a, const UCHAR *frame)
{
    static const UCHAR broadcast[6] = {255, 255, 255, 255, 255, 255};
    ULONG i;
    if (a->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS) return TRUE;
    if (RtlCompareMemory(frame, broadcast, 6) == 6)
        return (a->PacketFilter & NDIS_PACKET_TYPE_BROADCAST) != 0;
    if (frame[0] & 1) {
        if (a->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST) return TRUE;
        if (!(a->PacketFilter & NDIS_PACKET_TYPE_MULTICAST)) return FALSE;
        for (i = 0; i < a->MulticastCount; ++i)
            if (RtlCompareMemory(frame, a->Multicast[i], 6) == 6) return TRUE;
        return FALSE;
    }
    return (a->PacketFilter & NDIS_PACKET_TYPE_DIRECTED) &&
           RtlCompareMemory(frame, a->Mac, 6) == 6;
}

static NTSTATUS inject(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    const UCHAR *bytes = irp->AssociatedIrp.SystemBuffer;
    uint32_t count;
    ULONG i, offset = CHMLINK_BATCH_HEADER_SIZE, accepted = 0;
    ULONG64 acceptedBytes = 0;
    PMDL mdl;
    PNET_BUFFER_LIST first = NULL, last = NULL, nbl;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;

    /* METHOD_BUFFERED provides the owned, nonpaged snapshot. */
    if (chmlink_batch_validate(bytes, size, CHMLINK_MODE_ETHERNET,
        CHMLINK_ETHERNET_LIMIT, &count) != CHMLINK_OK) return STATUS_INVALID_PARAMETER;
    mdl = IoAllocateMdl((PVOID)bytes, size, FALSE, FALSE, NULL);
    if (mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(mdl);
    for (i = 0; i < count; ++i) {
        ULONG length;
        BOOLEAN accept;
        /* Windows targets are little endian; avoid unaligned structure casts. */
        NdisMoveMemory(&length, bytes + offset, sizeof(length));
        offset += 4;
        KeAcquireSpinLock(&a->Lock, &irql);
        accept = accepts(a, bytes + offset);
        KeReleaseSpinLock(&a->Lock, irql);
        if (accept) {
            nbl = NdisAllocateNetBufferAndNetBufferList(a->Pool, 0, 0, mdl, offset, length);
            if (nbl == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            nbl->SourceHandle = a->Handle;
            NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
            if (last != NULL) NET_BUFFER_LIST_NEXT_NBL(last) = nbl;
            else first = nbl;
            last = nbl;
            accepted++;
            acceptedBytes += length;
        }
        offset += (length + 3) & ~3u;
    }
    /* Allocate everything before indicating any packet to prevent partial
     * injection on allocation failure. NDIS returns RESOURCES indications
     * synchronously; no MDL or NBL is retained by an upper protocol driver. */
    if (NT_SUCCESS(status) && first != NULL) {
        NdisMIndicateReceiveNetBufferLists(a->Handle, first, NDIS_DEFAULT_PORT_NUMBER,
                                         accepted, NDIS_RECEIVE_FLAGS_RESOURCES);
    }
    while (first != NULL) {
        nbl = NET_BUFFER_LIST_NEXT_NBL(first);
        NdisFreeNetBufferList(first);
        first = nbl;
    }
    IoFreeMdl(mdl);
    KeAcquireSpinLock(&a->Lock, &irql);
    if (NT_SUCCESS(status)) {
        a->RxPackets += accepted;
        a->RxBytes += acceptedBytes;
        a->RxDiscards += count - accepted;
    } else a->RxDiscards += count;
    KeReleaseSpinLock(&a->Lock, irql);
    return status;
}

_Use_decl_annotations_
NTSTATUS ClDispatch(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    CL_ADAPTER *a = NULL;
    PLIST_ENTRY entry;
    KIRQL irql;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    BOOLEAN rundown = FALSE;
    BOOLEAN locked = FALSE;
    irp->IoStatus.Information = 0;

    /* File contexts own a reference independently of the NDIS adapter. The
     * global lock also protects acquiring a reference against cleanup/halt. */
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

    if (a != NULL) {
        KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
        locked = TRUE;
    }

    if (stack->MajorFunction == IRP_MJ_CLOSE || stack->MajorFunction == IRP_MJ_CLEANUP) {
        status = STATUS_SUCCESS;
        if (a != NULL && stack->MajorFunction == IRP_MJ_CLEANUP) {
            rundown = ExAcquireRundownProtection(&a->IoRundown);
            KeAcquireSpinLock(&a->Lock, &irql);
            a->Session = FALSE;
            a->Head = a->Count = 0;
            KeReleaseSpinLock(&a->Lock, irql);
            if (rundown) ClIndicateLink(a);
        }
        goto complete;
    }
    if (a == NULL) { status = STATUS_DEVICE_REMOVED; goto complete; }
    if (stack->MajorFunction != IRP_MJ_CREATE) {
        BOOLEAN ownsSession;
        KeAcquireSpinLock(&ClAdaptersLock, &irql);
        ownsSession = stack->FileObject != NULL && stack->FileObject->FsContext == a;
        KeReleaseSpinLock(&ClAdaptersLock, irql);
        if (!ownsSession) { status = STATUS_FILE_CLOSED; goto complete; }
    }
    rundown = ExAcquireRundownProtection(&a->IoRundown);
    if (!rundown) { status = STATUS_DEVICE_NOT_READY; goto complete; }
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    if (a->Removed) status = STATUS_DEVICE_REMOVED;
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    if (status == STATUS_DEVICE_REMOVED) goto complete;
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        if (stack->FileObject->FileName.Length != 0) {
            status = STATUS_OBJECT_NAME_NOT_FOUND; goto complete;
        }
        KeAcquireSpinLock(&a->Lock, &irql);
        if (a->Session) status = STATUS_SHARING_VIOLATION;
        else { a->Session = TRUE; status = STATUS_SUCCESS; }
        KeReleaseSpinLock(&a->Lock, irql);
        if (NT_SUCCESS(status)) {
            KeAcquireSpinLock(&ClAdaptersLock, &irql);
            stack->FileObject->FsContext = a;
            InterlockedIncrement(&a->References);
            KeReleaseSpinLock(&ClAdaptersLock, irql);
            ClIndicateLink(a);
        }
    } else if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        BOOLEAN running;
        KeAcquireSpinLock(&a->Lock, &irql);
        running = a->Running && a->Session;
        KeReleaseSpinLock(&a->Lock, irql);
        if (!running) { status = STATUS_DEVICE_NOT_READY; goto complete; }
        switch (stack->Parameters.DeviceIoControl.IoControlCode) {
        case CHMLINK_IOCTL_RECEIVE:
            if (stack->Parameters.DeviceIoControl.InputBufferLength != 0) status = STATUS_INVALID_PARAMETER;
            else status = receive(a, irp, stack->Parameters.DeviceIoControl.OutputBufferLength);
            break;
        case CHMLINK_IOCTL_INJECT:
            if (stack->Parameters.DeviceIoControl.OutputBufferLength != 0) status = STATUS_INVALID_PARAMETER;
            else status = inject(a, irp, stack->Parameters.DeviceIoControl.InputBufferLength);
            break;
        }
    }
complete:
    if (rundown) ExReleaseRundownProtection(&a->IoRundown);
    if (locked) KeReleaseMutex(&a->ControlMutex, FALSE);
    if (a != NULL) ClRelease(a);
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
