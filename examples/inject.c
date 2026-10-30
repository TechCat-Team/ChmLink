#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "chmlink/api.h"
#include "chmlink/device.h"
#include "chmlink/protocol.h"

int main(int argc, char **argv)
{
    unsigned long index;
    char *end;
    uint8_t frame[CHMLINK_ETHERNET_LIMIT + 1];
    uint8_t batch[CHMLINK_ETHERNET_LIMIT + CHMLINK_BATCH_HEADER_SIZE + 8];
    chmlink_batch_writer writer;
    chmlink_adapter *adapter;
    chmlink_adapter_info info;
    FILE *file;
    size_t length;
    int result;
    if (argc != 3) {
        fprintf(stderr, "Usage: chmlink-inject <interface-index> <packet-file>\n");
        return 2;
    }
    errno = 0;
    index = strtoul(argv[1], &end, 10);
    if (errno || *end || !index || argv[1][0] == '-') return 2;
    file = fopen(argv[2], "rb");
    if (file == NULL) { perror("Open frame file"); return 1; }
    length = fread(frame, 1, sizeof(frame), file);
    result = ferror(file);
    fclose(file);
    if (result || length > CHMLINK_ETHERNET_LIMIT) {
        fprintf(stderr, "Packet file is unreadable or exceeds the adapter limit.\n"); return 1;
    }
    adapter = chmlink_open((uint32_t)index);
    if (adapter == NULL) { fprintf(stderr, "Open failed: %lu\n", GetLastError()); return 1; }
    if (!chmlink_query(adapter, &info) ||
        chmlink_batch_begin(&writer, batch, sizeof(batch), (chmlink_mode)info.mode, info.packet_limit) != CHMLINK_OK ||
        chmlink_batch_append(&writer, frame, length) != CHMLINK_OK) {
        fprintf(stderr, "Invalid packet for this adapter.\n"); chmlink_close(adapter); return 1;
    }
    result = chmlink_inject(adapter, batch, (uint32_t)writer.length);
    if (!result) fprintf(stderr, "Inject failed: %lu\n", GetLastError());
    else printf("Submitted %lu packet bytes.\n",
                (unsigned long)length);
    chmlink_close(adapter);
    return result ? 0 : 1;
}
