#ifndef CHMLINK_CONFIG_H
#define CHMLINK_CONFIG_H

/* Device hardware-key value, independent of the network class software key.
 * One REG_BINARY value is committed before the first driver installation.
 * Header uint32 values are little endian; GUIDs use Windows GUID byte order.
 * 0 magic 'CLCF', 4 version, 8 size, 12 mode, 13 reserved[3], 16 adapter
 * GUID, 32 owner GUID, 48 Ethernet MAC or zero for IP, 54 reserved[2],
 * 56 uint32 MTU, 60 reserved[4].
 * Identity (mode, GUIDs, MAC) is immutable for the lifetime of a device
 * instance; the MTU can be changed, and takes effect when the device restarts.
 * The MTU is the largest IP packet the adapter carries: 1500 in Ethernet mode,
 * 1280 (the IPv6 minimum) to 65535 in IP mode. Version 2, written by earlier
 * tools, has no MTU field (bytes 54..63 are zero) and means 1500.
 */
#define CHMLINK_CONFIG_VALUE L"ChmLinkConfig"
#define CHMLINK_CONFIG_NAME L"ChmLinkName"
#define CHMLINK_CONFIG_SIZE 64u
#define CHMLINK_CONFIG_MODE 12u        /* uint8 */
#define CHMLINK_CONFIG_ADAPTER_ID 16u  /* GUID[16] */
#define CHMLINK_CONFIG_OWNER_ID 32u    /* GUID[16] */
#define CHMLINK_CONFIG_ID_SIZE 16u
#define CHMLINK_CONFIG_MAC 48u         /* uint8[6] */
#define CHMLINK_CONFIG_MAC_SIZE 6u
#define CHMLINK_CONFIG_MTU 56u         /* uint32, little endian */
#define CHMLINK_CONFIG_VERSION 3u
#define CHMLINK_CONFIG_VERSION_NO_MTU 2u
#define CHMLINK_MTU_DEFAULT 1500u
#define CHMLINK_IP_MTU_MIN 1280u
#define CHMLINK_IP_MTU_MAX 65535u
#define CHMLINK_CONFIG_MODE_ETHERNET 1u
#define CHMLINK_CONFIG_MODE_IP 2u
#define CHMLINK_DEFAULT_OWNER L"{7C71F369-78C2-44F4-94CD-A2C14A29469B}"

static __inline unsigned int chmlink_config_read32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static __inline void chmlink_config_set_mtu(unsigned char *p, unsigned int mtu)
{
    p[CHMLINK_CONFIG_MTU] = (unsigned char)mtu;
    p[CHMLINK_CONFIG_MTU + 1] = (unsigned char)(mtu >> 8);
    p[CHMLINK_CONFIG_MTU + 2] = (unsigned char)(mtu >> 16);
    p[CHMLINK_CONFIG_MTU + 3] = (unsigned char)(mtu >> 24);
}

/* Whether `mtu` is allowed for a device of `mode` (CHMLINK_CONFIG_MODE_*). */
static __inline int chmlink_config_mtu_allowed(unsigned int mode, unsigned int mtu)
{
    if (mode == CHMLINK_CONFIG_MODE_ETHERNET) return mtu == CHMLINK_MTU_DEFAULT;
    return mtu >= CHMLINK_IP_MTU_MIN && mtu <= CHMLINK_IP_MTU_MAX;
}

static __inline int chmlink_config_valid(const unsigned char *p, unsigned int size)
{
    unsigned int i, adapter = 0, owner = 0;
    if (size != CHMLINK_CONFIG_SIZE || p[0] != 'C' || p[1] != 'L' ||
        p[2] != 'C' || p[3] != 'F' ||
        (p[4] != CHMLINK_CONFIG_VERSION && p[4] != CHMLINK_CONFIG_VERSION_NO_MTU) ||
        p[5] || p[6] || p[7] || p[8] != CHMLINK_CONFIG_SIZE ||
        p[9] || p[10] || p[11]) return 0;
    if (p[CHMLINK_CONFIG_MODE] != CHMLINK_CONFIG_MODE_ETHERNET &&
        p[CHMLINK_CONFIG_MODE] != CHMLINK_CONFIG_MODE_IP) return 0;
    if (p[CHMLINK_CONFIG_MODE] == CHMLINK_CONFIG_MODE_ETHERNET) {
        if ((p[CHMLINK_CONFIG_MAC] & 3) != 2) return 0;
    } else {
        for (i = CHMLINK_CONFIG_MAC; i < CHMLINK_CONFIG_MAC + CHMLINK_CONFIG_MAC_SIZE; ++i)
            if (p[i]) return 0;
    }
    for (i = CHMLINK_CONFIG_MODE + 1; i < CHMLINK_CONFIG_ADAPTER_ID; ++i) if (p[i]) return 0;
    for (i = CHMLINK_CONFIG_MAC + CHMLINK_CONFIG_MAC_SIZE; i < CHMLINK_CONFIG_SIZE; ++i) {
        if (p[4] == CHMLINK_CONFIG_VERSION && i >= CHMLINK_CONFIG_MTU && i < CHMLINK_CONFIG_MTU + 4) continue;
        if (p[i]) return 0;
    }
    if (p[4] == CHMLINK_CONFIG_VERSION &&
        !chmlink_config_mtu_allowed(p[CHMLINK_CONFIG_MODE], chmlink_config_read32(p + CHMLINK_CONFIG_MTU)))
        return 0;
    for (i = 0; i < CHMLINK_CONFIG_ID_SIZE; ++i) {
        adapter |= p[CHMLINK_CONFIG_ADAPTER_ID + i];
        owner |= p[CHMLINK_CONFIG_OWNER_ID + i];
    }
    return adapter != 0 && owner != 0;
}

static __inline unsigned int chmlink_config_mode(const unsigned char *p)
{
    return p[CHMLINK_CONFIG_MODE];
}

/* The MTU of a valid configuration. */
static __inline unsigned int chmlink_config_mtu(const unsigned char *p)
{
    return p[4] == CHMLINK_CONFIG_VERSION ? chmlink_config_read32(p + CHMLINK_CONFIG_MTU) : CHMLINK_MTU_DEFAULT;
}

#endif
