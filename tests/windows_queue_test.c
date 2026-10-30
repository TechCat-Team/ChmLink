#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "queue:%d: %s (%lu/%d)\n", \
    __LINE__, #x, GetLastError(), WSAGetLastError()); ExitProcess(1); } } while (0)

static unsigned int consume(chmlink_adapter *a, uint8_t *buffer, unsigned char *seen, int foreign)
{
    uint32_t bytes, count, offset = CHMLINK_BATCH_HEADER_SIZE, i;
    unsigned int found = 0;
    if (!chmlink_receive(a, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
        REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS); return 0;
    }
    REQUIRE(chmlink_batch_validate(buffer, bytes, CHMLINK_MODE_ETHERNET, 1522, &count) == CHMLINK_OK);
    for (i = 0; i < count; ++i) {
        uint32_t length;
        const uint8_t *frame, *payload;
        unsigned int header, sequence;
        memcpy(&length, buffer + offset, 4); offset += 4;
        frame = buffer + offset; offset += (length + 3) & ~3u;
        if (length < 14 + 20 + 8 + 8 || frame[12] != 8 || frame[13] != 0 || frame[23] != 17) continue;
        header = (frame[14] & 15) * 4;
        if (header < 20 || length < 14 + header + 16) continue;
        payload = frame + 14 + header + 8;
        if (memcmp(payload, "CLQT", 4)) continue;
        REQUIRE(!foreign);
        sequence = (unsigned int)payload[4] | (unsigned int)payload[5] << 8;
        if (seen && sequence) {
            REQUIRE(sequence <= 1000 && !seen[sequence]); seen[sequence] = 1; ++found;
        }
    }
    return found;
}

void lifecycle_queue_check(uint32_t first, uint32_t second)
{
    WSADATA winsock;
    SOCKET socket_handle;
    struct sockaddr_in source = {0}, destination = {0};
    MIB_UNICASTIPADDRESS_ROW address;
    chmlink_adapter *a = chmlink_open(first), *b = chmlink_open(second);
    chmlink_adapter_info before, full, after;
    uint8_t payload[8] = {'C','L','Q','T',0,0,0,0};
    uint8_t *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    unsigned char seen[1001] = {0};
    unsigned int i, total = 0;
    DWORD enabled = 1, interface_index = htonl(first), timeout = 1000;
    ULONGLONG deadline;
    REQUIRE(a && b && buffer);
    REQUIRE(WSAStartup(MAKEWORD(2,2), &winsock) == 0);
    InitializeUnicastIpAddressEntry(&address); address.InterfaceIndex = first;
    address.Address.Ipv4.sin_family = AF_INET;
    REQUIRE(InetPtonA(AF_INET, "169.254.250.1", &address.Address.Ipv4.sin_addr) == 1);
    REQUIRE(GetUnicastIpAddressEntry(&address) == NO_ERROR);
    deadline = GetTickCount64() + 5000;
    while (address.DadState != IpDadStatePreferred && GetTickCount64() < deadline) {
        Sleep(10); REQUIRE(GetUnicastIpAddressEntry(&address) == NO_ERROR);
    }
    REQUIRE(address.DadState == IpDadStatePreferred);
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); REQUIRE(socket_handle != INVALID_SOCKET);
    REQUIRE(setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST, (const char *)&enabled, sizeof(enabled)) == 0);
    REQUIRE(setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    REQUIRE(setsockopt(socket_handle, IPPROTO_IP, IP_UNICAST_IF, (const char *)&interface_index, sizeof(interface_index)) == 0);
    source.sin_family = AF_INET; source.sin_addr = address.Address.Ipv4.sin_addr;
    REQUIRE(bind(socket_handle, (const struct sockaddr *)&source, sizeof(source)) == 0);
    destination.sin_family = AF_INET; destination.sin_port = htons(48888);
    REQUIRE(InetPtonA(AF_INET, "169.254.250.255", &destination.sin_addr) == 1);
    REQUIRE(chmlink_query(a, &before));
    /* Small frames are bounded by the packet count, not the ring's bytes. */
    for (i = 0; i < before.queue_capacity + 1024; ++i) {
        int result = sendto(socket_handle, (const char *)payload, sizeof(payload), 0,
                            (const struct sockaddr *)&destination, sizeof(destination));
        REQUIRE(result == sizeof(payload) || (result == SOCKET_ERROR && WSAGetLastError() == WSAENOBUFS));
    }
    REQUIRE(chmlink_query(a, &full));
    REQUIRE(full.queued_packets == full.queue_capacity && full.tx_queue_drops > before.tx_queue_drops);
    printf("Queue full: %lu/%lu, drops increased by %llu\n", (unsigned long)full.queued_packets,
        (unsigned long)full.queue_capacity, (unsigned long long)(full.tx_queue_drops - before.tx_queue_drops));
    /* One receive returns at most one batch; drain the whole queue. */
    for (i = 0; i < before.queue_capacity / 64 + 64; ++i) {
        REQUIRE(chmlink_query(a, &after));
        if (after.queued_packets == 0) break;
        consume(a, buffer, NULL, 0); consume(b, buffer, NULL, 1);
    }
    for (i = 1; i <= 1000; ++i) {
        payload[4] = (uint8_t)i; payload[5] = (uint8_t)(i >> 8);
        REQUIRE(sendto(socket_handle, (const char *)payload, sizeof(payload), 0,
            (const struct sockaddr *)&destination, sizeof(destination)) == sizeof(payload));
        total += consume(a, buffer, seen, 0); consume(b, buffer, NULL, 1);
    }
    deadline = GetTickCount64() + 5000;
    while (total < 1000 && GetTickCount64() < deadline) {
        total += consume(a, buffer, seen, 0); consume(b, buffer, NULL, 1); Sleep(1);
    }
    REQUIRE(total == 1000 && chmlink_query(a, &after));
    REQUIRE(after.queued_packets <= after.queue_capacity);
    closesocket(socket_handle); WSACleanup(); free(buffer); chmlink_close(a); chmlink_close(b);
    puts("PASS bounded queue saturation, recovery, 1000 unique marker packets and adapter isolation");
}
