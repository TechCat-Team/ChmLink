#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

static volatile LONG stopping;

static BOOL WINAPI stop(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&stopping, 1);
    return TRUE;
}

int main(int argc, char **argv)
{
    unsigned long index;
    char *end;
    chmlink_adapter *adapter;
    uint8_t *buffer;
    int result = 0;
    if (argc != 2) { fprintf(stderr, "Usage: chmlink-capture <interface-index>\n"); return 2; }
    errno = 0;
    index = strtoul(argv[1], &end, 10);
    if (errno || *end || !index || argv[1][0] == '-') return 2;
    buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    if (!buffer) return 1;
    adapter = chmlink_open((uint32_t)index);
    if (!adapter) { fprintf(stderr, "Open failed: %lu\n", GetLastError()); free(buffer); return 1; }
    SetConsoleCtrlHandler(stop, TRUE);
    while (!InterlockedCompareExchange(&stopping, 0, 0)) {
        uint32_t received, count;
        if (!chmlink_receive(adapter, buffer, CHMLINK_BATCH_MAX_SIZE, &received)) {
            DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS || error == ERROR_NOT_READY) { Sleep(10); continue; }
            fprintf(stderr, "Receive failed: %lu\n", error); result = 1; break;
        }
        if (chmlink_batch_validate(buffer, received, CHMLINK_MODE_ETHERNET, 1522, &count) == CHMLINK_OK)
            printf("Received %lu frames (%lu batch bytes)\n", (unsigned long)count, (unsigned long)received);
    }
    chmlink_close(adapter);
    free(buffer);
    return result;
}
