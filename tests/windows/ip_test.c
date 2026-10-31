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
static size_t make_reply(const uint8_t *packet, size_t length, uint8_t *out,
                         const uint8_t *local4, const uint8_t *peer4,
                         const uint8_t *local6, const uint8_t *peer6)
{
    size_t header, udp_length;
    uint8_t version = packet[0] >> 4;
    uint16_t checksum;
    uint32_t sum;
    uint8_t saved[16];
    if (!chmlink_ip_packet_valid(packet, length, 1500)) return 0;
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

static void check_ethernet_isolation(chmlink_adapter *ethernet, uint8_t *buffer)
{
    uint32_t bytes, count, i, offset = CHMLINK_BATCH_HEADER_SIZE;
    if (!chmlink_receive(ethernet, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
        REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS); return;
    }
    REQUIRE(chmlink_batch_validate(buffer, bytes, CHMLINK_MODE_ETHERNET, 1522, &count) == CHMLINK_OK);
    for (i = 0; i < count; ++i) {
        size_t j, length = get32(buffer + offset);
        offset += 4;
        for (j = 0; j + sizeof(marker) <= length; ++j)
            REQUIRE(memcmp(buffer + offset + j, marker, sizeof(marker)) != 0);
        offset += (uint32_t)((length + 3u) & ~3u);
    }
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    SOCKET s4, s6;
    struct sockaddr_in local4 = {0}, peer4 = {0};
    struct sockaddr_in6 local6 = {0}, peer6 = {0};
    chmlink_adapter *adapter, *ethernet = NULL;
    chmlink_adapter_info info;
    uint8_t *batch, *ethernetBatch = NULL, reply[1500], encoded[1532], incoming[64];
    chmlink_batch_writer writer;
    unsigned long index, nonblocking = 1;
    char *end;
    DWORD if4, if6;
    ULONGLONG deadline;
    int got4 = 0, got6 = 0;
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
    REQUIRE(chmlink_query(adapter, &info) && info.mode == CHMLINK_MODE_IP && info.packet_limit <= sizeof(reply));
    if (argc == 7) {
        unsigned long ethernetIndex;
        errno = 0; ethernetIndex = strtoul(argv[6], &end, 10);
        REQUIRE(!errno && *argv[6] != '-' && !*end && ethernetIndex && ethernetIndex != index);
        ethernet = chmlink_open((uint32_t)ethernetIndex); REQUIRE(ethernet != NULL);
        REQUIRE(chmlink_query(ethernet, &info) && info.mode == CHMLINK_MODE_ETHERNET);
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
    REQUIRE(sendto(s4, (const char *)marker, sizeof(marker), 0, (struct sockaddr *)&peer4, sizeof(peer4)) == sizeof(marker));
    REQUIRE(sendto(s6, (const char *)marker, sizeof(marker), 0, (struct sockaddr *)&peer6, sizeof(peer6)) == sizeof(marker));
    deadline = GetTickCount64() + 10000;
    while ((!got4 || !got6) && GetTickCount64() < deadline) {
        uint32_t bytes, count, i, offset = CHMLINK_BATCH_HEADER_SIZE;
        int n;
        if (chmlink_receive(adapter, batch, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
            REQUIRE(chmlink_batch_validate(batch, bytes, CHMLINK_MODE_IP, info.packet_limit, &count) == CHMLINK_OK);
            for (i = 0; i < count; ++i) {
                uint32_t length = get32(batch + offset); size_t reply_length;
                offset += 4;
                reply_length = make_reply(batch + offset, length, reply,
                    (const uint8_t *)&local4.sin_addr, (const uint8_t *)&peer4.sin_addr,
                    (const uint8_t *)&local6.sin6_addr, (const uint8_t *)&peer6.sin6_addr);
                offset += (length + 3u) & ~3u;
                if (!reply_length) continue;
                REQUIRE(chmlink_batch_begin(&writer, encoded, sizeof(encoded), CHMLINK_MODE_IP, info.packet_limit) == CHMLINK_OK);
                REQUIRE(chmlink_batch_append(&writer, reply, reply_length) == CHMLINK_OK);
                REQUIRE(chmlink_inject(adapter, encoded, (uint32_t)writer.length));
            }
        } else REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS);
        if (ethernet) check_ethernet_isolation(ethernet, ethernetBatch);
        n = recvfrom(s4, (char *)incoming, sizeof(incoming), 0, NULL, NULL);
        if (n >= 0) { REQUIRE(n == sizeof(marker) && !memcmp(incoming, marker, sizeof(marker))); got4 = 1; }
        else REQUIRE(WSAGetLastError() == WSAEWOULDBLOCK);
        n = recvfrom(s6, (char *)incoming, sizeof(incoming), 0, NULL, NULL);
        if (n >= 0) { REQUIRE(n == sizeof(marker) && !memcmp(incoming, marker, sizeof(marker))); got6 = 1; }
        else REQUIRE(WSAGetLastError() == WSAEWOULDBLOCK);
        if (!got4 || !got6) Sleep(10);
    }
    REQUIRE(got4 && got6);
    if (ethernet) {
        ULONGLONG endIsolation = GetTickCount64() + 250;
        while (GetTickCount64() < endIsolation) { check_ethernet_isolation(ethernet, ethernetBatch); Sleep(10); }
        chmlink_close(ethernet); free(ethernetBatch);
    }
    closesocket(s4); closesocket(s6); WSACleanup(); free(batch); chmlink_close(adapter);
    puts("PASS IPv4 and IPv6 UDP roundtrips through raw-IP adapter"); return 0;
}
