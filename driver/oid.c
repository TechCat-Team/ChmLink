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
    OID_802_3_XMIT_MORE_COLLISIONS, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER
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
    OID_GEN_RCV_NO_BUFFER, OID_GEN_STATISTICS, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER
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
    case OID_GEN_TRANSMIT_BLOCK_SIZE: value = a->Mode == CHMLINK_MODE_IP ? CHMLINK_IP_LIMIT : CHMLINK_ETHERNET_LIMIT; break;
    case OID_GEN_RECEIVE_BUFFER_SPACE:
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        value = (a->Mode == CHMLINK_MODE_IP ? CHMLINK_IP_LIMIT : CHMLINK_ETHERNET_LIMIT) * CHMLINK_QUEUE_PACKETS; break;
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

static NDIS_STATUS set(CL_ADAPTER *a, PNDIS_OID_REQUEST r)
{
    ULONG value, length = r->DATA.SET_INFORMATION.InformationBufferLength;
    PVOID input = r->DATA.SET_INFORMATION.InformationBuffer;
    KIRQL irql;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
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
