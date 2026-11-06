#ifndef CHMLINK_MANAGE_H
#define CHMLINK_MANAGE_H

/* Creating, changing and deleting ChmLink adapters (Windows only; requires
 * an administrator). Like the rest of the C API, functions return nonzero on
 * success and report errors through GetLastError().
 *
 * Every adapter records an owner GUID. Delete and MTU changes must name the
 * same owner, so that one application never removes another's adapters; a
 * NULL owner means CHMLINK_DEFAULT_OWNER_ID. Changes are serialized across
 * processes; a call waits up to ten seconds for another one to finish and then
 * fails with ERROR_BUSY.
 */
#include <stdint.h>
#include <wchar.h>
#include <guiddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* {7C71F369-78C2-44F4-94CD-A2C14A29469B} */
#define CHMLINK_DEFAULT_OWNER_ID \
    {0x7c71f369, 0x78c2, 0x44f4, {0x94, 0xcd, 0xa2, 0xc1, 0x4a, 0x29, 0x46, 0x9b}}

/* size must be sizeof(chmlink_create_options); zero the rest for defaults. */
typedef struct chmlink_create_options {
    uint32_t size;
    uint32_t mode;             /* CHMLINK_MODE_ETHERNET (default when 0) or CHMLINK_MODE_IP */
    const wchar_t *name;       /* connection name; NULL generates "ChmLink-<adapter id>" */
    const GUID *owner;         /* NULL: CHMLINK_DEFAULT_OWNER_ID */
    const uint8_t *mac;        /* Ethernet only, 6 bytes; NULL: random, locally administered */
    uint32_t mtu;              /* 0: 1500. IP 1280 to 65535; Ethernet 1280 to 9000 (driver 0.5.1.6 or later) */
    const wchar_t *driver_inf; /* NULL: the package embedded in chmlink.dll (see below) */
} chmlink_create_options;

/* flags */
#define CHMLINK_DEVICE_REBOOT_REQUIRED 0x1u /* Windows asks for a system restart */

typedef struct chmlink_device {
    uint32_t mode;             /* CHMLINK_MODE_* */
    uint32_t mtu;              /* configured; applies from the next device start */
    uint32_t flags;            /* CHMLINK_DEVICE_* from the call that filled this in */
    uint32_t interface_index;  /* 0 while Windows has no interface for it */
    uint8_t mac[6];            /* all zero for IP mode */
    GUID adapter_id;           /* stable identity, fixed at creation */
    GUID owner_id;
    GUID interface_guid;       /* the network interface GUID; zero when unknown */
    wchar_t name[129];         /* the name chosen at creation */
    wchar_t instance_id[200];  /* the device instance ID, e.g. ROOT\NET\0001 */
} chmlink_device;

/* Install a new adapter and wait until Windows has its network interface,
 * which then carries the requested name.
 *
 * Which driver: all adapters share one driver package. With driver_inf NULL,
 * the package embedded in chmlink.dll is compared with the one the existing
 * adapters run, and the newer wins: an older package is first upgraded on
 * every adapter (their data sessions end), unless one of them has a session
 * open; then the existing package is kept. A chmlink.dll built without a
 * package fails with ERROR_RESOURCE_NAME_NOT_FOUND. With driver_inf set, the
 * existing adapters must already run that very package (identical INF, SYS
 * and CAT), and no other adapter is changed.
 *
 * Fails with ERROR_DUP_NAME when the name, MAC or identity is in use, and
 * ERROR_REVISION_MISMATCH for an unsupported package, or when the existing
 * adapters run another one that cannot be replaced. On failure the new
 * adapter is removed again, except when even that failed: then the error is
 * ERROR_INSTALL_FAILURE, and chmlink_enumerate_devices shows the device.
 * created may be NULL. */
int chmlink_create_adapter(const chmlink_create_options *options, chmlink_device *created);

/* Remove an adapter. An open data session ends. */
int chmlink_delete_adapter(const wchar_t *instance_id, const GUID *owner);

/* Store a new MTU for an adapter (IP 1280 to 65535, Ethernet 1280 to 9000) and
 * restart the device so that it takes effect; an open data session ends. A
 * driver package too old for the value fails with ERROR_REVISION_MISMATCH. When the MTU was stored but the restart
 * failed, the error is ERROR_INSTALL_FAILURE and the change applies at the
 * next restart. flags may be NULL. */
int chmlink_set_adapter_mtu(const wchar_t *instance_id, const GUID *owner, uint32_t mtu,
                            uint32_t *flags);

/* Read one adapter. ERROR_FILE_NOT_FOUND when it is a ChmLink device without
 * a configuration (unmanaged), ERROR_ACCESS_DENIED when not ChmLink at all. */
int chmlink_get_device(const wchar_t *instance_id, chmlink_device *device);

/* Every configured adapter, or only those of owner when not NULL. Devices
 * whose configuration cannot be read are skipped. Return zero from the
 * callback to stop early (still a success). */
/* Remove every ChmLink driver package from the Driver Store that no adapter
 * uses, for example when uninstalling. Fails with the error of the last
 * package Windows refused (ERROR_INF_IN_USE_BY_DEVICES while adapters exist),
 * after removing the others. Once no adapter and no package is left, the
 * driver service and any copy of chmlink.sys in System32\drivers go too. */
int chmlink_delete_driver(void);

typedef int (*chmlink_device_callback)(const chmlink_device *device, void *context);
int chmlink_enumerate_devices(const GUID *owner, chmlink_device_callback callback, void *context);

#ifdef __cplusplus
}
#endif
#endif
