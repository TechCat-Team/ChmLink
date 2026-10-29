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
    FILE *file;
    size_t length;
    int result;
    if (argc != 3) {
        fprintf(stderr, "Usage: chmlink-inject <interface-index> <ethernet-frame-file>\n");
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
    if (result || length < 14 || length > CHMLINK_ETHERNET_LIMIT) {
        fprintf(stderr, "Expected one Ethernet frame of 14..1522 bytes without FCS.\n"); return 1;
    }
    if (chmlink_batch_begin(&writer, batch, sizeof(batch), CHMLINK_MODE_ETHERNET,
                           CHMLINK_ETHERNET_LIMIT) != CHMLINK_OK ||
        chmlink_batch_append(&writer, frame, length) != CHMLINK_OK) return 1;
    adapter = chmlink_open((uint32_t)index);
    if (adapter == NULL) { fprintf(stderr, "Open failed: %lu\n", GetLastError()); return 1; }
    result = chmlink_inject(adapter, batch, (uint32_t)writer.length);
    if (!result) fprintf(stderr, "Inject failed: %lu\n", GetLastError());
    else printf("Submitted %lu frame bytes; delivery depends on the adapter packet filter.\n",
                (unsigned long)length);
    chmlink_close(adapter);
    return result ? 0 : 1;
}
