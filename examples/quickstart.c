/* chmlink-quickstart: the whole lifecycle in one program. It creates an IP
 * adapter (installing the driver embedded in chmlink.dll when needed), gives
 * Windows the address 192.0.2.1/24 on it, and answers pings to 192.0.2.2 by
 * turning each ICMP echo request around. The adapter is deleted on exit.
 *
 *   chmlink-quickstart [seconds]   answer pings for that long (default 60)
 *   chmlink-quickstart --self-test ping 192.0.2.2 from this program, then exit
 *
 * Run it as administrator. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"

static volatile LONG stopping, logged;

/* The SDK says what it does and why something failed. */
static void print_log(chmlink_log_level level, const wchar_t *message, void *context)
{
    static const wchar_t *const names[] = {L"info", L"warning", L"error"};
    (void)context;
    fwprintf(stderr, L"[chmlink %ls] %ls\n", names[level], message);
    InterlockedIncrement(&logged);
}

static void print_version(const char *what, uint64_t v)
{
    printf("%s %u.%u.%u.%u\n", what, (unsigned int)(v >> 48), (unsigned int)(v >> 32 & 0xffff),
           (unsigned int)(v >> 16 & 0xffff), (unsigned int)(v & 0xffff));
}
static uint8_t scratch[65535];

static BOOL WINAPI on_console(DWORD event)
{
    (void)event;
    InterlockedExchange(&stopping, 1);
    return TRUE;
}

/* Turn an IPv4 ICMP echo request into its reply, in place. Returns 0 for any
 * other packet. */
static int make_reply(uint8_t *ip, uint32_t size)
{
    uint32_t header, total;
    uint8_t address[4];
    uint16_t sum;
    if (size < 20 || ip[0] >> 4 != 4) return 0;
    header = (uint32_t)(ip[0] & 15) * 4;
    total = (uint32_t)ip[2] << 8 | ip[3];
    if (header < 20 || total > size || total < header + 8 || ip[9] != 1 || ip[header] != 8) return 0;
    /* Swapping the addresses leaves the IP header checksum valid. */
    memcpy(address, ip + 12, 4); memcpy(ip + 12, ip + 16, 4); memcpy(ip + 16, address, 4);
    ip[8] = 64;                       /* fresh TTL: recompute the header checksum */
    ip[10] = ip[11] = 0;
    sum = (uint16_t)~chmlink_checksum_add(ip, header, 0);
    ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
    ip[header] = 0;                   /* echo reply */
    ip[header + 2] = ip[header + 3] = 0;
    sum = (uint16_t)~chmlink_checksum_add(ip + header, total - header, 0);
    ip[header + 2] = (uint8_t)(sum >> 8); ip[header + 3] = (uint8_t)sum;
    return 1;
}

static DWORD WINAPI self_test(void *context)
{
    HANDLE icmp = IcmpCreateFile();
    char reply[sizeof(ICMP_ECHO_REPLY) + 64], data[32] = "ChmLink quickstart";
    IN_ADDR peer;
    int i, answered = 0;
    (void)context;
    InetPtonA(AF_INET, "192.0.2.2", &peer);
    for (i = 0; i < 3 && icmp != INVALID_HANDLE_VALUE; ++i) {
        if (IcmpSendEcho(icmp, peer.S_un.S_addr, data, sizeof(data), NULL, reply, sizeof(reply), 2000) == 1 &&
            ((ICMP_ECHO_REPLY *)reply)->Status == IP_SUCCESS) {
            printf("reply from 192.0.2.2 in %lu ms\n", ((ICMP_ECHO_REPLY *)reply)->RoundTripTime);
            answered++;
        }
    }
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
    InterlockedExchange(&stopping, 1);
    return answered == 3 ? 0 : 1;
}

/* 192.0.2.1/24 on the adapter, and wait until Windows may use it. */
static int add_address(uint32_t index)
{
    MIB_UNICASTIPADDRESS_ROW row;
    ULONGLONG deadline = GetTickCount64() + 10000;
    DWORD error;
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceIndex = index;
    row.Address.Ipv4.sin_family = AF_INET;
    InetPtonA(AF_INET, "192.0.2.1", &row.Address.Ipv4.sin_addr);
    row.OnLinkPrefixLength = 24;
    row.DadState = IpDadStatePreferred;
    error = CreateUnicastIpAddressEntry(&row);
    if (error != NO_ERROR) { SetLastError(error); return 0; }
    while (GetUnicastIpAddressEntry(&row) == NO_ERROR && row.DadState != IpDadStatePreferred) {
        if (GetTickCount64() > deadline) { SetLastError(ERROR_TIMEOUT); return 0; }
        Sleep(50);
    }
    return 1;
}

int main(int argc, char **argv)
{
    chmlink_create_options options;
    chmlink_device device;
    chmlink_adapter *adapter;
    chmlink_adapter_info info;
    uint64_t driver = 0;
    HANDLE tester = NULL;
    ULONGLONG deadline;
    unsigned long seconds = 60, replies = 0;
    DWORD tested = 0;
    int test = argc == 2 && strcmp(argv[1], "--self-test") == 0;
    if (argc == 2 && !test) seconds = strtoul(argv[1], NULL, 10);
    if (argc > 2 || seconds == 0) { fprintf(stderr, "Usage: chmlink-quickstart [seconds | --self-test]\n"); return 2; }

    chmlink_set_logger(print_log, NULL);
    print_version("chmlink.dll", chmlink_get_sdk_version());

    /* 1. Create the adapter. Defaults: random identity, MTU 1500, the
     *    embedded driver, CHMLINK_DEFAULT_OWNER_ID as owner. */
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.mode = CHMLINK_MODE_IP;
    options.name = L"ChmLink Quickstart";
    if (!chmlink_create_adapter(&options, &device)) {
        fprintf(stderr, "Creating the adapter failed: %lu\n", GetLastError()); return 1;
    }
    printf("created %ls, interface %u\n", device.name, device.interface_index);
    if (chmlink_get_driver_version(&driver)) print_version("driver", driver);

    /* 2. Open the data session, then address the interface. */
    adapter = chmlink_open(device.interface_index);
    if (adapter == NULL || !add_address(device.interface_index)) {
        fprintf(stderr, "Setting up the adapter failed: %lu\n", GetLastError());
        if (adapter) chmlink_close(adapter);
        chmlink_delete_adapter(device.instance_id, NULL);
        return 1;
    }
    if (chmlink_query(adapter, &info))
        printf("driver contracts %u to %u, this SDK speaks %u\n", info.min_version, info.version,
               (unsigned int)CHMLINK_DEVICE_VERSION);
    SetConsoleCtrlHandler(on_console, TRUE);
    if (test) tester = CreateThread(NULL, 0, self_test, NULL, 0, NULL);
    else printf("try: ping 192.0.2.2   (%lu s, Ctrl+C stops)\n", seconds);

    /* 3. Move packets: Windows sends (receive_packet), we inject (send_packet). */
    deadline = GetTickCount64() + (test ? 15000 : (ULONGLONG)seconds * 1000);
    while (!stopping && GetTickCount64() < deadline) {
        uint32_t size;
        const uint8_t *packet;
        if (!chmlink_wait_readable(adapter, 100)) {
            if (GetLastError() == ERROR_TIMEOUT) continue;
            fprintf(stderr, "Waiting failed: %lu\n", GetLastError()); break;
        }
        while ((packet = chmlink_receive_packet(adapter, &size, NULL)) != NULL) {
            uint8_t *reply;
            if (size <= sizeof(scratch)) {
                memcpy(scratch, packet, size);
                if (make_reply(scratch, size) && (reply = chmlink_allocate_packet(adapter, size, NULL)) != NULL) {
                    memcpy(reply, scratch, size);
                    chmlink_send_packet(adapter, reply);
                    replies++;
                }
            }
            chmlink_release_packet(adapter, packet);
        }
    }

    /* 4. Close the session and delete the adapter. */
    if (tester) { WaitForSingleObject(tester, INFINITE); GetExitCodeThread(tester, &tested); CloseHandle(tester); }
    chmlink_close(adapter);
    if (!chmlink_delete_adapter(device.instance_id, NULL)) {
        fprintf(stderr, "Deleting the adapter failed: %lu\n", GetLastError()); return 1;
    }
    printf("answered %lu pings; adapter deleted\n", replies);
    if (test) {
        /* CI builds the DLL and the driver from one tree: the versions match,
         * and creating and deleting were logged. */
        if (driver != chmlink_get_sdk_version() || logged < 2) tested = 1;
        puts(tested == 0 ? "PASS quickstart self-test" : "FAIL quickstart self-test");
    }
    return (int)tested;
}
