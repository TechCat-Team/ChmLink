#include "chmlink.h"
#include "chmlink/offload.h"

/* Task offloads, Ethernet mode only: the Windows TCP/IP stack does not use
 * task offloads on NdisMediumIP interfaces (measured on Windows 11 26200: the
 * same offload configuration is applied to an Ethernet-mode adapter and
 * ignored on an IP-mode one), so an IP-mode adapter advertises none and
 * grants none.
 *
 * The adapter advertises what it can do at initialization,
 * with every offload off. A session that can handle descriptors switches them
 * on with CHMLINK_IOCTL_SET_OFFLOAD; the driver then indicates the new current
 * configuration to the stack, and off again when the session ends. The stack
 * or an administrator can switch single offloads off through
 * OID_TCP_OFFLOAD_PARAMETERS; those are kept in OffloadAllowed. */

#define CL_LSO_MAX_SIZE 64000u

ULONG ClOffloadHardware(const CL_ADAPTER *a)
{
    return a->Mode == CHMLINK_MODE_IP ? 0 : CL_OFFLOAD_FINE_ALL;
}

static ULONG expand(ULONG session)
{
    ULONG fine = 0;
    if (session & CHMLINK_OFFLOAD_TX_CHECKSUM) fine |= CL_TX_TCP4 | CL_TX_UDP4 | CL_TX_TCP6 | CL_TX_UDP6;
    if ((session & CHMLINK_OFFLOAD_TX_LSO) && (session & CHMLINK_OFFLOAD_TX_CHECKSUM)) fine |= CL_LSO4 | CL_LSO6;
    if (session & CHMLINK_OFFLOAD_RX_CHECKSUM) fine |= CL_RX_IP4 | CL_RX_TCP4 | CL_RX_UDP4 | CL_RX_TCP6 | CL_RX_UDP6;
    if ((session & CHMLINK_OFFLOAD_RX_RSC) && (session & CHMLINK_OFFLOAD_RX_CHECKSUM)) fine |= CL_RSC4 | CL_RSC6;
    return fine;
}

ULONG ClOffloadEffectiveLocked(const CL_ADAPTER *a)
{
    return a->OffloadAllowed & expand(a->OffloadSession) & ClOffloadHardware(a);
}

ULONG ClOffloadGrantedLocked(const CL_ADAPTER *a)
{
    ULONG fine = ClOffloadEffectiveLocked(a), coarse = 0;
    if (fine & (CL_TX_TCP4 | CL_TX_UDP4 | CL_TX_TCP6 | CL_TX_UDP6)) coarse |= CHMLINK_OFFLOAD_TX_CHECKSUM;
    if (fine & (CL_LSO4 | CL_LSO6)) coarse |= CHMLINK_OFFLOAD_TX_LSO;
    if (fine & (CL_RX_IP4 | CL_RX_TCP4 | CL_RX_UDP4 | CL_RX_TCP6 | CL_RX_UDP6)) coarse |= CHMLINK_OFFLOAD_RX_CHECKSUM;
    if (fine & (CL_RSC4 | CL_RSC6)) coarse |= CHMLINK_OFFLOAD_RX_RSC;
    return coarse;
}

static ULONG on(ULONG fine, ULONG bit) { return (fine & bit) ? NDIS_OFFLOAD_SUPPORTED : NDIS_OFFLOAD_NOT_SUPPORTED; }

/* `fine` = the CL_* offloads to describe as on. */
static void fill(const CL_ADAPTER *a, NDIS_OFFLOAD *o, ULONG fine)
{
    ULONG encapsulation = a->Mode == CHMLINK_MODE_IP ? NDIS_ENCAPSULATION_NULL : NDIS_ENCAPSULATION_IEEE_802_3;
    NdisZeroMemory(o, sizeof(*o));
    o->Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
    o->Header.Revision = NDIS_OFFLOAD_REVISION_3;
    o->Header.Size = NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_3;
    if (fine & (CL_TX_TCP4 | CL_TX_UDP4)) {
        o->Checksum.IPv4Transmit.Encapsulation = encapsulation;
        o->Checksum.IPv4Transmit.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv4Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv4Transmit.TcpChecksum = on(fine, CL_TX_TCP4);
        o->Checksum.IPv4Transmit.UdpChecksum = on(fine, CL_TX_UDP4);
    }
    if (fine & (CL_RX_IP4 | CL_RX_TCP4 | CL_RX_UDP4)) {
        o->Checksum.IPv4Receive.Encapsulation = encapsulation;
        o->Checksum.IPv4Receive.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv4Receive.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv4Receive.TcpChecksum = on(fine, CL_RX_TCP4);
        o->Checksum.IPv4Receive.UdpChecksum = on(fine, CL_RX_UDP4);
        o->Checksum.IPv4Receive.IpChecksum = on(fine, CL_RX_IP4);
    }
    if (fine & (CL_TX_TCP6 | CL_TX_UDP6)) {
        o->Checksum.IPv6Transmit.Encapsulation = encapsulation;
        o->Checksum.IPv6Transmit.IpExtensionHeadersSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv6Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv6Transmit.TcpChecksum = on(fine, CL_TX_TCP6);
        o->Checksum.IPv6Transmit.UdpChecksum = on(fine, CL_TX_UDP6);
    }
    if (fine & (CL_RX_TCP6 | CL_RX_UDP6)) {
        o->Checksum.IPv6Receive.Encapsulation = encapsulation;
        o->Checksum.IPv6Receive.IpExtensionHeadersSupported = NDIS_OFFLOAD_NOT_SUPPORTED;
        o->Checksum.IPv6Receive.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
        o->Checksum.IPv6Receive.TcpChecksum = on(fine, CL_RX_TCP6);
        o->Checksum.IPv6Receive.UdpChecksum = on(fine, CL_RX_UDP6);
    }
    if (fine & CL_LSO4) {
        o->LsoV2.IPv4.Encapsulation = encapsulation;
        o->LsoV2.IPv4.MaxOffLoadSize = CL_LSO_MAX_SIZE;
        o->LsoV2.IPv4.MinSegmentCount = 2;
    }
    if (fine & CL_LSO6) {
        o->LsoV2.IPv6.Encapsulation = encapsulation;
        o->LsoV2.IPv6.MaxOffLoadSize = CL_LSO_MAX_SIZE;
        o->LsoV2.IPv6.MinSegmentCount = 2;
        o->LsoV2.IPv6.IpExtensionHeadersSupported = NDIS_OFFLOAD_NOT_SUPPORTED;
        o->LsoV2.IPv6.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    }
    o->Rsc.IPv4.Enabled = (fine & CL_RSC4) != 0;
    o->Rsc.IPv6.Enabled = (fine & CL_RSC6) != 0;
}

void ClOffloadFill(const CL_ADAPTER *a, NDIS_OFFLOAD *offload, ULONG fine)
{
    fill(a, offload, fine);
}

NDIS_STATUS ClSetOffloadAttributes(CL_ADAPTER *a)
{
    NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES attributes;
    NDIS_OFFLOAD current, hardware;
    fill(a, &current, 0);
    fill(a, &hardware, ClOffloadHardware(a));
    NdisZeroMemory(&attributes, sizeof(attributes));
    attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES;
    attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1;
    attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1;
    attributes.DefaultOffloadConfiguration = &current;
    attributes.HardwareOffloadCapabilities = &hardware;
    return NdisMSetMiniportAttributes(a->Handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&attributes);
}

/* PASSIVE_LEVEL, ControlMutex held, the NDIS handle valid (rundown held or
 * inside an NDIS callback). Tells the stack what is on now. */
void ClIndicateOffload(CL_ADAPTER *a)
{
    NDIS_OFFLOAD offload;
    NDIS_STATUS_INDICATION indication;
    KIRQL irql;
    ULONG fine;
    KeAcquireSpinLock(&a->Lock, &irql);
    fine = ClOffloadEffectiveLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    fill(a, &offload, fine);
    NdisZeroMemory(&indication, sizeof(indication));
    indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    indication.SourceHandle = a->Handle;
    indication.StatusCode = NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG;
    indication.StatusBuffer = &offload;
    indication.StatusBufferSize = sizeof(offload);
    NdisMIndicateStatusEx(a->Handle, &indication);
}

/* Apply one NDIS_OFFLOAD_PARAMETERS checksum value to a TX bit and an RX bit. */
static void apply_checksum(ULONG *allowed, UCHAR value, ULONG tx, ULONG rx)
{
    switch (value) {
    case NDIS_OFFLOAD_PARAMETERS_TX_RX_DISABLED: *allowed &= ~(tx | rx); break;
    case NDIS_OFFLOAD_PARAMETERS_TX_ENABLED_RX_DISABLED: *allowed = (*allowed | tx) & ~rx; break;
    case NDIS_OFFLOAD_PARAMETERS_RX_ENABLED_TX_DISABLED: *allowed = (*allowed | rx) & ~tx; break;
    case NDIS_OFFLOAD_PARAMETERS_TX_RX_ENABLED: *allowed |= tx | rx; break;
    default: break;   /* NO_CHANGE */
    }
}

/* OID_TCP_OFFLOAD_PARAMETERS. Lock held. *changed tells the caller to
 * indicate the new configuration once the lock is released. */
NDIS_STATUS ClOffloadParametersLocked(CL_ADAPTER *a, const void *buffer, ULONG length, BOOLEAN *changed)
{
    NDIS_OFFLOAD_PARAMETERS p;
    ULONG allowed = a->OffloadAllowed;
    *changed = FALSE;
    if (buffer == NULL || length < NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_1) return NDIS_STATUS_INVALID_LENGTH;
    NdisZeroMemory(&p, sizeof(p));
    NdisMoveMemory(&p, buffer, min(length, (ULONG)sizeof(p)));
    if (p.IPv4Checksum == NDIS_OFFLOAD_PARAMETERS_TX_RX_DISABLED ||
        p.IPv4Checksum == NDIS_OFFLOAD_PARAMETERS_TX_ENABLED_RX_DISABLED) allowed &= ~CL_RX_IP4;
    else if (p.IPv4Checksum == NDIS_OFFLOAD_PARAMETERS_RX_ENABLED_TX_DISABLED ||
             p.IPv4Checksum == NDIS_OFFLOAD_PARAMETERS_TX_RX_ENABLED) allowed |= CL_RX_IP4;
    apply_checksum(&allowed, p.TCPIPv4Checksum, CL_TX_TCP4, CL_RX_TCP4);
    apply_checksum(&allowed, p.UDPIPv4Checksum, CL_TX_UDP4, CL_RX_UDP4);
    apply_checksum(&allowed, p.TCPIPv6Checksum, CL_TX_TCP6, CL_RX_TCP6);
    apply_checksum(&allowed, p.UDPIPv6Checksum, CL_TX_UDP6, CL_RX_UDP6);
    if (p.LsoV2IPv4 == NDIS_OFFLOAD_PARAMETERS_LSOV2_DISABLED) allowed &= ~CL_LSO4;
    else if (p.LsoV2IPv4 == NDIS_OFFLOAD_PARAMETERS_LSOV2_ENABLED) allowed |= CL_LSO4;
    if (p.LsoV2IPv6 == NDIS_OFFLOAD_PARAMETERS_LSOV2_DISABLED) allowed &= ~CL_LSO6;
    else if (p.LsoV2IPv6 == NDIS_OFFLOAD_PARAMETERS_LSOV2_ENABLED) allowed |= CL_LSO6;
    if (length >= NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_3 && p.Header.Revision >= NDIS_OFFLOAD_PARAMETERS_REVISION_3) {
        if (p.RscIPv4 == NDIS_OFFLOAD_PARAMETERS_RSC_DISABLED) allowed &= ~CL_RSC4;
        else if (p.RscIPv4 == NDIS_OFFLOAD_PARAMETERS_RSC_ENABLED) allowed |= CL_RSC4;
        if (p.RscIPv6 == NDIS_OFFLOAD_PARAMETERS_RSC_DISABLED) allowed &= ~CL_RSC6;
        else if (p.RscIPv6 == NDIS_OFFLOAD_PARAMETERS_RSC_ENABLED) allowed |= CL_RSC6;
    }
    *changed = allowed != a->OffloadAllowed;
    a->OffloadAllowed = allowed;
    return NDIS_STATUS_SUCCESS;
}

/* ---- send path ---- */

static void complete_ipv4_header(UCHAR *ip, ULONG available)
{
    ULONG header;
    uint16_t sum;
    if (available < 20) return;
    header = (ip[0] & 15u) * 4u;
    if (header < 20 || header > available) return;
    ip[10] = 0;
    ip[11] = 0;
    sum = (uint16_t)~chmlink_checksum_add(ip, header, 0);
    ip[10] = (UCHAR)(sum >> 8);
    ip[11] = (UCHAR)sum;
}

static ULONG ip_version(const CL_ADAPTER *a, const UCHAR *packet, ULONG length)
{
    ULONG l3 = a->Mode == CHMLINK_MODE_IP ? 0 : 14;
    if (length <= l3) return 0;
    return packet[l3] >> 4;
}

BOOLEAN ClDescribeSend(CL_ADAPTER *a, PNET_BUFFER_LIST nbl, UCHAR *packet,
    ULONG length, ULONG fine, chmlink_offload *d)
{
    NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO lso;
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum;
    ULONG l3 = a->Mode == CHMLINK_MODE_IP ? 0 : 14;
    ULONG version, offset, header;

    NdisZeroMemory(d, sizeof(*d));
    lso.Value = NET_BUFFER_LIST_INFO(nbl, TcpLargeSendNetBufferListInfo);
    if (lso.Transmit.Type == NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE && lso.LsoV2Transmit.MSS != 0) {
        version = ip_version(a, packet, length);
        if (version == 4) {
            header = length > l3 ? (ULONG)(packet[l3] & 15u) * 4u : 0;
            if (!(fine & CL_LSO4) || header < 20 || length < l3 + header + 20) return FALSE;
            /* The stack leaves the total length at zero for LSO. */
            packet[l3 + 2] = (UCHAR)((length - l3) >> 8);
            packet[l3 + 3] = (UCHAR)(length - l3);
            complete_ipv4_header(packet + l3, length - l3);
        } else if (version == 6) {
            if (!(fine & CL_LSO6) || length < l3 + 40 + 20) return FALSE;
            packet[l3 + 4] = (UCHAR)((length - l3 - 40) >> 8);
            packet[l3 + 5] = (UCHAR)(length - l3 - 40);
        } else return FALSE;
        offset = lso.LsoV2Transmit.TcpHeaderOffset;
        if (offset < l3 || offset + 20 > length) return FALSE;
        header = (ULONG)(packet[offset + 12] >> 4) * 4u;
        if (header < 20 || offset + header >= length) return FALSE;
        d->flags = CHMLINK_OFFLOAD_NEEDS_CSUM;
        d->gso_type = (UCHAR)(version == 4 ? CHMLINK_GSO_TCPV4 : CHMLINK_GSO_TCPV6);
        d->hdr_len = (uint16_t)(offset + header);
        d->gso_size = (uint16_t)lso.LsoV2Transmit.MSS;
        d->csum_start = (uint16_t)offset;
        d->csum_offset = 16;
        return chmlink_offload_valid(a->Mode, packet, length, ClPacketLimit(a), d) ? TRUE : FALSE;
    }

    checksum.Value = NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo);
    version = checksum.Transmit.IsIPv4 ? 4 : checksum.Transmit.IsIPv6 ? 6 : ip_version(a, packet, length);
    if (checksum.Transmit.IsIPv4 && checksum.Transmit.IpHeaderChecksum)
        complete_ipv4_header(packet + l3, length > l3 ? length - l3 : 0);
    if (checksum.Transmit.TcpChecksum || checksum.Transmit.UdpChecksum) {
        ULONG need;
        chmlink_offload pending;
        NdisZeroMemory(&pending, sizeof(pending));
        if (checksum.Transmit.TcpChecksum) {
            offset = checksum.Transmit.TcpHeaderOffset;
            pending.csum_offset = 16;
            need = version == 4 ? CL_TX_TCP4 : version == 6 ? CL_TX_TCP6 : 0;
        } else {
            if (version == 4) {
                if (length <= l3 || (packet[l3] & 15u) < 5) return FALSE;
                offset = l3 + (ULONG)(packet[l3] & 15u) * 4u;
            } else if (version == 6) {
                offset = l3 + 40;
            } else return FALSE;
            pending.csum_offset = 6;
            need = version == 4 ? CL_TX_UDP4 : CL_TX_UDP6;
        }
        if (offset > 0xffffu || offset + pending.csum_offset + 2 > length) return FALSE;
        pending.flags = CHMLINK_OFFLOAD_NEEDS_CSUM;
        pending.csum_start = (uint16_t)offset;
        if (need != 0 && (fine & need)) {
            *d = pending;
        } else {
            /* The session did not get this offload (or is closing it): finish
             * the checksum here rather than hand it an unfinished packet. */
            if (chmlink_offload_complete(a->Mode, packet, length, &pending) != CHMLINK_OK) return FALSE;
        }
    }
    if (d->flags != 0 && !chmlink_offload_valid(a->Mode, packet, length, ClPacketLimit(a), d))
        return FALSE;
    return TRUE;
}

/* ---- receive path ---- */

BOOLEAN ClPrepareInjected(CL_ADAPTER *a, PNET_BUFFER_LIST nbl, const UCHAR *packet,
    ULONG length, const chmlink_offload *d, ULONG fine)
{
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum;
    NDIS_RSC_NBL_INFO rsc;
    ULONG l3 = a->Mode == CHMLINK_MODE_IP ? 0 : 14;
    ULONG version, protocol;
    BOOLEAN valid, needs;

    if (d->flags == 0 && d->gso_type == 0 && d->hdr_len == 0 && d->gso_size == 0 &&
        d->csum_start == 0 && d->csum_offset == 0) return TRUE;
    if (!chmlink_offload_valid(a->Mode, packet, length, ClPacketLimit(a), d)) return FALSE;
    version = length > l3 ? packet[l3] >> 4 : 0;
    if (version == 4) {
        if (length < l3 + 20) return FALSE;
        protocol = packet[l3 + 9];
    } else if (version == 6) {
        if (length < l3 + 40) return FALSE;
        protocol = packet[l3 + 6];
    } else return FALSE;
    valid = (d->flags & CHMLINK_OFFLOAD_DATA_VALID) != 0;
    needs = (d->flags & CHMLINK_OFFLOAD_NEEDS_CSUM) != 0;
    /* An injected packet is either complete and verified, or the session
     * asks the stack to compute the checksum. Nothing else is meaningful. */
    if (needs && !valid) return FALSE;

    NdisZeroMemory(&checksum, sizeof(checksum));
    if (valid) {
        if (version == 4 && (fine & CL_RX_IP4)) checksum.Receive.IpChecksumSucceeded = 1;
        if (protocol == 6) {
            if (fine & (version == 4 ? CL_RX_TCP4 : CL_RX_TCP6)) checksum.Receive.TcpChecksumSucceeded = 1;
        } else if (protocol == 17) {
            if (fine & (version == 4 ? CL_RX_UDP4 : CL_RX_UDP6)) checksum.Receive.UdpChecksumSucceeded = 1;
        }
        if (needs) {
            if (version == 4 && (fine & CL_RX_IP4)) checksum.Receive.IpChecksumValueInvalid = 1;
            /* NDIS has no "value invalid" flag for UDP; UdpChecksumSucceeded alone covers it. */
            if (protocol == 6 && (fine & (version == 4 ? CL_RX_TCP4 : CL_RX_TCP6)))
                checksum.Receive.TcpChecksumValueInvalid = 1;
        }
    }
    if (d->gso_type != CHMLINK_GSO_NONE) {
        ULONG type = d->gso_type & ~(ULONG)CHMLINK_GSO_ECN;
        ULONG segments;
        if (!valid) return FALSE;
        if (type == CHMLINK_GSO_TCPV4) {
            if (!(fine & CL_RSC4) || version != 4) return FALSE;
        } else if (type == CHMLINK_GSO_TCPV6) {
            if (!(fine & CL_RSC6) || version != 6) return FALSE;
        } else return FALSE;
        if (protocol != 6 || d->gso_size == 0 || d->hdr_len >= length) return FALSE;
        segments = (length - d->hdr_len + d->gso_size - 1) / d->gso_size;
        if (segments == 0 || segments > 0xffffu) return FALSE;
        NdisZeroMemory(&rsc, sizeof(rsc));
        rsc.Info.CoalescedSegCount = (USHORT)segments;
        rsc.Info.DupAckCount = 0;
        NET_BUFFER_LIST_INFO(nbl, TcpRecvSegCoalesceInfo) = rsc.Value;
        /* Verified but not recomputed: the value bits are deliberately unset. */
        checksum.Receive.TcpChecksumSucceeded = 1;
        checksum.Receive.TcpChecksumValueInvalid = 1;
        if (version == 4) {
            checksum.Receive.IpChecksumSucceeded = 1;
            checksum.Receive.IpChecksumValueInvalid = 1;
        }
        {
            KIRQL irql;
            KeAcquireSpinLock(&a->Lock, &irql);
            a->RscPackets += segments;
            a->RscOctets += length - d->hdr_len;
            a->RscEvents++;
            KeReleaseSpinLock(&a->Lock, irql);
        }
    }
    NET_BUFFER_LIST_INFO(nbl, TcpIpChecksumNetBufferListInfo) = checksum.Value;
    return TRUE;
}
