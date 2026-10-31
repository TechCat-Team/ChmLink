#include "chmlink.h"

const NDIS_OID ClOids[] = {
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_INTERRUPT_MODERATION, OID_GEN_LINK_PARAMETERS,
    OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_VENDOR_ID,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER, OID_GEN_STATISTICS,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER,
    OID_TCP_OFFLOAD_PARAMETERS, OID_OFFLOAD_ENCAPSULATION, OID_TCP_RSC_STATISTICS,
    OID_TCP_OFFLOAD_CURRENT_CONFIG, OID_TCP_OFFLOAD_HARDWARE_CAPABILITIES
};
const ULONG ClOidBytes = sizeof(ClOids);
const NDIS_OID ClIpOids[] = {
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_INTERRUPT_MODERATION, OID_GEN_LINK_PARAMETERS,
    OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_VENDOR_ID,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER, OID_GEN_STATISTICS, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER,
    OID_TCP_OFFLOAD_PARAMETERS, OID_OFFLOAD_ENCAPSULATION, OID_TCP_RSC_STATISTICS,
    OID_TCP_OFFLOAD_CURRENT_CONFIG, OID_TCP_OFFLOAD_HARDWARE_CAPABILITIES
};
const ULONG ClIpOidBytes = sizeof(ClIpOids);

static NDIS_STATUS query(CL_ADAPTER *a, PNDIS_OID_REQUEST r)
{
    ULONG value = 0, size = sizeof(value);
    ULONG64 counter = 0;
    const void *source = &value;
    NDIS_INTERRUPT_MODERATION_PARAMETERS moderation;
    NDIS_STATISTICS_INFO statistics;
    KIRQL irql;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;

    r->DATA.QUERY_INFORMATION.BytesWritten = 0;
    r->DATA.QUERY_INFORMATION.BytesNeeded = 0;
    if (a->Mode == CHMLINK_MODE_IP &&
        (r->DATA.QUERY_INFORMATION.Oid == OID_802_3_PERMANENT_ADDRESS ||
         r->DATA.QUERY_INFORMATION.Oid == OID_802_3_CURRENT_ADDRESS ||
         r->DATA.QUERY_INFORMATION.Oid == OID_802_3_MULTICAST_LIST ||
         r->DATA.QUERY_INFORMATION.Oid == OID_802_3_MAXIMUM_LIST_SIZE ||
         r->DATA.QUERY_INFORMATION.Oid == OID_802_3_RCV_ERROR_ALIGNMENT ||
         r->DATA.QUERY_INFORMATION.Oid == OID_802_3_XMIT_ONE_COLLISION ||
         r->DATA.QUERY_INFORMATION.Oid == OID_802_3_XMIT_MORE_COLLISIONS)) return NDIS_STATUS_NOT_SUPPORTED;
    KeAcquireSpinLock(&a->Lock, &irql);
    switch (r->DATA.QUERY_INFORMATION.Oid) {
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE: value = ClPacketLimit(a); break;
    case OID_GEN_RECEIVE_BUFFER_SPACE:
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        value = 4u * 1024u * 1024u; break;   /* the SDK's default ring capacity */
    case OID_GEN_VENDOR_DESCRIPTION: source = "ChmLink"; size = 8; break;
    case OID_GEN_VENDOR_DRIVER_VERSION: value = CHMLINK_VENDOR_DRIVER_VERSION; break;
    case OID_GEN_VENDOR_ID: value = 0xffffff; break;
    case OID_GEN_CURRENT_PACKET_FILTER: value = a->PacketFilter; break;
    case OID_GEN_CURRENT_LOOKAHEAD: value = a->Lookahead; break;
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS: source = a->Mac; size = 6; break;
    case OID_802_3_MULTICAST_LIST:
        source = a->Multicast; size = a->MulticastCount * 6; break;
    case OID_802_3_MAXIMUM_LIST_SIZE: value = CL_MULTICAST_MAX; break;
    case OID_GEN_XMIT_OK: counter = a->TxPackets; goto counter_value;
    case OID_GEN_RCV_OK: counter = a->RxPackets; goto counter_value;
    case OID_GEN_XMIT_ERROR: counter = a->TxErrors; goto counter_value;
    case OID_GEN_RCV_ERROR: counter = 0; goto counter_value;
    case OID_GEN_RCV_NO_BUFFER: counter = a->RxDiscards; goto counter_value;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS:
counter_value:
        source = &counter;
        size = r->DATA.QUERY_INFORMATION.InformationBufferLength >= 8 ? 8 : 4;
        break;
    case OID_GEN_INTERRUPT_MODERATION:
        NdisZeroMemory(&moderation, sizeof(moderation));
        moderation.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        moderation.Header.Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
        moderation.Header.Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
        moderation.InterruptModeration = NdisInterruptModerationNotSupported;
        source = &moderation; size = sizeof(moderation); break;
    case OID_GEN_STATISTICS:
        NdisZeroMemory(&statistics, sizeof(statistics));
        statistics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        statistics.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
        statistics.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
        statistics.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
            NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR |
            NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS;
        statistics.ifHCInOctets = a->RxBytes;
        statistics.ifHCOutOctets = a->TxBytes;
        statistics.ifOutErrors = a->TxErrors;
        statistics.ifInDiscards = a->RxDiscards;
        source = &statistics; size = sizeof(statistics); break;
    case OID_TCP_RSC_STATISTICS: {
        NDIS_RSC_STATISTICS_INFO rsc;
        NdisZeroMemory(&rsc, sizeof(rsc));
        rsc.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        rsc.Header.Revision = NDIS_RSC_STATISTICS_REVISION_1;
        rsc.Header.Size = NDIS_SIZEOF_RSC_STATISTICS_REVISION_1;
        rsc.CoalescedPkts = a->RscPackets;
        rsc.CoalescedOctets = a->RscOctets;
        rsc.CoalesceEvents = a->RscEvents;
        rsc.Aborts = 0;
        source = &rsc; size = sizeof(rsc); break;
    }
    case OID_TCP_OFFLOAD_CURRENT_CONFIG:
    case OID_TCP_OFFLOAD_HARDWARE_CAPABILITIES: {
        NDIS_OFFLOAD offload;
        ULONG fine = r->DATA.QUERY_INFORMATION.Oid == OID_TCP_OFFLOAD_CURRENT_CONFIG ?
            ClOffloadEffectiveLocked(a) : ClOffloadHardware(a);
        ClOffloadFill(a, &offload, fine);
        source = &offload; size = sizeof(offload); break;
    }
    case OID_PNP_QUERY_POWER: size = 0; break;
    default: status = NDIS_STATUS_NOT_SUPPORTED; break;
    }
    if (status == NDIS_STATUS_SUCCESS) {
        if (r->DATA.QUERY_INFORMATION.InformationBufferLength < size) {
            r->DATA.QUERY_INFORMATION.BytesNeeded = size;
            status = NDIS_STATUS_BUFFER_TOO_SHORT;
        } else {
            if (size != 0) NdisMoveMemory(r->DATA.QUERY_INFORMATION.InformationBuffer, source, size);
            r->DATA.QUERY_INFORMATION.BytesWritten = size;
        }
    }
    KeReleaseSpinLock(&a->Lock, irql);
    return status;
}

/* NDIS requires a new current configuration after OID_TCP_OFFLOAD_PARAMETERS
 * changes anything. The indication needs PASSIVE and ControlMutex, so a
 * request that arrives at DISPATCH hands the work to an I/O work item. */
typedef struct CL_INDICATE_WORK {
    NDIS_HANDLE Item;
    CL_ADAPTER *Adapter;
} CL_INDICATE_WORK;

static NDIS_IO_WORKITEM_FUNCTION indicate_offload_work;

_Use_decl_annotations_
static VOID indicate_offload_work(PVOID context, NDIS_HANDLE item)
{
    CL_INDICATE_WORK *work = context;
    CL_ADAPTER *a;
    if (work == NULL) { NdisFreeIoWorkItem(item); return; }   /* never: queued with a context */
    a = work->Adapter;
    if (ExAcquireRundownProtection(&a->IoRundown)) {
        KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
        ClIndicateOffload(a);
        KeReleaseMutex(&a->ControlMutex, FALSE);
        ExReleaseRundownProtection(&a->IoRundown);
    }
    NdisFreeIoWorkItem(item);
    ExFreePoolWithTag(work, CL_TAG);
    ClRelease(a);
}

/* The work item owns `work` and frees it; PREfast does not follow that hand-over (C6014). */
#pragma warning(push)
#pragma warning(disable: 6014)
static void queue_offload_indication(CL_ADAPTER *a)
{
    NDIS_HANDLE item;
    CL_INDICATE_WORK *work;
    item = NdisAllocateIoWorkItem(a->Handle);
    if (item == NULL) return;
    work = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*work), CL_TAG);
    if (work == NULL) { NdisFreeIoWorkItem(item); return; }
    work->Item = item;
    work->Adapter = a;
    InterlockedIncrement(&a->References);
    NdisQueueIoWorkItem(item, indicate_offload_work, work);
}
#pragma warning(pop)

static NDIS_STATUS offload_parameters(CL_ADAPTER *a, PNDIS_OID_REQUEST r)
{
    ULONG length = r->DATA.SET_INFORMATION.InformationBufferLength;
    BOOLEAN changed = FALSE;
    NDIS_STATUS status;
    KIRQL irql;
    r->DATA.SET_INFORMATION.BytesRead = 0;
    r->DATA.SET_INFORMATION.BytesNeeded = 0;
    KeAcquireSpinLock(&a->Lock, &irql);
    status = ClOffloadParametersLocked(a, r->DATA.SET_INFORMATION.InformationBuffer, length, &changed);
    KeReleaseSpinLock(&a->Lock, irql);
    if (status != NDIS_STATUS_SUCCESS) return status;
    r->DATA.SET_INFORMATION.BytesRead = length;
    if (!changed) return NDIS_STATUS_SUCCESS;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
        ClIndicateOffload(a);
        KeReleaseMutex(&a->ControlMutex, FALSE);
    } else {
        queue_offload_indication(a);
    }
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS offload_encapsulation(CL_ADAPTER *a, PNDIS_OID_REQUEST r)
{
    NDIS_OFFLOAD_ENCAPSULATION *e = r->DATA.SET_INFORMATION.InformationBuffer;
    ULONG length = r->DATA.SET_INFORMATION.InformationBufferLength;
    ULONG expected = a->Mode == CHMLINK_MODE_IP ? NDIS_ENCAPSULATION_NULL : NDIS_ENCAPSULATION_IEEE_802_3;
    r->DATA.SET_INFORMATION.BytesRead = 0;
    r->DATA.SET_INFORMATION.BytesNeeded = 0;
    if (e == NULL || length < NDIS_SIZEOF_OFFLOAD_ENCAPSULATION_REVISION_1) {
        r->DATA.SET_INFORMATION.BytesNeeded = NDIS_SIZEOF_OFFLOAD_ENCAPSULATION_REVISION_1;
        return NDIS_STATUS_INVALID_LENGTH;
    }
    if (e->Header.Type != NDIS_OBJECT_TYPE_OFFLOAD_ENCAPSULATION ||
        e->Header.Revision != NDIS_OFFLOAD_ENCAPSULATION_REVISION_1 ||
        e->Header.Size < NDIS_SIZEOF_OFFLOAD_ENCAPSULATION_REVISION_1)
        return NDIS_STATUS_INVALID_PARAMETER;
    if ((e->IPv4.Enabled == NDIS_OFFLOAD_SET_ON && e->IPv4.EncapsulationType != expected) ||
        (e->IPv6.Enabled == NDIS_OFFLOAD_SET_ON && e->IPv6.EncapsulationType != expected))
        return NDIS_STATUS_INVALID_PARAMETER;
    r->DATA.SET_INFORMATION.BytesRead = length;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS set(CL_ADAPTER *a, PNDIS_OID_REQUEST r)
{
    ULONG value, length = r->DATA.SET_INFORMATION.InformationBufferLength;
    PVOID input = r->DATA.SET_INFORMATION.InformationBuffer;
    KIRQL irql;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    if (r->DATA.SET_INFORMATION.Oid == OID_TCP_OFFLOAD_PARAMETERS)
        return offload_parameters(a, r);
    if (r->DATA.SET_INFORMATION.Oid == OID_OFFLOAD_ENCAPSULATION)
        return offload_encapsulation(a, r);
    r->DATA.SET_INFORMATION.BytesRead = 0;
    r->DATA.SET_INFORMATION.BytesNeeded = 0;
    if (r->DATA.SET_INFORMATION.Oid == OID_GEN_LINK_PARAMETERS ||
        r->DATA.SET_INFORMATION.Oid == OID_GEN_INTERRUPT_MODERATION)
        return NDIS_STATUS_NOT_SUPPORTED;
    KeAcquireSpinLock(&a->Lock, &irql);
    if (r->DATA.SET_INFORMATION.Oid == OID_802_3_MULTICAST_LIST && a->Mode != CHMLINK_MODE_IP) {
        if (length % 6 != 0) status = NDIS_STATUS_INVALID_LENGTH;
        else if (length > sizeof(a->Multicast)) status = NDIS_STATUS_MULTICAST_FULL;
        else {
            if (length != 0) NdisMoveMemory(a->Multicast, input, length);
            a->MulticastCount = length / 6;
        }
    } else if (r->DATA.SET_INFORMATION.Oid == OID_GEN_CURRENT_PACKET_FILTER ||
               r->DATA.SET_INFORMATION.Oid == OID_GEN_CURRENT_LOOKAHEAD ||
               r->DATA.SET_INFORMATION.Oid == OID_PNP_SET_POWER) {
        if (length != sizeof(value)) {
            r->DATA.SET_INFORMATION.BytesNeeded = sizeof(value);
            status = NDIS_STATUS_INVALID_LENGTH;
        } else {
            NdisMoveMemory(&value, input, sizeof(value));
            if (r->DATA.SET_INFORMATION.Oid == OID_GEN_CURRENT_PACKET_FILTER) {
                if (value & ~CL_FILTERS) status = NDIS_STATUS_NOT_SUPPORTED;
                else a->PacketFilter = value;
            } else if (r->DATA.SET_INFORMATION.Oid == OID_GEN_CURRENT_LOOKAHEAD) {
                if (value > CHMLINK_ETHERNET_MTU) status = NDIS_STATUS_INVALID_DATA;
                else a->Lookahead = value;
            } else if (value < NdisDeviceStateD0 || value > NdisDeviceStateD3) {
                status = NDIS_STATUS_INVALID_DATA;
            } else {
                /* NDIS pauses the adapter before a sleep state and restarts it
                 * after D0; the state only tells the session why it paused. */
                a->PowerState = (NDIS_DEVICE_POWER_STATE)value;
                /* A system that resumes after a shutdown notification (fast
                 * startup hibernates the kernel session) is running again. */
                if (value == NdisDeviceStateD0) a->ShuttingDown = FALSE;
                ClSessionStateLocked(a);
            }
        }
    } else status = NDIS_STATUS_NOT_SUPPORTED;
    if (status == NDIS_STATUS_SUCCESS) r->DATA.SET_INFORMATION.BytesRead = length;
    KeReleaseSpinLock(&a->Lock, irql);
    return status;
}

_Use_decl_annotations_
NDIS_STATUS ClOidRequest(NDIS_HANDLE context, PNDIS_OID_REQUEST request)
{
    if (request->RequestType == NdisRequestQueryInformation ||
        request->RequestType == NdisRequestQueryStatistics) return query(context, request);
    if (request->RequestType == NdisRequestSetInformation) return set(context, request);
    return NDIS_STATUS_NOT_SUPPORTED;
}
