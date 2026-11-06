#include <stdio.h>
#include <string.h>
#include "chmlink/config.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void baseline(unsigned char *p)
{
    memset(p, 0, CHMLINK_CONFIG_SIZE);
    memcpy(p, "CLCF", 4);
    p[4] = CHMLINK_CONFIG_VERSION;
    p[8] = CHMLINK_CONFIG_SIZE;
    p[12] = CHMLINK_CONFIG_MODE_ETHERNET;
    p[16] = 1; p[32] = 2; p[48] = 2;
    chmlink_config_set_mtu(p, CHMLINK_MTU_DEFAULT);
}

int main(void)
{
    unsigned char bytes[CHMLINK_CONFIG_SIZE];
    unsigned int i, value;
    baseline(bytes);
    CHECK(chmlink_config_valid(bytes, sizeof(bytes)));
    for (i = 0; i < 128; ++i)
        if (i != CHMLINK_CONFIG_SIZE) CHECK(!chmlink_config_valid(bytes, i));
    /* Every header/reserved byte is fixed. Reject single-byte corruption,
     * including high bytes that must not truncate to the supported version. */
    for (i = 0; i < CHMLINK_CONFIG_SIZE; ++i) {
        if (i >= 16 && i < 54) continue;
        if (i >= CHMLINK_CONFIG_MTU && i < CHMLINK_CONFIG_MTU + 4) continue;   /* below */
        for (value = 0; value < 256; ++value) {
            unsigned char saved = bytes[i];
            bytes[i] = (unsigned char)value;
            CHECK(chmlink_config_valid(bytes, sizeof(bytes)) == (value == saved));
            bytes[i] = saved;
        }
    }
    /* Only locally administered unicast addresses are accepted, independent
     * of the remaining address bits. Broadcast and global addresses fail. */
    for (value = 0; value < 256; ++value) {
        bytes[48] = (unsigned char)value;
        CHECK(chmlink_config_valid(bytes, sizeof(bytes)) == ((value & 3) == 2));
    }
    baseline(bytes); memset(bytes + 16, 0, 16);
    CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    bytes[31] = 1; CHECK(chmlink_config_valid(bytes, sizeof(bytes)));
    memset(bytes + 32, 0, 16); CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    bytes[47] = 1; CHECK(chmlink_config_valid(bytes, sizeof(bytes)));
    baseline(bytes); bytes[12] = CHMLINK_CONFIG_MODE_IP;
    memset(bytes + 48, 0, 6);
    CHECK(chmlink_config_valid(bytes, sizeof(bytes)));
    CHECK(chmlink_config_mode(bytes) == CHMLINK_CONFIG_MODE_IP);
    bytes[48] = 2; CHECK(!chmlink_config_valid(bytes, sizeof(bytes))); bytes[48] = 0;
    bytes[12] = CHMLINK_CONFIG_MODE_ETHERNET; CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    bytes[12] = CHMLINK_CONFIG_MODE_IP; bytes[4] = 4; CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    bytes[4] = 1; bytes[12] = 0; CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));

    /* MTU: Ethernet from 1280 to 9000; IP from 1280 to 65535. */
    baseline(bytes);
    CHECK(chmlink_config_mtu(bytes) == 1500);
    for (value = 0; value < 70000; value += (value < 1600 || (value > 8900 && value < 9100) ? 1 : 97)) {
        chmlink_config_set_mtu(bytes, value);
        CHECK(chmlink_config_valid(bytes, sizeof(bytes)) == (value >= 1280 && value <= 9000));
        if (value >= 1280 && value <= 9000) CHECK(chmlink_config_mtu(bytes) == value);
    }
    chmlink_config_set_mtu(bytes, 9000); CHECK(chmlink_config_valid(bytes, sizeof(bytes)));
    chmlink_config_set_mtu(bytes, 9001); CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    chmlink_config_set_mtu(bytes, 1279); CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    /* The version 2 layout has no MTU field: such a device always runs at 1500. */
    baseline(bytes); bytes[4] = CHMLINK_CONFIG_VERSION_NO_MTU; memset(bytes + CHMLINK_CONFIG_MTU, 0, 4);
    CHECK(chmlink_config_valid(bytes, sizeof(bytes)) && chmlink_config_mtu(bytes) == 1500);
    baseline(bytes); bytes[12] = CHMLINK_CONFIG_MODE_IP; memset(bytes + 48, 0, 6);
    for (value = 0; value < 70000; value += (value < 1600 ? 1 : 97)) {
        chmlink_config_set_mtu(bytes, value);
        CHECK(chmlink_config_valid(bytes, sizeof(bytes)) == (value >= 1280 && value <= 65535));
        if (value >= 1280 && value <= 65535) CHECK(chmlink_config_mtu(bytes) == value);
    }
    chmlink_config_set_mtu(bytes, 65535); CHECK(chmlink_config_valid(bytes, sizeof(bytes)));
    chmlink_config_set_mtu(bytes, 65536); CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    chmlink_config_set_mtu(bytes, 0x80000500u); CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    chmlink_config_set_mtu(bytes, 9000);
    bytes[54] = 1; CHECK(!chmlink_config_valid(bytes, sizeof(bytes))); bytes[54] = 0;
    bytes[63] = 1; CHECK(!chmlink_config_valid(bytes, sizeof(bytes))); bytes[63] = 0;
    CHECK(chmlink_config_valid(bytes, sizeof(bytes)) && chmlink_config_mtu(bytes) == 9000);

    /* Version 2 (earlier tools): no MTU field, which reads as 1500. */
    baseline(bytes); bytes[4] = CHMLINK_CONFIG_VERSION_NO_MTU;
    CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));       /* MTU bytes must be zero */
    chmlink_config_set_mtu(bytes, 0);
    CHECK(chmlink_config_valid(bytes, sizeof(bytes)) && chmlink_config_mtu(bytes) == 1500);
    bytes[12] = CHMLINK_CONFIG_MODE_IP; memset(bytes + 48, 0, 6);
    CHECK(chmlink_config_valid(bytes, sizeof(bytes)) && chmlink_config_mtu(bytes) == 1500);
    bytes[57] = 0x23; CHECK(!chmlink_config_valid(bytes, sizeof(bytes)));
    return 0;
}
