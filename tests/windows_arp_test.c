#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "chmlink/api.h"
#include "chmlink/device.h"
#include "chmlink/protocol.h"
#include "arp_peer.h"

typedef struct peer_context {
    chmlink_adapter *adapter;
    uint8_t local[4], peer[4];
    LONG stop, replied, error;
} peer_context;

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static DWORD WINAPI peer_worker(void *arg)
{
    peer_context *ctx = arg;
    const uint8_t mac[6] = {0x02, 0x43, 0x4c, 0x00, 0x00, 0x02};
    uint8_t *batch = malloc(CHMLINK_BATCH_MAX_SIZE);
    if (!batch) { InterlockedExchange(&ctx->error, ERROR_NOT_ENOUGH_MEMORY); return 1; }
    while (!InterlockedCompareExchange(&ctx->stop, 0, 0)) {
        uint32_t size, count, i, offset = CHMLINK_BATCH_HEADER_SIZE;
        if (!chmlink_receive(ctx->adapter, batch, CHMLINK_BATCH_MAX_SIZE, &size)) {
            DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS || error == ERROR_NOT_READY) { Sleep(10); continue; }
            InterlockedExchange(&ctx->error, (LONG)error); break;
        }
        if (chmlink_batch_validate(batch, size, CHMLINK_MODE_ETHERNET,
                                   CHMLINK_ETHERNET_LIMIT, &count) != CHMLINK_OK) {
            InterlockedExchange(&ctx->error, ERROR_INVALID_DATA); break;
        }
        for (i = 0; i < count; ++i) {
            uint32_t len = read_u32(batch + offset);
            uint8_t frame[42], out[72];
            chmlink_batch_writer writer;
            offset += 4;
            if (arp_make_reply(batch + offset, len, ctx->local, ctx->peer, mac, frame)) {
                if (chmlink_batch_begin(&writer, out, sizeof(out), CHMLINK_MODE_ETHERNET,
                                        CHMLINK_ETHERNET_LIMIT) != CHMLINK_OK ||
                    chmlink_batch_append(&writer, frame, sizeof(frame)) != CHMLINK_OK ||
                    !chmlink_inject(ctx->adapter, out, (uint32_t)writer.length)) {
                    InterlockedExchange(&ctx->error, (LONG)GetLastError());
                    InterlockedExchange(&ctx->stop, 1); break;
                }
                InterlockedIncrement(&ctx->replied);
            }
            offset += (len + 3) & ~3u;
        }
    }
    free(batch);
    return 0;
}

int main(int argc, char **argv)
{
    peer_context ctx = {0};
    chmlink_adapter_info info;
    struct in_addr local, peer;
    MIB_UNICASTIPADDRESS_ROW address;
    MIB_IPNET_ROW2 neighbor;
    unsigned long index, received_mac[2] = {0};
    ULONG mac_length = 6, result, attempt;
    char *end;
    HANDLE thread;
    int ok;
    const uint8_t expected_mac[6] = {0x02, 0x43, 0x4c, 0, 0, 2};
    if (argc != 4) {
        fprintf(stderr, "Usage: chmlink-arp-test <interface-index> <local-ipv4> <unused-peer-ipv4>\n");
        return 2;
    }
    errno = 0; index = strtoul(argv[1], &end, 10);
    if (errno || *end || !index || argv[1][0] == '-' ||
        InetPtonA(AF_INET, argv[2], &local) != 1 ||
        InetPtonA(AF_INET, argv[3], &peer) != 1 || local.s_addr == peer.s_addr) return 2;
    if (((const uint8_t *)&local.s_addr)[0] != 169 || ((const uint8_t *)&local.s_addr)[1] != 254 ||
        ((const uint8_t *)&peer.s_addr)[0] != 169 || ((const uint8_t *)&peer.s_addr)[1] != 254) {
        fprintf(stderr, "Use distinct link-local IPv4 addresses for this test.\n"); return 2;
    }
    InitializeUnicastIpAddressEntry(&address);
    address.InterfaceIndex = (NET_IFINDEX)index;
    address.Address.Ipv4.sin_family = AF_INET;
    address.Address.Ipv4.sin_addr = local;
    if (GetUnicastIpAddressEntry(&address) != NO_ERROR) {
        fprintf(stderr, "Local IPv4 address is not assigned to this interface.\n"); return 1;
    }
    memset(&neighbor, 0, sizeof(neighbor));
    neighbor.InterfaceIndex = (NET_IFINDEX)index;
    neighbor.Address.Ipv4.sin_family = AF_INET;
    neighbor.Address.Ipv4.sin_addr = peer;
    result = GetIpNetEntry2(&neighbor);
    if (result != ERROR_NOT_FOUND) {
        fprintf(stderr, "Peer already has a neighbor entry or lookup failed (%lu); choose an unused peer.\n", result);
        return 1;
    }
    memcpy(ctx.local, &local.s_addr, 4); memcpy(ctx.peer, &peer.s_addr, 4);
    ctx.adapter = chmlink_open((uint32_t)index);
    if (!ctx.adapter) { fprintf(stderr, "Open failed: %lu\n", GetLastError()); return 1; }
    if (!chmlink_query(ctx.adapter, &info) || info.mode != CHMLINK_MODE_ETHERNET) {
        fprintf(stderr, "ARP verification requires an Ethernet adapter.\n"); chmlink_close(ctx.adapter); return 1;
    }
    thread = CreateThread(NULL, 0, peer_worker, &ctx, 0, NULL);
    if (!thread) { fprintf(stderr, "Thread creation failed: %lu\n", GetLastError()); chmlink_close(ctx.adapter); return 1; }
    /* SendARP uses the chosen source address; the worker owns the same adapter
     * session while responding to the resulting request. */
    result = ERROR_NOT_READY;
    for (attempt = 0; attempt < 3 && result != NO_ERROR && ctx.error == 0; ++attempt) {
        Sleep(1000);
        mac_length = 6;
        result = SendARP(peer.s_addr, local.s_addr, received_mac, &mac_length);
    }
    InterlockedExchange(&ctx.stop, 1);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    chmlink_close(ctx.adapter);
    ok = result == NO_ERROR && mac_length == 6 &&
         memcmp(received_mac, expected_mac, 6) == 0 && ctx.replied > 0 && ctx.error == 0;
    if (!ok) {
        fprintf(stderr, "ARP test failed: SendARP=%lu replies=%ld worker_error=%ld mac_length=%lu\n",
                result, ctx.replied, ctx.error, mac_length);
        return 1;
    }
    printf("PASS: Windows resolved the simulated peer through ChmLink (%ld ARP response(s)).\n", ctx.replied);
    return 0;
}
