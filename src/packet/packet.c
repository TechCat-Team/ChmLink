#include "chmlink/packet.h"

int chmlink_ip_packet_valid(const uint8_t *data, size_t length, size_t max_length)
{
    size_t header, declared;
    unsigned int version;
    if (data == NULL || length == 0 || length > max_length) return 0;
    version = data[0] >> 4;
    if (version == 4) {
        if (length < 20) return 0;
        header = (size_t)(data[0] & 15u) * 4u;
        declared = ((size_t)data[2] << 8) | data[3];
        return header >= 20 && header <= length && declared >= header && declared == length;
    }
    if (version == 6) {
        if (length < 40) return 0;
        declared = ((size_t)data[4] << 8) | data[5];
        /* A zero payload is valid for a 40-byte packet; a longer packet with
         * zero payload length would require unsupported jumbo-option parsing. */
        return declared == length - 40;
    }
    return 0;
}

int chmlink_packet_valid(chmlink_mode mode, const uint8_t *data,
                         size_t length, size_t max_length)
{
    if (data == NULL || length == 0 || length > max_length) {
        return 0;
    }

    if (mode == CHMLINK_MODE_ETHERNET) {
        return length >= 14;
    }
    if (mode != CHMLINK_MODE_IP) {
        return 0;
    }

    return chmlink_ip_packet_valid(data, length, max_length);
}
