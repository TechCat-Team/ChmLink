#include "chmlink.h"

typedef char CL_VERSION_FITS_CHARACTERISTICS[
    (CHMLINK_VERSION_MAJOR <= 0xff && CHMLINK_VERSION_MINOR <= 0xff) ? 1 : -1];

NDIS_HANDLE ClDriverHandle;
LIST_ENTRY ClAdapters;
KSPIN_LOCK ClAdaptersLock;

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS c;
    InitializeListHead(&ClAdapters);
    KeInitializeSpinLock(&ClAdaptersLock);
    NdisZeroMemory(&c, sizeof(c));
    c.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    c.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.MajorNdisVersion = 6;
    c.MinorNdisVersion = 60;
    c.MajorDriverVersion = (UCHAR)CHMLINK_VERSION_MAJOR;
    c.MinorDriverVersion = (UCHAR)CHMLINK_VERSION_MINOR;
    c.InitializeHandlerEx = ClInitialize;
    c.HaltHandlerEx = ClHalt;
    c.UnloadHandler = ClUnload;
    c.PauseHandler = ClPause;
    c.RestartHandler = ClRestart;
    c.SendNetBufferListsHandler = ClSend;
    c.ReturnNetBufferListsHandler = ClReturn;
    c.OidRequestHandler = ClOidRequest;
    c.CancelOidRequestHandler = ClCancelOid;
    c.CancelSendHandler = ClCancelSend;
    c.DevicePnPEventNotifyHandler = ClPnp;
    c.ShutdownHandlerEx = ClShutdown;
    return NdisMRegisterMiniportDriver(object, path, NULL, &c, &ClDriverHandle);
}

_Use_decl_annotations_
void ClUnload(PDRIVER_OBJECT object)
{
    UNREFERENCED_PARAMETER(object);
    NdisMDeregisterMiniportDriver(ClDriverHandle);
}

/* PASSIVE_LEVEL. A surprise removal is followed by pause and halt; until then
 * new requests fail as removed instead of running into a device that is gone,
 * and a waiting session wakes up to see REMOVING. */
_Use_decl_annotations_
void ClPnp(NDIS_HANDLE context, PNET_DEVICE_PNP_EVENT event)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    if (event->DevicePnPEvent != NdisDevicePnPEventSurpriseRemoved) return;   /* power profile: nothing to adapt */
    KeAcquireSpinLock(&a->Lock, &irql);
    a->SurpriseRemoved = TRUE;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
}

/* NdisShutdownPowerOff runs at PASSIVE_LEVEL while the system powers off,
 * without a pause or halt: close the data path and wake a waiting session.
 * NdisShutdownBugCheck runs at HIGH_LEVEL; nothing may be touched then. */
_Use_decl_annotations_
void ClShutdown(NDIS_HANDLE context, NDIS_SHUTDOWN_ACTION action)
{
    CL_ADAPTER *a = context;
    KIRQL irql;
    if (action != NdisShutdownPowerOff) return;
    KeAcquireSpinLock(&a->Lock, &irql);
    a->ShuttingDown = TRUE;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
}

_Use_decl_annotations_
void ClCancelOid(NDIS_HANDLE context, PVOID id)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(id);
    /* OID requests are completed synchronously. */
}

_Use_decl_annotations_
void ClCancelSend(NDIS_HANDLE context, PVOID id)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(id);
    /* No NBL is retained after the send callback. */
}

_Use_decl_annotations_
void ClReturn(NDIS_HANDLE context, PNET_BUFFER_LIST lists, ULONG flags)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(lists);
    UNREFERENCED_PARAMETER(flags);
    /* All indications carry RESOURCES; NDIS must not return these NBLs. */
    NT_ASSERT(FALSE);
}
