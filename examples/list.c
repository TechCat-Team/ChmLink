#include <windows.h>
#include <stdio.h>
#include <inttypes.h>
#include "chmlink/api.h"

static int show(const chmlink_adapter_info *info, void *context)
{
    unsigned int *count = context;
    (*count)++;
    printf("%s index=%" PRIu32 " mode=%" PRIu32 " mtu=%" PRIu32
           " state=0x%" PRIx32 " capabilities=0x%" PRIx32 "\n",
           info->interface_id, info->interface_index, info->mode, info->mtu,
           info->state, info->capabilities);
    printf("  queued=%" PRIu32 "/%" PRIu32 " tx=%" PRIu64
           " rx=%" PRIu64 " queue_drops=%" PRIu64 "\n",
           info->queued_packets, info->queue_capacity, info->tx_packets,
           info->rx_packets, info->tx_queue_drops);
    return 1;
}

int main(void)
{
    unsigned int count = 0;
    if (!chmlink_enumerate(show, &count)) {
        fprintf(stderr, "Enumeration failed: %lu\n", GetLastError());
        return 1;
    }
    if (count == 0) puts("No ChmLink adapters found.");
    return 0;
}
