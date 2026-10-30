#ifndef CHMLINK_DRIVER_H
#define CHMLINK_DRIVER_H

#include <ntddk.h>
#include <ndis.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include "chmlink/device.h"
#include "chmlink/protocol.h"
#include "chmlink/config.h"

typedef char CL_MODE_IP_CONTRACT_MATCH[(CHMLINK_CONFIG_MODE_IP == CHMLINK_MODE_IP) ? 1 : -1];
typedef char CL_MODE_ETHERNET_CONTRACT_MATCH[(CHMLINK_CONFIG_MODE_ETHERNET == CHMLINK_MODE_ETHERNET) ? 1 : -1];

#define CL_TAG 'kLmC'
#define CL_FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
    NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS)
#define CL_MULTICAST_MAX 32

typedef struct CL_PACKET {
    ULONG Length;
    UCHAR Bytes[CHMLINK_ETHERNET_LIMIT];
} CL_PACKET;

typedef enum CL_ADAPTER_STATE {
    ClAdapterInitializing, ClAdapterPaused, ClAdapterRunning,
    ClAdapterPausing, ClAdapterRemoving
} CL_ADAPTER_STATE;

typedef enum CL_SESSION_STATE {
    ClSessionClosed, ClSessionOpen, ClSessionClosing
} CL_SESSION_STATE;

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
    CL_ADAPTER_STATE State; /* protected by Lock */
    CL_SESSION_STATE SessionState; /* protected by Lock */
    PKEVENT ReadEvent; /* referenced object; protected by Lock */
    BOOLEAN IoDrained; /* NDIS lifecycle callbacks are serialized */
    BOOLEAN Removed;
    ULONG InterfaceIndex;
    NET_LUID Luid;
    ULONG PacketFilter;
    ULONG Lookahead;
    ULONG MulticastCount;
    UCHAR Multicast[CL_MULTICAST_MAX][6];
    UCHAR Mac[6];
    chmlink_mode Mode;
    UCHAR Configuration[CHMLINK_CONFIG_SIZE];
    CL_PACKET *Queue;
    ULONG Head;
    ULONG Count;
    ULONG64 TxPackets, TxBytes, TxErrors;
    ULONG64 RxPackets, RxBytes, RxDiscards;
    ULONG64 TxQueueDrops;
} CL_ADAPTER;

/* Mode is fixed during ClInitialize and never changes afterwards, so these
 * helpers are safe without holding the adapter lock. */
static __inline ULONG ClPacketLimit(const CL_ADAPTER *a)
{
    return a->Mode == CHMLINK_MODE_IP ? CHMLINK_IP_LIMIT : CHMLINK_ETHERNET_LIMIT;
}

/* Smallest frame Windows may hand us: an IPv4 header or an Ethernet header. */
static __inline ULONG ClPacketMinimum(const CL_ADAPTER *a)
{
    return a->Mode == CHMLINK_MODE_IP ? 20u : 14u;
}

extern NDIS_HANDLE ClDriverHandle;
extern LIST_ENTRY ClAdapters;
extern KSPIN_LOCK ClAdaptersLock;
extern const NDIS_OID ClOids[];
extern const ULONG ClOidBytes;
extern const NDIS_OID ClIpOids[];
extern const ULONG ClIpOidBytes;

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
void ClUpdateReadEventLocked(CL_ADAPTER *adapter);
void ClDetachReadEvent(CL_ADAPTER *adapter);
void ClIndicateLink(CL_ADAPTER *adapter);
NDIS_STATUS ClRegisterControl(CL_ADAPTER *adapter, NET_IFINDEX index);
NDIS_STATUS ClReadConfiguration(NDIS_HANDLE adapter, UCHAR *configuration);

#endif
