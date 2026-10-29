#ifndef CHMLINK_DRIVER_H
#define CHMLINK_DRIVER_H

#include <ntddk.h>
#include <ndis.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include "chmlink/device.h"
#include "chmlink/protocol.h"

#define CL_TAG 'kLmC'
#define CL_FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
    NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS)
#define CL_MULTICAST_MAX 32

typedef struct CL_PACKET {
    ULONG Length;
    UCHAR Bytes[CHMLINK_ETHERNET_LIMIT];
} CL_PACKET;

typedef struct CL_ADAPTER {
    LIST_ENTRY Link;
    LONG References;
    NDIS_HANDLE Handle;
    NDIS_HANDLE Pool;
    NDIS_HANDLE ControlHandle;
    PDEVICE_OBJECT ControlObject;
    EX_RUNDOWN_REF IoRundown;
    KSPIN_LOCK Lock;
    KMUTEX ControlMutex;
    BOOLEAN Running;
    BOOLEAN Removed;
    BOOLEAN Session;
    ULONG PacketFilter;
    ULONG Lookahead;
    ULONG MulticastCount;
    UCHAR Multicast[CL_MULTICAST_MAX][6];
    UCHAR Mac[6];
    CL_PACKET *Queue;
    ULONG Head;
    ULONG Count;
    ULONG64 TxPackets, TxBytes, TxErrors;
    ULONG64 RxPackets, RxBytes, RxDiscards;
} CL_ADAPTER;

extern NDIS_HANDLE ClDriverHandle;
extern LIST_ENTRY ClAdapters;
extern KSPIN_LOCK ClAdaptersLock;
extern const NDIS_OID ClOids[];
extern const ULONG ClOidBytes;

DRIVER_INITIALIZE DriverEntry;
MINIPORT_UNLOAD ClUnload;
MINIPORT_INITIALIZE ClInitialize;
MINIPORT_HALT ClHalt;
MINIPORT_PAUSE ClPause;
MINIPORT_RESTART ClRestart;
MINIPORT_SEND_NET_BUFFER_LISTS ClSend;
MINIPORT_RETURN_NET_BUFFER_LISTS ClReturn;
MINIPORT_OID_REQUEST ClOidRequest;
MINIPORT_CANCEL_OID_REQUEST ClCancelOid;
MINIPORT_CANCEL_SEND ClCancelSend;
MINIPORT_DEVICE_PNP_EVENT_NOTIFY ClPnp;
MINIPORT_SHUTDOWN ClShutdown;
DRIVER_DISPATCH ClDispatch;

void ClRelease(CL_ADAPTER *adapter);
void ClIndicateLink(CL_ADAPTER *adapter);
NDIS_STATUS ClRegisterControl(CL_ADAPTER *adapter, NET_IFINDEX index);

#endif
