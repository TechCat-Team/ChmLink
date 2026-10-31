#ifndef CHMLINK_TEST_ARP_PEER_H
#define CHMLINK_TEST_ARP_PEER_H
#include <stddef.h>
#include <stdint.h>

/* Respond only to Ethernet/IPv4 ARP requests for the selected peer address. */
int arp_make_reply(const uint8_t *frame, size_t length,
                   const uint8_t local_ip[4], const uint8_t peer_ip[4],
                   const uint8_t peer_mac[6], uint8_t reply[42]);
#endif
