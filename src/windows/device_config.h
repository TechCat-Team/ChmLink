/* Internal to the SDK and chmlink-device: the stored adapter configuration
 * (chmlink/config.h) and the driver package checks behind chmlink/manage.h.
 * Not installed or exported. */
#ifndef CHMLINK_DEVICE_CONFIG_H
#define CHMLINK_DEVICE_CONFIG_H

#include <windows.h>
#include <setupapi.h>
#include <stdint.h>
#include "chmlink/config.h"

typedef struct CL_DEVICE_CONFIG {
    BYTE Bytes[CHMLINK_CONFIG_SIZE];
    wchar_t Name[129];
} CL_DEVICE_CONFIG;

/* Packages from 0.3.0.0 read configuration version 2; from 0.4.2.0 also
 * version 3 (with the MTU), which creation writes. */
#define CL_PACKAGE(a, b, c, d) (((unsigned long long)(a) << 48) | ((unsigned long long)(b) << 32) | \
                                ((unsigned long long)(c) << 16) | (unsigned long long)(d))
#define CL_PACKAGE_PARTS(p) (unsigned int)((p) >> 48), (unsigned int)((p) >> 32 & 0xffff), \
                            (unsigned int)((p) >> 16 & 0xffff), (unsigned int)((p) & 0xffff)
#define CL_PACKAGE_CONFIG_2 CL_PACKAGE(0, 3, 0, 0)
#define CL_PACKAGE_CONFIG_3 CL_PACKAGE(0, 4, 2, 0)
/* The oldest package whose contract range includes this SDK's contract
 * (CHMLINK_DEVICE_VERSION): adapters this SDK creates need at least this one.
 * Raise it together with CHMLINK_DEVICE_VERSION. */
#define CL_PACKAGE_SDK CL_PACKAGE(0, 4, 3, 0)

extern const wchar_t ClHardwareId[], ClIpHardwareId[];

BOOL ClIsDevice(HDEVINFO set, SP_DEVINFO_DATA *device);
unsigned int ClDeviceMode(HDEVINFO set, SP_DEVINFO_DATA *device);
BOOL ClReadConfig(HDEVINFO set, SP_DEVINFO_DATA *device, CL_DEVICE_CONFIG *config);
/* mac NULL: random for Ethernet; mtu 0 means the default (1500). */
BOOL ClNewConfig(CL_DEVICE_CONFIG *config, const GUID *owner, const wchar_t *name,
                 const BYTE *mac, unsigned int mode, unsigned int mtu);
BOOL ClCheckUnique(const CL_DEVICE_CONFIG *config);
BOOL ClWriteConfig(HDEVINFO set, SP_DEVINFO_DATA *device, const CL_DEVICE_CONFIG *config);
/* Replace only the MTU of the stored configuration (`current`, as read), which
 * also moves it to configuration version 3. */
BOOL ClUpdateMtu(HDEVINFO set, SP_DEVINFO_DATA *device, const CL_DEVICE_CONFIG *current, unsigned int mtu);
BOOL ClCheckOwner(const CL_DEVICE_CONFIG *config, const GUID *owner);
BOOL ClParseGuid(const wchar_t *text, GUID *id);
BOOL ClParseMac(const wchar_t *text, BYTE mac[6]);

BOOL ClIsOemName(const wchar_t *name);
/* The package's DriverVer as CL_PACKAGE(...); fails below CL_PACKAGE_CONFIG_2. */
BOOL ClPackageVersion(const wchar_t *inf, unsigned long long *package);
/* The bound driver's version as CL_PACKAGE(...), or 0 when unknown. */
unsigned long long ClBoundPackage(HDEVINFO set, SP_DEVINFO_DATA *device);
/* Select from one signed INF and install it on one existing devnode. */
BOOL ClInstallOnDevice(HDEVINFO set, SP_DEVINFO_DATA *device, const wchar_t *inf, BOOL *reboot);

/* The network interface of a device: its GUID once the class installer has
 * assigned one, and (index not NULL) its index once NDIS has bound it. */
BOOL ClInterfaceOf(HDEVINFO set, SP_DEVINFO_DATA *device, GUID *guid, uint32_t *index);
/* Whether an interface belongs to an adapter that no longer exists. */
BOOL ClIsStaleInterface(const GUID *guid);

/* package.c */
/* Every ChmLink device runs exactly this package (identical INF, SYS, CAT). */
BOOL ClCheckPackage(const wchar_t *inf);
/* The package a new adapter installs given the embedded one; may upgrade the
 * existing adapters to it. */
BOOL ClSelectPackage(const wchar_t *embedded, wchar_t chosen[MAX_PATH], BOOL *reboot);
/* ERROR_RESOURCE_NAME_NOT_FOUND when the module embeds no package. */
BOOL ClExtractDriver(wchar_t directory[MAX_PATH], wchar_t inf[MAX_PATH]);
void ClRemoveExtracted(const wchar_t *directory);
/* The INF has this project's schema (hardware ID and service name). */
BOOL ClIsOwnPackage(const wchar_t *inf);

/* The process-wide lock of every configuration change. */
BOOL ClLock(HANDLE *mutex);
void ClUnlock(HANDLE mutex);

#endif
