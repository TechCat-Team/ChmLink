#include "chmlink.h"

void ClRelease(CL_ADAPTER *a)
{
    if (InterlockedDecrement(&a->References) == 0) {
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
    link.MediaConnectState = a->Session ? MediaConnectStateConnected : MediaConnectStateDisconnected;
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
    ULONG index = parameters->IfIndex;
    UNREFERENCED_PARAMETER(driverContext);

    a = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*a), CL_TAG);
    if (a == NULL) return NDIS_STATUS_RESOURCES;
    NdisZeroMemory(a, sizeof(*a));
    a->References = 1;
    a->Handle = handle;
    a->Lookahead = CHMLINK_ETHERNET_MTU;
    KeInitializeSpinLock(&a->Lock);
    KeInitializeMutex(&a->ControlMutex, 0);
    ExInitializeRundownProtection(&a->IoRundown);
    a->Queue = ExAllocatePoolWithTag(NonPagedPoolNx,
        sizeof(CL_PACKET) * CHMLINK_QUEUE_PACKETS, CL_TAG);
    if (a->Queue == NULL) { ClRelease(a); return NDIS_STATUS_RESOURCES; }
    a->Mac[0] = 0x02;
    a->Mac[1] = 0x43;
    a->Mac[2] = (UCHAR)(index >> 24);
    a->Mac[3] = (UCHAR)(index >> 16);
    a->Mac[4] = (UCHAR)(index >> 8);
    a->Mac[5] = (UCHAR)index;

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
    general.MediaType = NdisMedium802_3;
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
    general.MaxMulticastListSize = CL_MULTICAST_MAX;
    general.MacAddressLength = 6;
    general.PermanentMacAddress = a->Mac;
    general.CurrentMacAddress = a->Mac;
    general.AccessType = NET_IF_ACCESS_BROADCAST;
    general.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    general.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    general.IfType = IF_TYPE_ETHERNET_CSMACD;
    general.IfConnectorPresent = FALSE;
    general.SupportedOidList = (PNDIS_OID)ClOids;
    general.SupportedOidListLength = ClOidBytes;
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
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    InsertTailList(&ClAdapters, &a->Link);
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    return NDIS_STATUS_SUCCESS;
fail:
    if (a->Pool != NULL) NdisFreeNetBufferListPool(a->Pool);
    ClRelease(a);
    return status;
}

NDIS_STATUS ClPause(NDIS_HANDLE context, PNDIS_MINIPORT_PAUSE_PARAMETERS parameters)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(parameters);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->Running = FALSE;
    a->Count = a->Head = 0;
    KeReleaseSpinLock(&a->Lock, irql);
    ExWaitForRundownProtectionRelease(&a->IoRundown);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS ClRestart(NDIS_HANDLE context, PNDIS_MINIPORT_RESTART_PARAMETERS parameters)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(parameters);
    /* Initial initialization is paused, but its rundown has not run yet. */
    if (!ExAcquireRundownProtection(&a->IoRundown)) {
        ExReInitializeRundownProtection(&a->IoRundown);
    } else {
        ExReleaseRundownProtection(&a->IoRundown);
    }
    KeAcquireSpinLock(&a->Lock, &irql);
    a->Running = TRUE;
    KeReleaseSpinLock(&a->Lock, irql);
    ClIndicateLink(a);
    return NDIS_STATUS_SUCCESS;
}

void ClHalt(NDIS_HANDLE context, NDIS_HALT_ACTION action)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(action);
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    RemoveEntryList(&a->Link);
    a->Removed = TRUE;
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    ExWaitForRundownProtectionRelease(&a->IoRundown);
    NdisDeregisterDeviceEx(a->ControlHandle);
    NdisFreeNetBufferListPool(a->Pool);
    ClRelease(a);
}

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
        NDIS_STATUS status = NDIS_STATUS_SUCCESS;
        KeAcquireSpinLock(&a->Lock, &irql);
        if (!acquired || !a->Running) status = NDIS_STATUS_PAUSED;
        else if (!a->Session) status = NDIS_STATUS_MEDIA_DISCONNECTED;
        for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL && status == NDIS_STATUS_SUCCESS;
             nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG length = NET_BUFFER_DATA_LENGTH(nb);
            CL_PACKET *packet;
            PVOID source;
            if (length < 14 || length > CHMLINK_ETHERNET_LIMIT) {
                status = NDIS_STATUS_INVALID_LENGTH; break;
            }
            if (a->Count + added >= CHMLINK_QUEUE_PACKETS) {
                status = NDIS_STATUS_RESOURCES; break;
            }
            packet = &a->Queue[(a->Head + a->Count + added) % CHMLINK_QUEUE_PACKETS];
            source = NdisGetDataBuffer(nb, length, packet->Bytes, 1, 0);
            if (source == NULL) { status = NDIS_STATUS_RESOURCES; break; }
            if (source != packet->Bytes) NdisMoveMemory(packet->Bytes, source, length);
            packet->Length = length;
            added++;
            bytes += length;
        }
        if (status == NDIS_STATUS_SUCCESS) {
            a->Count += added;
            a->TxPackets += added;
            a->TxBytes += bytes;
        } else a->TxErrors++;
        KeReleaseSpinLock(&a->Lock, irql);
        NET_BUFFER_LIST_STATUS(nbl) = status;
    }
    NdisMSendNetBufferListsComplete(a->Handle, lists,
        (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
    if (acquired) ExReleaseRundownProtection(&a->IoRundown);
}
