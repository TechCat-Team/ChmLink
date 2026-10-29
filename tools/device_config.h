#ifndef CHMLINK_DEVICE_CONFIG_H
#define CHMLINK_DEVICE_CONFIG_H

#include <windows.h>
#include <setupapi.h>
#include "chmlink/config.h"

typedef struct CL_DEVICE_CONFIG {
    BYTE Bytes[CHMLINK_CONFIG_SIZE];
    wchar_t Name[129];
} CL_DEVICE_CONFIG;

BOOL ClIsDevice(HDEVINFO set, SP_DEVINFO_DATA *device);
BOOL ClReadConfig(HDEVINFO set, SP_DEVINFO_DATA *device, CL_DEVICE_CONFIG *config);
BOOL ClNewConfig(CL_DEVICE_CONFIG *config, const wchar_t *owner, const wchar_t *name,
                 const wchar_t *mac);
BOOL ClCheckUnique(HDEVINFO set, SP_DEVINFO_DATA *skip, const CL_DEVICE_CONFIG *config);
BOOL ClWriteConfig(HDEVINFO set, SP_DEVINFO_DATA *device, const CL_DEVICE_CONFIG *config);
BOOL ClCheckOwner(const CL_DEVICE_CONFIG *config, const wchar_t *owner);
int ClShowConfig(const wchar_t *instance);
int ClAdoptDevice(const wchar_t *instance, const wchar_t *owner, const wchar_t *name);

#endif
