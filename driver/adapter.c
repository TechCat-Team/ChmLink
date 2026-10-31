#include "chmlink.h"

void ClRelease(CL_ADAPTER *a)
{
    if (InterlockedDecrement(&a->References) == 0) {
        NT_ASSERT(a->Session == NULL);
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

/* PREfast cannot see through the reference count that frees the adapter in ClRelease, so every
 * early return below is reported as a leak of the adapter. Each failure path does release it:
 * References starts at 1 and ClRelease frees at zero. */
#pragma warning(disable: 28197)

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
    a->PowerState = NdisDeviceStateD0;
    a->OffloadAllowed = CL_OFFLOAD_FINE_ALL;
    KeInitializeSpinLock(&a->Lock);
    KeInitializeMutex(&a->ControlMutex, 0);
    KeInitializeEvent(&a->RxDrained, NotificationEvent, TRUE);
    ExInitializeRundownProtection(&a->IoRundown);
    status = ClReadConfiguration(handle, a->Configuration);
    if (status != NDIS_STATUS_SUCCESS) goto fail;
    a->Mode = (chmlink_mode)chmlink_config_mode(a->Configuration);
    /* chmlink_config_valid allows only 1500 for Ethernet and 1280..65535 for IP. */
    a->Mtu = chmlink_config_mtu(a->Configuration);
    a->Lookahead = a->Mtu;
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
    general.MtuSize = a->Mtu;
    general.MaxXmitLinkSpeed = general.MaxRcvLinkSpeed = 1000000000ULL;
    general.XmitLinkSpeed = general.RcvLinkSpeed = 1000000000ULL;
    general.MediaConnectState = MediaConnectStateDisconnected;
    general.MediaDuplexState = MediaDuplexStateFull;
    general.LookaheadSize = a->Mtu;
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
    status = ClSetOffloadAttributes(a);
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

#pragma warning(default: 28197)

_Use_decl_annotations_
NDIS_STATUS ClPause(NDIS_HANDLE context, PNDIS_MINIPORT_PAUSE_PARAMETERS parameters)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    UNREFERENCED_PARAMETER(parameters);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterPausing;
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    ExWaitForRundownProtectionRelease(&a->IoRundown);
    a->IoDrained = TRUE;
    /* NDIS requires every indicated NBL back before the pause completes. */
    if (InterlockedCompareExchange(&a->RxOutstanding, 0, 0) != 0)
        KeWaitForSingleObject(&a->RxDrained, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterPaused;
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS ClRestart(NDIS_HANDLE context, PNDIS_MINIPORT_RESTART_PARAMETERS parameters)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    BOOLEAN offload;
    UNREFERENCED_PARAMETER(parameters);
    if (a->IoDrained) {
        ExReInitializeRundownProtection(&a->IoRundown);
        a->IoDrained = FALSE;
    }
    KeAcquireSpinLock(&a->Lock, &irql);
    a->State = ClAdapterRunning;
    offload = a->OffloadSession != 0;
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
    ClIndicateLink(a);
    /* NDIS may forget the configuration across a pause; say it again. */
    if (offload) ClIndicateOffload(a);
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
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    /* Pause has normally run the rundown down already. A second wait on a
     * run-down reference is not a no-op: if the first wait had to block, the
     * reference still holds that (stale, on-stack) wait block, and a second
     * wait reads it as live protection and never returns. Wait only if no
     * pause did, as for a halt straight after initialization. */
    if (!a->IoDrained) {
        ExWaitForRundownProtectionRelease(&a->IoRundown);
        a->IoDrained = TRUE;
    }
    NdisDeregisterDeviceEx(a->ControlHandle);
    NdisFreeNetBufferListPool(a->Pool);
    ClRelease(a);
}

static void complete_large_send(PNET_BUFFER_LIST nbl)
{
    NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO info;
    info.Value = NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo);
    if (info.Transmit.Type != NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE) return;
    NdisZeroMemory(&info, sizeof(info));
    info.LsoV2TransmitComplete.Type = NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE;
    info.LsoV2TransmitComplete.Reserved = 0;
    NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo) = info.Value;
}

_Use_decl_annotations_
void ClSend(NDIS_HANDLE context, PNET_BUFFER_LIST lists, NDIS_PORT_NUMBER port, ULONG flags)
{
    CL_ADAPTER *a = context;
    CL_SESSION *session = NULL;
    PNET_BUFFER_LIST nbl;
    KIRQL irql, sendIrql;
    ULONG fine = 0;
    ULONG64 txPackets = 0, txBytes = 0, txErrors = 0, txDrops = 0;
    BOOLEAN acquired, wake = FALSE, broken = FALSE;
    uint32_t cursor, published;
    UNREFERENCED_PARAMETER(port);

    acquired = ExAcquireRundownProtection(&a->IoRundown);
    if (acquired) {
        KeAcquireSpinLock(&a->Lock, &irql);
        if (a->State == ClAdapterRunning && a->Session != NULL) {
            /* Take the send reference before releasing the lock: cleanup
             * clears a->Session under it and then drains this reference. */
            if (ExAcquireRundownProtection(&a->Session->SendRundown)) session = a->Session;
            if (session != NULL) fine = ClOffloadEffectiveLocked(a);
        }
        KeReleaseSpinLock(&a->Lock, irql);
    }
    if (session == NULL) {
        NDIS_STATUS status = acquired ? NDIS_STATUS_MEDIA_DISCONNECTED : NDIS_STATUS_PAUSED;
        for (nbl = lists; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
            NET_BUFFER_LIST_STATUS(nbl) = status;
            complete_large_send(nbl);
        }
        if (acquired) ExReleaseRundownProtection(&a->IoRundown);
        NdisMSendNetBufferListsComplete(a->Handle, lists,
            (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
        return;
    }

    KeAcquireSpinLock(&session->SendLock, &sendIrql);
    cursor = session->SendCursor;
    published = cursor;
    for (nbl = lists; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        PNET_BUFFER nb;
        NDIS_STATUS status = NDIS_STATUS_SUCCESS;
        BOOLEAN full = FALSE;
        ULONG64 segments = 0, bytes = 0;
        uint32_t start = cursor;
        for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL && status == NDIS_STATUS_SUCCESS;
             nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG length = NET_BUFFER_DATA_LENGTH(nb);
            chmlink_offload descriptor;
            chmlink_ring_status reserved;
            UCHAR *slot;
            PVOID source;
            if (length < ClPacketMinimum(a) || length > CHMLINK_PACKET_MAX_SIZE) {
                status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            reserved = chmlink_ring_reserve(&session->From.View, &cursor, length, NULL, &slot);
            if (reserved != CHMLINK_RING_OK) {
                status = reserved == CHMLINK_RING_FULL ? NDIS_STATUS_RESOURCES : NDIS_STATUS_INVALID_DATA;
                full = reserved == CHMLINK_RING_FULL;
                if (reserved == CHMLINK_RING_CORRUPT) broken = TRUE;
                break;
            }
            source = NdisGetDataBuffer(nb, length, slot, 1, 0);
            if (source == NULL) { status = NDIS_STATUS_RESOURCES; break; }
            if (source != slot) NdisMoveMemory(slot, source, length);
            if (!ClDescribeSend(a, nbl, slot, length, fine, &descriptor)) {
                status = NDIS_STATUS_INVALID_DATA;
                break;
            }
            chmlink_ring_describe(slot, &descriptor);
            if (a->Mode == CHMLINK_MODE_IP &&
                !chmlink_ip_packet_valid(slot, length,
                    descriptor.gso_type != CHMLINK_GSO_NONE ? CHMLINK_PACKET_MAX_SIZE : ClPacketLimit(a))) {
                status = NDIS_STATUS_INVALID_DATA;
                break;
            }
            bytes += length;
            segments += descriptor.gso_type != CHMLINK_GSO_NONE ?
                (length - descriptor.hdr_len + descriptor.gso_size - 1) / descriptor.gso_size : 1;
        }
        if (status != NDIS_STATUS_SUCCESS) {
            /* Unpublished records are simply abandoned by moving the cursor back. */
            cursor = start;
            txErrors++;
            if (full) {
                PNET_BUFFER nb2;
                for (nb2 = NET_BUFFER_LIST_FIRST_NB(nbl); nb2 != NULL; nb2 = NET_BUFFER_NEXT_NB(nb2))
                    txDrops++;
            }
        } else {
            txPackets += segments;
            txBytes += bytes;
        }
        NET_BUFFER_LIST_STATUS(nbl) = status;
        complete_large_send(nbl);
    }
    if (cursor != published) {
        session->SendCursor = cursor;
        wake = chmlink_ring_publish(&session->From.View, cursor) != 0;
    }
    KeReleaseSpinLock(&session->SendLock, sendIrql);
    if (wake) KeSetEvent(session->From.Event, IO_NO_INCREMENT, FALSE);
    if (broken) {
        /* The application broke the ring's head; stop using this session and
         * let it see CLOSED instead of failing every send silently. */
        session->Broken = TRUE;
        KeAcquireSpinLock(&a->Lock, &irql);
        ClSessionStateLocked(a);
        KeReleaseSpinLock(&a->Lock, irql);
    }
    ExReleaseRundownProtection(&session->SendRundown);

    if (txPackets != 0 || txBytes != 0 || txErrors != 0 || txDrops != 0) {
        KeAcquireSpinLock(&a->Lock, &irql);
        a->TxPackets += txPackets;
        a->TxBytes += txBytes;
        a->TxErrors += txErrors;
        a->TxQueueDrops += txDrops;
        KeReleaseSpinLock(&a->Lock, irql);
    }
    ExReleaseRundownProtection(&a->IoRundown);
    NdisMSendNetBufferListsComplete(a->Handle, lists,
        (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
}
