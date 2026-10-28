#ifndef CHMLINK_PACKET_H
#define CHMLINK_PACKET_H

#include <stddef.h>
#include <stdint.h>

/* The values are part of the user-mode/driver contract. */
typedef enum chmlink_mode {
    CHMLINK_MODE_ETHERNET = 1,
    CHMLINK_MODE_IP = 2
} chmlink_mode;

/*
 * Check only the minimum structural requirements. Ethernet frames do not
 * contain an FCS. IP version is checked, but packet lengths and checksums are
 * the responsibility of the network stack. max_length is the adapter's limit.
 */
int chmlink_packet_valid(chmlink_mode mode, const uint8_t *data,
                         size_t length, size_t max_length);

#endif
