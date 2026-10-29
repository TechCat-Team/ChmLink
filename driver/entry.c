#include "chmlink.h"

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
    c.MajorDriverVersion = 0;
    c.MinorDriverVersion = 1;
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

_Use_decl_annotations_
void ClPnp(NDIS_HANDLE context, PNET_DEVICE_PNP_EVENT event)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(event);
}

_Use_decl_annotations_
void ClShutdown(NDIS_HANDLE context, NDIS_SHUTDOWN_ACTION action)
{
    CL_ADAPTER *a = context;
    UNREFERENCED_PARAMETER(action);
    /* Shutdown may run at high IRQL. No allocation, waiting, or I/O here. */
    UNREFERENCED_PARAMETER(a);
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
