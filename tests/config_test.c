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
    p[16] = 1; p[32] = 2; p[48] = 2;
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
    return 0;
}
