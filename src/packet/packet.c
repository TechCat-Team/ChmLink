#include "chmlink/packet.h"

int chmlink_packet_valid(chmlink_mode mode, const uint8_t *data,
                         size_t length, size_t max_length)
{
    unsigned int version;

    if (data == NULL || length == 0 || length > max_length) {
        return 0;
    }

    if (mode == CHMLINK_MODE_ETHERNET) {
        return length >= 14;
    }
    if (mode != CHMLINK_MODE_IP) {
        return 0;
    }

    version = data[0] >> 4;
    return (version == 4 && length >= 20) ||
           (version == 6 && length >= 40);
}
