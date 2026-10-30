#include "chmlink.h"

void ClUpdateReadEventLocked(CL_ADAPTER *a)
{
    if (a->ReadEvent != NULL) {
        if (a->Count != 0 || a->State != ClAdapterRunning || a->SessionState != ClSessionOpen)
            KeSetEvent(a->ReadEvent, IO_NO_INCREMENT, FALSE);
        else KeClearEvent(a->ReadEvent);
    }
}

void ClDetachReadEvent(CL_ADAPTER *a)
{
    PKEVENT event;
    KIRQL irql;
    KeAcquireSpinLock(&a->Lock, &irql);
    event = a->ReadEvent;
    a->ReadEvent = NULL;
    if (event != NULL) KeSetEvent(event, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&a->Lock, irql);
    if (event != NULL) ObDereferenceObject(event);
}

void ClRelease(CL_ADAPTER *a)
{
    if (InterlockedDecrement(&a->References) == 0) {
        NT_ASSERT(a->ReadEvent == NULL);
        if (a->Queue != NULL) ExFreePoolWithTag(a->Queue, CL_TAG);
        ExFreePoolWithTag(a, CL_TAG);
    }
}

void ClIndicateLink(CL_ADAPTER *a)
{
    NDIS_LINK_STATE link;
    NDIS_STATUS_INDICATION indication;
    KIRQL irql;
    NdisZeroMemory(&link, sizeof(link));
    link.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    link.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    link.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    KeAcquireSpinLock(&a->Lock, &irql);
    link.MediaConnectState = a->SessionState == ClSessionOpen ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    KeReleaseSpinLock(&a->Lock, irql);
    link.MediaDuplexState = MediaDuplexStateFull;
    link.XmitLinkSpeed = link.RcvLinkSpeed = 1000000000ULL;
    link.PauseFunctions = NdisPauseFunctionsUnsupported;
    NdisZeroMemory(&indication, sizeof(indication));
    indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    indication.SourceHandle = a->Handle;
    indication.StatusCode = NDIS_STATUS_LINK_STATE;
    indication.StatusBuffer = &link;
    indication.StatusBufferSize = sizeof(link);
    NdisMIndicateStatusEx(a->Handle, &indication);
}

_Use_decl_annotations_
NDIS_STATUS ClInitialize(NDIS_HANDLE handle, NDIS_HANDLE driverContext,
                        PNDIS_MINIPORT_INIT_PARAMETERS parameters)
{
    CL_ADAPTER *a;
    NDIS_STATUS status;
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES registration;
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES general;
    NET_BUFFER_LIST_POOL_PARAMETERS pool;
    NDIS_PM_CAPABILITIES power;
    KIRQL irql;
    PLIST_ENTRY entry;
    ULONG index = parameters->IfIndex;
    UNREFERENCED_PARAMETER(driverContext);

    a = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*a), CL_TAG);
    if (a == NULL) return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(a, sizeof(*a));
    a->References = 1;
    a->Handle = handle;
    a->InterfaceIndex = index;
    a->Luid = parameters->NetLuid;
    a->Lookahead = CHMLINK_ETHERNET_MTU;
    KeInitializeSpinLock(&a->Lock);
    KeInitializeMutex(&a->ControlMutex, 0);
    ExInitializeRundownProtection(&a->IoRundown);
    a->Queue = ExAllocatePoolWithTag(NonPagedPoolNx,
        sizeof(CL_PACKET) * CHMLINK_QUEUE_PACKETS, CL_TAG);
    if (a->Queue == NULL) { ClRelease(a); return NDIS_STATUS_RESOURCES; }
    status = ClReadConfiguration(handle, a->Configuration);
    if (status != NDIS_STATUS_SUCCESS) goto fail;
    a->Mode = (chmlink_mode)chmlink_config_mode(a->Configuration);
    NdisMoveMemory(a->Mac, a->Configuration + CHMLINK_CONFIG_MAC, sizeof(a->Mac));

    NdisZeroMemory(&registration, sizeof(registration));
    registration.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    registration.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.MiniportAdapterContext = a;
    registration.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND;
    registration.InterfaceType = NdisInterfaceInternal;
    status = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&registration);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    NdisZeroMemory(&power, sizeof(power));
    power.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    power.Header.Revision = NDIS_PM_CAPABILITIES_REVISION_1;
    power.Header.Size = NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1;
    NdisZeroMemory(&general, sizeof(general));
    general.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    general.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    general.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    general.MediaType = a->Mode == CHMLINK_MODE_IP ? NdisMediumIP : NdisMedium802_3;
    general.PhysicalMediumType = NdisPhysicalMediumUnspecified;
    general.MtuSize = CHMLINK_ETHERNET_MTU;
    general.MaxXmitLinkSpeed = general.MaxRcvLinkSpeed = 1000000000ULL;
    general.XmitLinkSpeed = general.RcvLinkSpeed = 1000000000ULL;
    general.MediaConnectState = MediaConnectStateDisconnected;
    general.MediaDuplexState = MediaDuplexStateFull;
    general.LookaheadSize = CHMLINK_ETHERNET_MTU;
    general.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
        NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    general.SupportedPacketFilters = CL_FILTERS;
    general.MaxMulticastListSize = a->Mode == CHMLINK_MODE_IP ? 0 : CL_MULTICAST_MAX;
    general.MacAddressLength = a->Mode == CHMLINK_MODE_IP ? 0 : 6;
    if (a->Mode != CHMLINK_MODE_IP) {
        NdisMoveMemory(general.PermanentMacAddress, a->Mac, sizeof(a->Mac));
        NdisMoveMemory(general.CurrentMacAddress, a->Mac, sizeof(a->Mac));
    }
    general.AccessType = a->Mode == CHMLINK_MODE_IP ? NET_IF_ACCESS_POINT_TO_POINT : NET_IF_ACCESS_BROADCAST;
    general.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    general.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    general.IfType = a->Mode == CHMLINK_MODE_IP ? IF_TYPE_PROP_VIRTUAL : IF_TYPE_ETHERNET_CSMACD;
    general.IfConnectorPresent = FALSE;
    general.SupportedOidList = (PNDIS_OID)(a->Mode == CHMLINK_MODE_IP ? ClIpOids : ClOids);
    general.SupportedOidListLength = a->Mode == CHMLINK_MODE_IP ? ClIpOidBytes : ClOidBytes;
    general.PowerManagementCapabilitiesEx = &power;
    general.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR |
        NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS;
    status = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&general);
    if (status != NDIS_STATUS_SUCCESS) goto fail;
    NdisZeroMemory(&pool, sizeof(pool));
    pool.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pool.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pool.fAllocateNetBuffer = TRUE;
    pool.PoolTag = CL_TAG;
    a->Pool = NdisAllocateNetBufferListPool(handle, &pool);
    if (a->Pool == NULL) { status = NDIS_STATUS_RESOURCES; goto fail; }
    status = ClRegisterControl(a, index);
    if (status != NDIS_STATUS_SUCCESS) goto fail;
    a->State = ClAdapterPaused;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    for (entry = ClAdapters.Flink; entry != &ClAdapters; entry = entry->Flink) {
        CL_ADAPTER *other = CONTAINING_RECORD(entry, CL_ADAPTER, Link);
        if ((other->Mode == CHMLINK_MODE_ETHERNET && a->Mode == CHMLINK_MODE_ETHERNET &&
             RtlCompareMemory(other->Mac, a->Mac, sizeof(a->Mac)) == sizeof(a->Mac)) ||
            RtlCompareMemory(other->Configuration + CHMLINK_CONFIG_ADAPTER_ID,
                             a->Configuration + CHMLINK_CONFIG_ADAPTER_ID,
                             CHMLINK_CONFIG_ID_SIZE) == CHMLINK_CONFIG_ID_SIZE) {
            KeReleaseSpinLock(&ClAdaptersLock, irql);
            NdisDeregisterDeviceEx(a->ControlHandle);
            status = NDIS_STATUS_INVALID_DATA;
            goto fail;
        }
    }
    InsertTailList(&ClAdapters, &a->Link);
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    return NDIS_STATUS_SUCCESS;
fail:
    if (a->Pool != NULL) NdisFreeNetBufferListPool(a->Pool);
    ClRelease(a);
    return status;
}

_Use_decl_annotations_
NDIS_STATUS ClPause(NDIS_HANDLE context, PNDIS_MINIPORT_PAUSE_PARAMETERS parameters)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(parameters);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterPausing;
    a->TxQueueDrops += a->Count;
    a->Count = a->Head = 0;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    ExWaitForRundownProtectionRelease(&a->IoRundown);
    a->IoDrained = TRUE;
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterPaused;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS ClRestart(NDIS_HANDLE context, PNDIS_MINIPORT_RESTART_PARAMETERS parameters)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(parameters);
    if (a->IoDrained) {
        ExReInitializeRundownProtection(&a->IoRundown);
        a->IoDrained = FALSE;
    }
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterRunning;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
    ClIndicateLink(a);
    KeReleaseMutex(&a->ControlMutex, FALSE);
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
void ClHalt(NDIS_HANDLE context, NDIS_HALT_ACTION action)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(action);
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    RemoveEntryList(&a->Link);
    a->Removed = TRUE;
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterRemoving;
    a->TxQueueDrops += a->Count;
    a->Count = a->Head = 0;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    ExWaitForRundownProtectionRelease(&a->IoRundown);
    ClDetachReadEvent(a);
    NdisDeregisterDeviceEx(a->ControlHandle);
    NdisFreeNetBufferListPool(a->Pool);
    ClRelease(a);
}

_Use_decl_annotations_
void ClSend(NDIS_HANDLE context, PNET_BUFFER_LIST lists, NDIS_PORT_NUMBER port, ULONG flags)
{
    CL_ADAPTER *a = context;
    PNET_BUFFER_LIST nbl;
    KIRQL irql;
    BOOLEAN acquired = ExAcquireRundownProtection(&a->IoRundown);
    UNREFERENCED_PARAMETER(port);
    for (nbl = lists; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        PNET_BUFFER nb;
        ULONG added = 0;
        ULONG64 bytes = 0;
        BOOLEAN queue_full = FALSE;
        NDIS_STATUS status = NDIS_STATUS_SUCCESS;
        KeAcquireSpinLock(&a->Lock, &irql);
        if (!acquired || a->State != ClAdapterRunning) status = NDIS_STATUS_PAUSED;
        else if (a->SessionState != ClSessionOpen) status = NDIS_STATUS_MEDIA_DISCONNECTED;
        for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL && status == NDIS_STATUS_SUCCESS;
             nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG length = NET_BUFFER_DATA_LENGTH(nb);
            CL_PACKET *packet;
            PVOID source;
            if (length < ClPacketMinimum(a) || length > ClPacketLimit(a)) {
                status = NDIS_STATUS_INVALID_LENGTH; break;
            }
            if (a->Count + added >= CHMLINK_QUEUE_PACKETS) {
                queue_full = TRUE;
                status = NDIS_STATUS_RESOURCES; break;
            }
            packet = &a->Queue[(a->Head + a->Count + added) % CHMLINK_QUEUE_PACKETS];
            source = NdisGetDataBuffer(nb, length, packet->Bytes, 1, 0);
            if (source == NULL) { status = NDIS_STATUS_RESOURCES; break; }
            if (source != packet->Bytes) NdisMoveMemory(packet->Bytes, source, length);
            if (a->Mode == CHMLINK_MODE_IP &&
                !chmlink_ip_packet_valid(packet->Bytes, length, CHMLINK_IP_LIMIT)) {
                status = NDIS_STATUS_INVALID_DATA; break;
            }
            packet->Length = length;
            added++;
            bytes += length;
        }
        if (status == NDIS_STATUS_SUCCESS) {
            a->Count += added;
            a->TxPackets += added;
            a->TxBytes += bytes;
        } else {
            a->TxErrors++;
            if (queue_full) {
                /* Nothing from this NBL was published; account for the whole
                 * rejected chain, not just its first overflowing packet. */
                for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL; nb = NET_BUFFER_NEXT_NB(nb))
                    a->TxQueueDrops++;
            }
        }
        ClUpdateReadEventLocked(a);
        KeReleaseSpinLock(&a->Lock, irql);
        NET_BUFFER_LIST_STATUS(nbl) = status;
    }
    NdisMSendNetBufferListsComplete(a->Handle, lists,
        (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
    if (acquired) ExReleaseRundownProtection(&a->IoRundown);
}
