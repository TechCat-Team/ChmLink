#include "chmlink/query.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

void chmlink_query_encode(uint8_t *out, const chmlink_query_info *info)
{
    memset(out, 0, CHMLINK_INFO_SIZE);
    put32(out + CHMLINK_INFO_VERSION, CHMLINK_DEVICE_VERSION);
    put32(out + CHMLINK_INFO_LENGTH, CHMLINK_INFO_SIZE);
    put32(out + CHMLINK_INFO_CAPABILITIES, info->capabilities);
    put32(out + CHMLINK_INFO_STATE, info->state);
    put32(out + CHMLINK_INFO_MODE, info->mode);
    put32(out + CHMLINK_INFO_INTERFACE_INDEX, info->interface_index);
    put32(out + CHMLINK_INFO_MTU, info->mtu);
    put32(out + CHMLINK_INFO_PACKET_LIMIT, info->packet_limit);
    put32(out + CHMLINK_INFO_QUEUE_CAPACITY, info->queue_capacity);
    put32(out + CHMLINK_INFO_QUEUED_PACKETS, info->queued_packets);
    put32(out + CHMLINK_INFO_PACKET_FILTER, info->packet_filter);
    memcpy(out + CHMLINK_INFO_MAC, info->mac, CHMLINK_INFO_MAC_SIZE);
    put64(out + CHMLINK_INFO_LUID, info->luid);
    put64(out + CHMLINK_INFO_TX_PACKETS, info->tx_packets);
    put64(out + CHMLINK_INFO_TX_BYTES, info->tx_bytes);
    put64(out + CHMLINK_INFO_TX_ERRORS, info->tx_errors);
    put64(out + CHMLINK_INFO_RX_PACKETS, info->rx_packets);
    put64(out + CHMLINK_INFO_RX_BYTES, info->rx_bytes);
    put64(out + CHMLINK_INFO_RX_DISCARDS, info->rx_discards);
    put64(out + CHMLINK_INFO_TX_QUEUE_DROPS, info->tx_queue_drops);
}

static int mac_is_zero(const uint8_t *mac)
{
    unsigned int i;
    for (i = 0; i < CHMLINK_INFO_MAC_SIZE; ++i)
        if (mac[i] != 0) return 0;
    return 1;
}

/* Ethernet: exclusive Ethernet capability; the limit holds an MTU-sized
 * payload plus the 14-byte Ethernet header. */
static int ethernet_valid(const chmlink_query_info *info)
{
    return (info->capabilities & CHMLINK_CAP_ETHERNET) &&
           !(info->capabilities & CHMLINK_CAP_IP) &&
           info->packet_limit >= 14 &&
           info->mtu <= info->packet_limit - 14;
}

/* IP: exclusive IP capability, no MAC, IPv6 minimum MTU, and the packet
 * limit equals the MTU because there is no link-layer header. */
static int ip_valid(const chmlink_query_info *info)
{
    return (info->capabilities & CHMLINK_CAP_IP) &&
           !(info->capabilities & CHMLINK_CAP_ETHERNET) &&
           info->packet_limit >= 1280 && info->mtu >= 1280 &&
           info->mtu == info->packet_limit &&
           mac_is_zero(info->mac);
}

static int common_valid(const chmlink_query_info *info)
{
    return (info->capabilities & CHMLINK_CAP_BATCH) &&
           info->packet_limit <= CHMLINK_PACKET_MAX_SIZE &&
           info->mtu != 0 && info->queue_capacity != 0 &&
           info->queued_packets <= info->queue_capacity;
}

chmlink_result chmlink_query_decode(chmlink_query_info *out,
                                    const void *bytes, size_t length)
{
    const uint8_t *p = (const uint8_t *)bytes;
    chmlink_query_info next;
    int mode_ok;

    if (out == NULL || p == NULL) return CHMLINK_INVALID_ARGUMENT;
    if (length != CHMLINK_INFO_SIZE ||
        get32(p + CHMLINK_INFO_VERSION) != CHMLINK_DEVICE_VERSION ||
        get32(p + CHMLINK_INFO_LENGTH) != CHMLINK_INFO_SIZE) {
        return CHMLINK_UNSUPPORTED_VERSION;
    }
    memset(&next, 0, sizeof(next));
    next.version = get32(p + CHMLINK_INFO_VERSION);
    next.capabilities = get32(p + CHMLINK_INFO_CAPABILITIES);
    next.state = get32(p + CHMLINK_INFO_STATE);
    next.mode = get32(p + CHMLINK_INFO_MODE);
    next.interface_index = get32(p + CHMLINK_INFO_INTERFACE_INDEX);
    next.mtu = get32(p + CHMLINK_INFO_MTU);
    next.packet_limit = get32(p + CHMLINK_INFO_PACKET_LIMIT);
    next.queue_capacity = get32(p + CHMLINK_INFO_QUEUE_CAPACITY);
    next.queued_packets = get32(p + CHMLINK_INFO_QUEUED_PACKETS);
    next.packet_filter = get32(p + CHMLINK_INFO_PACKET_FILTER);
    memcpy(next.mac, p + CHMLINK_INFO_MAC, CHMLINK_INFO_MAC_SIZE);
    next.luid = get64(p + CHMLINK_INFO_LUID);
    next.tx_packets = get64(p + CHMLINK_INFO_TX_PACKETS);
    next.tx_bytes = get64(p + CHMLINK_INFO_TX_BYTES);
    next.tx_errors = get64(p + CHMLINK_INFO_TX_ERRORS);
    next.rx_packets = get64(p + CHMLINK_INFO_RX_PACKETS);
    next.rx_bytes = get64(p + CHMLINK_INFO_RX_BYTES);
    next.rx_discards = get64(p + CHMLINK_INFO_RX_DISCARDS);
    next.tx_queue_drops = get64(p + CHMLINK_INFO_TX_QUEUE_DROPS);

    if (next.mode == CHMLINK_MODE_ETHERNET) mode_ok = ethernet_valid(&next);
    else if (next.mode == CHMLINK_MODE_IP) mode_ok = ip_valid(&next);
    else mode_ok = 0;
    if (!mode_ok || !common_valid(&next)) return CHMLINK_INVALID_QUERY;
    *out = next;
    return CHMLINK_OK;
}
