#include "arp_peer.h"
#include <string.h>

int arp_make_reply(const uint8_t *f, size_t length, const uint8_t local_ip[4],
                   const uint8_t peer_ip[4], const uint8_t peer_mac[6], uint8_t r[42])
{
    if (!f || !local_ip || !peer_ip || !peer_mac || !r || length < 42 ||
        f[12] != 0x08 || f[13] != 0x06 || f[14] != 0 || f[15] != 1 ||
        f[16] != 0x08 || f[17] != 0 || f[18] != 6 || f[19] != 4 ||
        f[20] != 0 || f[21] != 1 || memcmp(f + 28, local_ip, 4) ||
        memcmp(f + 38, peer_ip, 4) || memcmp(f + 6, f + 22, 6)) return 0;
    memcpy(r, f + 22, 6);
    memcpy(r + 6, peer_mac, 6);
    memcpy(r + 12, f + 12, 8);
    r[20] = 0; r[21] = 2;
    memcpy(r + 22, peer_mac, 6);
    memcpy(r + 28, peer_ip, 4);
    memcpy(r + 32, f + 22, 6);
    memcpy(r + 38, local_ip, 4);
    return 1;
}
