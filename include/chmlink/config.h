#ifndef CHMLINK_CONFIG_H
#define CHMLINK_CONFIG_H

/* Device hardware-key value, independent of the network class software key.
 * One REG_BINARY value is committed before the first driver installation.
 * Header uint32 values are little endian; GUIDs use Windows GUID byte order.
 * 0 magic 'CLCF', 4 version, 8 size, 12 mode, 13 reserved[3], 16 adapter
 * GUID, 32 owner GUID, 48 Ethernet MAC or zero for IP, 54 reserved[10].
 * Identity is immutable for the lifetime of a device instance.
 */
#define CHMLINK_CONFIG_VALUE L"ChmLinkConfig"
#define CHMLINK_CONFIG_NAME L"ChmLinkName"
#define CHMLINK_CONFIG_SIZE 64u
#define CHMLINK_CONFIG_VERSION 2u
#define CHMLINK_CONFIG_MODE_ETHERNET 1u
#define CHMLINK_CONFIG_MODE_IP 2u
#define CHMLINK_DEFAULT_OWNER L"{7C71F369-78C2-44F4-94CD-A2C14A29469B}"

static __inline int chmlink_config_valid(const unsigned char *p, unsigned int size)
{
    unsigned int i, adapter = 0, owner = 0;
    if (size != CHMLINK_CONFIG_SIZE || p[0] != 'C' || p[1] != 'L' ||
        p[2] != 'C' || p[3] != 'F' ||
        p[4] != CHMLINK_CONFIG_VERSION ||
        p[5] || p[6] || p[7] || p[8] != CHMLINK_CONFIG_SIZE ||
        p[9] || p[10] || p[11]) return 0;
    if (p[12] != CHMLINK_CONFIG_MODE_ETHERNET && p[12] != CHMLINK_CONFIG_MODE_IP) return 0;
    if (p[12] == CHMLINK_CONFIG_MODE_ETHERNET) {
        if ((p[48] & 3) != 2) return 0;
    } else {
        for (i = 48; i < 54; ++i) if (p[i]) return 0;
    }
    for (i = 13; i < 16; ++i) if (p[i]) return 0;
    for (i = 54; i < CHMLINK_CONFIG_SIZE; ++i) if (p[i]) return 0;
    for (i = 0; i < 16; ++i) { adapter |= p[16 + i]; owner |= p[32 + i]; }
    return adapter != 0 && owner != 0;
}

static __inline unsigned int chmlink_config_mode(const unsigned char *p)
{
    return p[12];
}

#endif
