#ifndef CHMLINK_VERSION_H
#define CHMLINK_VERSION_H

/* Single source of the driver package version. driver/chmlink.inf DriverVer
 * must match CHMLINK_VERSION_STRING; the version_test CTest enforces this. */
#define CHMLINK_VERSION_MAJOR 0u
#define CHMLINK_VERSION_MINOR 3u
#define CHMLINK_VERSION_PATCH 0u
#define CHMLINK_VERSION_BUILD 0u
#define CHMLINK_VERSION_STRING "0.3.0.0"
/* OID_GEN_VENDOR_DRIVER_VERSION: major in the high word, minor in the low. */
#define CHMLINK_VENDOR_DRIVER_VERSION \
    ((CHMLINK_VERSION_MAJOR << 16) | CHMLINK_VERSION_MINOR)

/* Device interface contract version, independent of the package version. */
#define CHMLINK_DEVICE_VERSION 3u
#define CHMLINK_CAP_ETHERNET 0x01u
#define CHMLINK_CAP_BATCH 0x02u
#define CHMLINK_CAP_STATISTICS 0x04u
#define CHMLINK_CAP_WAIT 0x08u
#define CHMLINK_CAP_IP 0x10u
#define CHMLINK_STATE_RUNNING 0x01u
#define CHMLINK_STATE_SESSION 0x02u
#define CHMLINK_STATE_PAUSED 0x04u
#define CHMLINK_STATE_PAUSING 0x08u
#define CHMLINK_STATE_REMOVING 0x10u

#endif
