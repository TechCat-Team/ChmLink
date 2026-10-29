#include "chmlink/protocol.h"

/* Build with Clang's -fsanitize=fuzzer,address,undefined. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)chmlink_batch_validate(data, size, CHMLINK_MODE_ETHERNET,
                               CHMLINK_PACKET_MAX_SIZE, NULL);
    (void)chmlink_batch_validate(data, size, CHMLINK_MODE_IP,
                               CHMLINK_PACKET_MAX_SIZE, NULL);
    return 0;
}
