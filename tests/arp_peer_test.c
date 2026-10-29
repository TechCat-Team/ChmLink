#include "arp_peer.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
int main(void)
{
    const uint8_t local[4] = {169,254,1,1}, peer[4] = {169,254,1,2};
    const uint8_t mac[6] = {2,1,2,3,4,5}, sender[6] = {2,9,8,7,6,5};
    uint8_t req[42] = {0}, reply[42] = {0};
    memset(req, 255, 6); memcpy(req + 6, sender, 6);
    req[12]=8; req[13]=6; req[15]=1; req[16]=8; req[18]=6; req[19]=4; req[21]=1;
    memcpy(req+22,sender,6); memcpy(req+28,local,4); memcpy(req+38,peer,4);
    CHECK(arp_make_reply(req,42,local,peer,mac,reply));
    CHECK(!memcmp(reply,sender,6) && !memcmp(reply+6,mac,6));
    CHECK(reply[20]==0 && reply[21]==2 && !memcmp(reply+22,mac,6));
    CHECK(!memcmp(reply+28,peer,4) && !memcmp(reply+32,sender,6) && !memcmp(reply+38,local,4));
    CHECK(!arp_make_reply(req,41,local,peer,mac,reply));
    req[21]=2; CHECK(!arp_make_reply(req,42,local,peer,mac,reply));
    req[21]=1; req[38]^=1; CHECK(!arp_make_reply(req,42,local,peer,mac,reply));
    req[38]^=1; req[22]^=1; CHECK(!arp_make_reply(req,42,local,peer,mac,reply));
    return 0;
}
