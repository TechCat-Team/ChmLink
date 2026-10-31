#ifndef CHMLINK_PACKET_H
#define CHMLINK_PACKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The values are part of the user-mode/driver contract. */
typedef enum chmlink_mode {
    CHMLINK_MODE_ETHERNET = 1,
    CHMLINK_MODE_IP = 2
} chmlink_mode;

/*
 * Check only the minimum structural requirements. Ethernet frames do not
 * contain an FCS. IP packets use strict declared-length framing; checksums and
 * transport semantics remain the responsibility of the network stack.
 * max_length is the adapter's limit.
 */
int chmlink_packet_valid(chmlink_mode mode, const uint8_t *data,
                         size_t length, size_t max_length);

/* Strict framing for the experimental raw-IP data path. Accept exactly one
 * complete IPv4 or IPv6 packet; no checksum or transport validation is done.
 * IPv6 jumbograms are not supported. Fragmented IP packets are permitted. */
int chmlink_ip_packet_valid(const uint8_t *data, size_t length, size_t max_length);

/* A hash of the packet's flow for spreading packets over queues: IPv4/IPv6
 * addresses, the protocol and, for TCP and UDP outside fragments, the ports.
 * Packets of one flow hash alike, in both directions; other packets (ARP, short or unknown ones)
 * hash to 0. Ethernet frames may carry one 802.1Q tag. Reads only what length
 * covers; no validation beyond that. */
uint32_t chmlink_flow_hash(chmlink_mode mode, const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
