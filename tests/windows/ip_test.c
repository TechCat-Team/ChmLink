#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"
#include "chmlink/device.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (win=%lu wsa=%d)\n", \
    __FILE__, __LINE__, #x, GetLastError(), WSAGetLastError()); ExitProcess(1); } } while (0)

static const uint8_t marker[] = "ChmLink-IP-test";

static uint32_t sum_words(uint32_t sum, const uint8_t *p, size_t n)
{
    while (n >= 2) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t finish_sum(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

/* Echo only the test UDP datagram. The output is a complete raw-IP packet,
 * with endpoints reversed and IPv6's mandatory UDP checksum calculated. */
static size_t make_reply(const uint8_t *packet, size_t length, uint8_t *out, uint32_t limit,
                         const uint8_t *local4, const uint8_t *peer4,
                         const uint8_t *local6, const uint8_t *peer6)
{
    size_t header, udp_length;
    uint8_t version = packet[0] >> 4;
    uint16_t checksum;
    uint32_t sum;
    uint8_t saved[16];
    /* Fragments are not echoed: a datagram up to the MTU must arrive whole. */
    if (!chmlink_ip_packet_valid(packet, length, limit)) return 0;
    if (version == 4) {
        header = (packet[0] & 15u) * 4u;
        if (packet[9] != IPPROTO_UDP || (packet[6] & 0x3f) || packet[7] ||
            memcmp(packet + 12, local4, 4) || memcmp(packet + 16, peer4, 4)) return 0;
    } else {
        header = 40;
        if (packet[6] != IPPROTO_UDP || memcmp(packet + 8, local6, 16) ||
            memcmp(packet + 24, peer6, 16)) return 0;
    }
    if (length < header + 8 + sizeof(marker)) return 0;
    udp_length = ((size_t)packet[header + 4] << 8) | packet[header + 5];
    if (udp_length != length - header || memcmp(packet + header + 8, marker, sizeof(marker))) return 0;
    memcpy(out, packet, length);
    if (version == 4) {
        memcpy(saved, out + 12, 4); memcpy(out + 12, out + 16, 4); memcpy(out + 16, saved, 4);
        out[8] = 64; out[10] = out[11] = 0;
        put16(out + 10, finish_sum(sum_words(0, out, header)));
    } else {
        memcpy(saved, out + 8, 16); memcpy(out + 8, out + 24, 16); memcpy(out + 24, saved, 16);
        out[7] = 64;
    }
    saved[0] = out[header]; saved[1] = out[header + 1];
    out[header] = out[header + 2]; out[header + 1] = out[header + 3];
    out[header + 2] = saved[0]; out[header + 3] = saved[1];
    out[header + 6] = out[header + 7] = 0;
    if (version == 6) {
        sum = sum_words(0, out + 8, 32);
        sum += (uint32_t)udp_length + IPPROTO_UDP;
        checksum = finish_sum(sum_words(sum, out + header, udp_length));
        put16(out + header + 6, checksum ? checksum : 0xffffu);
    }
    return length;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wait_for_address(ULONG index, int family, const void *address)
{
    MIB_UNICASTIPADDRESS_ROW row;
    ULONGLONG deadline = GetTickCount64() + 10000;
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceIndex = index;
    if (family == AF_INET) {
        row.Address.Ipv4.sin_family = AF_INET;
        memcpy(&row.Address.Ipv4.sin_addr, address, 4);
    } else {
        row.Address.Ipv6.sin6_family = AF_INET6;
        memcpy(&row.Address.Ipv6.sin6_addr, address, 16);
        if (IN6_IS_ADDR_LINKLOCAL(&row.Address.Ipv6.sin6_addr)) row.Address.Ipv6.sin6_scope_id = index;
    }
    while (GetTickCount64() < deadline) {
        if (GetUnicastIpAddressEntry(&row) == NO_ERROR && row.DadState == IpDadStatePreferred) return;
        Sleep(50);
    }
    fprintf(stderr, "Test address is not preferred on the selected adapter.\n"); ExitProcess(1);
}

static uint32_t ethernet_limit = CHMLINK_ETHERNET_LIMIT;

/* The Ethernet adapter's MTU (tools/test-ip.ps1 passes the one it configured)
 * sets its frame limit: a frame of exactly packet_limit bytes is accepted by
 * the SDK and the driver, one byte more is refused before it reaches the ring. */
static void check_ethernet_mtu(chmlink_adapter *ethernet, const chmlink_adapter_info *info)
{
    char expected[16];
    chmlink_adapter_info after;
    uint8_t *frame;
    ULONGLONG deadline;
    REQUIRE(info->packet_limit == info->mtu + CHMLINK_ETHERNET_OVERHEAD);
    if (GetEnvironmentVariableA("CHMLINK_TEST_ETHERNET_MTU", expected, sizeof(expected)) != 0)
        REQUIRE(info->mtu == (uint32_t)strtoul(expected, NULL, 10));
    ethernet_limit = info->packet_limit;
    REQUIRE(chmlink_allocate_packet(ethernet, info->packet_limit + 1, NULL) == NULL &&
            GetLastError() == ERROR_INVALID_PARAMETER);
    frame = chmlink_allocate_packet(ethernet, info->packet_limit, NULL); REQUIRE(frame != NULL);
    memset(frame, 0xff, 6);                       /* broadcast, so the receive filter passes it */
    memcpy(frame + 6, info->mac, 6);
    frame[12] = 0x88; frame[13] = 0xb5;           /* IEEE local experimental EtherType */
    memset(frame + 14, 0x5a, info->packet_limit - 14);
    chmlink_send_packet(ethernet, frame);
    deadline = GetTickCount64() + 5000;
    do {
        Sleep(10);
        REQUIRE(chmlink_query(ethernet, &after));
    } while (after.rx_packets == info->rx_packets && GetTickCount64() < deadline);
    REQUIRE(after.rx_packets == info->rx_packets + 1 && after.rx_discards == info->rx_discards);
    printf("ethernet mtu %u: a %u-byte frame was accepted, one byte more refused\n", info->mtu, info->packet_limit);
}

static void check_ethernet_isolation(chmlink_adapter *ethernet, uint8_t *buffer)
{
    uint32_t bytes, count, i, offset = CHMLINK_BATCH_HEADER_SIZE;
    if (!chmlink_receive(ethernet, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
        REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS); return;
    }
    REQUIRE(chmlink_batch_validate(buffer, bytes, CHMLINK_MODE_ETHERNET, ethernet_limit, &count) == CHMLINK_OK);
    for (i = 0; i < count; ++i) {
        size_t j, length = get32(buffer + offset);
        offset += 4;
        for (j = 0; j + sizeof(marker) <= length; ++j)
            REQUIRE(memcmp(buffer + offset + j, marker, sizeof(marker)) != 0);
        offset += (uint32_t)((length + 3u) & ~3u);
    }
}

typedef struct exchange_args {
    chmlink_adapter *adapter, *ethernet;
    uint8_t *batch, *ethernet_batch;
    SOCKET s4, s6;
    struct sockaddr_in *local4, *peer4;
    struct sockaddr_in6 *local6, *peer6;
    uint32_t limit;
} exchange_args;

/* Send a UDP datagram of `size4` bytes over IPv4 and `size6` over IPv6
 * (marker first), echo them back through the adapter, and require both to
 * come back whole. */
static void exchange(const exchange_args *x, size_t size4, size_t size6)
{
    static uint8_t reply[CHMLINK_PACKET_MAX_SIZE], encoded[CHMLINK_PACKET_MAX_SIZE + 64];
    static uint8_t data[65536], incoming[65536];
    chmlink_batch_writer writer;
    ULONGLONG deadline;
    int got4 = 0, got6 = 0;
    size_t i;
    for (i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 13 + 5);
    memcpy(data, marker, sizeof(marker));
    REQUIRE(sendto(x->s4, (const char *)data, (int)size4, 0, (struct sockaddr *)x->peer4, sizeof(*x->peer4)) == (int)size4);
    REQUIRE(sendto(x->s6, (const char *)data, (int)size6, 0, (struct sockaddr *)x->peer6, sizeof(*x->peer6)) == (int)size6);
    deadline = GetTickCount64() + 10000;
    while ((!got4 || !got6) && GetTickCount64() < deadline) {
        uint32_t bytes, count, k, offset = CHMLINK_BATCH_HEADER_SIZE;
        int n;
        if (chmlink_receive(x->adapter, x->batch, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
            REQUIRE(chmlink_batch_validate(x->batch, bytes, CHMLINK_MODE_IP, x->limit, &count) == CHMLINK_OK);
            for (k = 0; k < count; ++k) {
                uint32_t length = get32(x->batch + offset); size_t reply_length;
                offset += 4;
                reply_length = make_reply(x->batch + offset, length, reply, x->limit,
                    (const uint8_t *)&x->local4->sin_addr, (const uint8_t *)&x->peer4->sin_addr,
                    (const uint8_t *)&x->local6->sin6_addr, (const uint8_t *)&x->peer6->sin6_addr);
                offset += (length + 3u) & ~3u;
                if (!reply_length) continue;
                REQUIRE(chmlink_batch_begin(&writer, encoded, sizeof(encoded), CHMLINK_MODE_IP, x->limit) == CHMLINK_OK);
                REQUIRE(chmlink_batch_append(&writer, reply, reply_length) == CHMLINK_OK);
                REQUIRE(chmlink_inject(x->adapter, encoded, (uint32_t)writer.length));
            }
        } else REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS);
        if (x->ethernet) check_ethernet_isolation(x->ethernet, x->ethernet_batch);
        n = recvfrom(x->s4, (char *)incoming, sizeof(incoming), 0, NULL, NULL);
        if (n >= 0) { REQUIRE((size_t)n == size4 && !memcmp(incoming, data, size4)); got4 = 1; }
        else REQUIRE(WSAGetLastError() == WSAEWOULDBLOCK);
        n = recvfrom(x->s6, (char *)incoming, sizeof(incoming), 0, NULL, NULL);
        if (n >= 0) { REQUIRE((size_t)n == size6 && !memcmp(incoming, data, size6)); got6 = 1; }
        else REQUIRE(WSAGetLastError() == WSAEWOULDBLOCK);
        if (!got4 || !got6) Sleep(10);
    }
    REQUIRE(got4 && got6);
}

typedef struct find_device_args {
    uint32_t index;
    unsigned int matches;
    chmlink_device device;
} find_device_args;

static int find_device(const chmlink_device *device, void *context)
{
    find_device_args *args = context;
    if (device->interface_index == args->index) { args->device = *device; args->matches++; }
    return 1;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    SOCKET s4, s6;
    struct sockaddr_in local4 = {0}, peer4 = {0};
    struct sockaddr_in6 local6 = {0}, peer6 = {0};
    chmlink_adapter *adapter, *ethernet = NULL;
    chmlink_adapter_info info;
    uint8_t *batch, *ethernetBatch = NULL;
    exchange_args x;
    unsigned long index, nonblocking = 1;
    char *end;
    DWORD if4, if6;
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "Usage: chmlink-ip-test <ifIndex> <local4> <peer4> <local6> <peer6> [ethernet-ifIndex]\n"); return 2;
    }
    errno = 0; index = strtoul(argv[1], &end, 10);
    REQUIRE(!errno && *argv[1] != '-' && !*end && index);
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    local4.sin_family = peer4.sin_family = AF_INET;
    local6.sin6_family = peer6.sin6_family = AF_INET6;
    REQUIRE(InetPtonA(AF_INET, argv[2], &local4.sin_addr) == 1 && InetPtonA(AF_INET, argv[3], &peer4.sin_addr) == 1);
    REQUIRE(InetPtonA(AF_INET6, argv[4], &local6.sin6_addr) == 1 && InetPtonA(AF_INET6, argv[5], &peer6.sin6_addr) == 1);
    REQUIRE(memcmp(&local4.sin_addr, &peer4.sin_addr, 4) && memcmp(&local6.sin6_addr, &peer6.sin6_addr, 16));
    if (IN6_IS_ADDR_LINKLOCAL(&local6.sin6_addr)) local6.sin6_scope_id = (ULONG)index;
    if (IN6_IS_ADDR_LINKLOCAL(&peer6.sin6_addr)) peer6.sin6_scope_id = (ULONG)index;
    peer4.sin_port = peer6.sin6_port = htons(49381);
    adapter = chmlink_open((uint32_t)index); REQUIRE(adapter != NULL);
    REQUIRE(chmlink_query(adapter, &info) && info.mode == CHMLINK_MODE_IP && info.packet_limit == info.mtu);
    {
        /* tools/test-ip.ps1 passes the MTU it configured. */
        char expected[16];
        if (GetEnvironmentVariableA("CHMLINK_TEST_MTU", expected, sizeof(expected)) != 0)
            REQUIRE(info.mtu == (uint32_t)strtoul(expected, NULL, 10));
    }
    {
        /* The management API finds the same adapter with its configuration. */
        find_device_args found = {0};
        found.index = (uint32_t)index;
        REQUIRE(chmlink_enumerate_devices(NULL, find_device, &found) && found.matches == 1);
        REQUIRE(found.device.mode == CHMLINK_MODE_IP && found.device.mtu == info.mtu && found.device.name[0]);
        REQUIRE(chmlink_get_device(found.device.instance_id, &found.device) &&
                found.device.interface_index == (uint32_t)index);
    }
    if (argc == 7) {
        unsigned long ethernetIndex;
        errno = 0; ethernetIndex = strtoul(argv[6], &end, 10);
        REQUIRE(!errno && *argv[6] != '-' && !*end && ethernetIndex && ethernetIndex != index);
        ethernet = chmlink_open((uint32_t)ethernetIndex); REQUIRE(ethernet != NULL);
        REQUIRE(chmlink_query(ethernet, &info) && info.mode == CHMLINK_MODE_ETHERNET);
        check_ethernet_mtu(ethernet, &info);
        ethernetBatch = malloc(CHMLINK_BATCH_MAX_SIZE); REQUIRE(ethernetBatch != NULL);
        REQUIRE(chmlink_query(adapter, &info) && info.mode == CHMLINK_MODE_IP);
    }
    wait_for_address((ULONG)index, AF_INET, &local4.sin_addr);
    wait_for_address((ULONG)index, AF_INET6, &local6.sin6_addr);
    batch = malloc(CHMLINK_BATCH_MAX_SIZE); REQUIRE(batch != NULL);
    s4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); s6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(s4 != INVALID_SOCKET && s6 != INVALID_SOCKET);
    if4 = htonl((ULONG)index); if6 = (DWORD)index;
    REQUIRE(setsockopt(s4, IPPROTO_IP, IP_UNICAST_IF, (const char *)&if4, sizeof(if4)) == 0);
    REQUIRE(setsockopt(s6, IPPROTO_IPV6, IPV6_UNICAST_IF, (const char *)&if6, sizeof(if6)) == 0);
    REQUIRE(bind(s4, (struct sockaddr *)&local4, sizeof(local4)) == 0 && bind(s6, (struct sockaddr *)&local6, sizeof(local6)) == 0);
    REQUIRE(ioctlsocket(s4, FIONBIO, &nonblocking) == 0 && ioctlsocket(s6, FIONBIO, &nonblocking) == 0);
    {
        int big = 0;
        x.adapter = adapter; x.ethernet = ethernet; x.ethernet_batch = ethernetBatch; x.batch = batch;
        x.s4 = s4; x.s6 = s6; x.peer4 = &peer4; x.peer6 = &peer6; x.local4 = &local4; x.local6 = &local6;
        x.limit = info.packet_limit;
        exchange(&x, sizeof(marker), sizeof(marker));
        /* A datagram that fills the MTU exactly crosses the adapter as one packet. */
        if (info.mtu > 1500) { exchange(&x, info.mtu - 28, info.mtu - 48); big = 1; }
        printf("mtu %u%s\n", info.mtu, big ? ": full-size IPv4 and IPv6 datagrams passed unfragmented" : "");
    }
    if (ethernet) {
        ULONGLONG endIsolation = GetTickCount64() + 250;
        while (GetTickCount64() < endIsolation) { check_ethernet_isolation(ethernet, ethernetBatch); Sleep(10); }
        chmlink_close(ethernet); free(ethernetBatch);
    }
    closesocket(s4); closesocket(s6); WSACleanup(); free(batch); chmlink_close(adapter);
    puts("PASS IPv4 and IPv6 UDP roundtrips through raw-IP adapter"); return 0;
}
