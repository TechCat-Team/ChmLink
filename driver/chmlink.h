#ifndef CHMLINK_DRIVER_H
#define CHMLINK_DRIVER_H

#include <ntddk.h>
#include <ndis.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include "chmlink/device.h"
#include "chmlink/protocol.h"
#include "chmlink/config.h"
#include "chmlink/ring.h"
#include "chmlink/session.h"

typedef char CL_MODE_IP_CONTRACT_MATCH[(CHMLINK_CONFIG_MODE_IP == CHMLINK_MODE_IP) ? 1 : -1];
typedef char CL_MODE_ETHERNET_CONTRACT_MATCH[(CHMLINK_CONFIG_MODE_ETHERNET == CHMLINK_MODE_ETHERNET) ? 1 : -1];

#define CL_TAG 'kLmC'
#define CL_FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
    NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_PROMISCUOUS)
#define CL_MULTICAST_MAX 32

/* Driver-internal, fine-grained offload bits. A session asks for the four
 * coarse CHMLINK_OFFLOAD_* bits; OffloadAllowed holds what the adapter can do
 * and has not been told to switch off through OID_TCP_OFFLOAD_PARAMETERS. */
#define CL_TX_TCP4 0x0001u
#define CL_TX_UDP4 0x0002u
#define CL_TX_TCP6 0x0004u
#define CL_TX_UDP6 0x0008u
#define CL_RX_IP4 0x0010u
#define CL_RX_TCP4 0x0020u
#define CL_RX_UDP4 0x0040u
#define CL_RX_TCP6 0x0080u
#define CL_RX_UDP6 0x0100u
#define CL_LSO4 0x0200u
#define CL_LSO6 0x0400u
#define CL_RSC4 0x0800u
#define CL_RSC6 0x1000u
#define CL_OFFLOAD_FINE_ALL 0x1fffu

/* One batch of indicated NBLs, released in order. ~64 batches of up to 64
 * packets each cover the outstanding bound. */
#define CL_BATCH_QUEUE_SIZE 64
#define CL_SESSION_BATCH_MAX 64
#define CL_SESSION_OUTSTANDING_MAX 4096
/* Receive spreading: at most this many indicating threads per session. */
#define CL_SPREAD_MAX 4

typedef enum CL_ADAPTER_STATE {
    ClAdapterInitializing, ClAdapterPaused, ClAdapterRunning,
    ClAdapterPausing, ClAdapterRemoving
} CL_ADAPTER_STATE;

typedef enum CL_SESSION_STATE {
    ClSessionClosed, ClSessionOpen, ClSessionClosing
} CL_SESSION_STATE;

typedef struct CL_ADAPTER CL_ADAPTER;

/* One shared-memory ring registered by a session. */
typedef struct CL_RING {
    PMDL Mdl;               /* locks the user pages */
    PVOID Address;          /* system view of the same memory */
    chmlink_ring View;
    PKEVENT Event;          /* referenced user event */
    ULONG Capacity;
} CL_RING;

/* End position and number of indicated NBLs the consumer must still return.
 * Id identifies the batch for ClReturn; a value of zero is never used. */
typedef struct CL_BATCH {
    uint32_t End;
    uint32_t Id;
    LONG Pending;
} CL_BATCH;

struct CL_SESSION;

/* One to-Windows ring and the kernel thread that indicates its records. A
 * session has one per producing application thread, up to
 * CHMLINK_INJECT_RINGS_MAX, so indications run on several cores. */
typedef struct CL_RXQ {
    struct CL_SESSION *Session;
    CL_RING Ring;           /* application produces packets for Windows */
    uint32_t ReadCursor;
    CL_BATCH Batches[CL_BATCH_QUEUE_SIZE];
    ULONG BatchHead;
    ULONG BatchCount;
    KSPIN_LOCK BatchLock;
    uint32_t NextBatchId;
    LONG Outstanding;       /* this queue's NBLs not yet returned */
    LONGLONG IdleGap;       /* smoothed time the ring stays empty before data, QPC ticks */
    LONGLONG EmptySince;    /* QPC time the ring was found empty; 0 while it has data */
    KEVENT StateEvent;      /* auto-reset: state change or a return */
    PETHREAD Thread;
    BOOLEAN Stop;           /* this queue only (a failed ADD_INJECT_RING) */
} CL_RXQ;

/* Receive spreading, as RSS does on a NIC: the stack processes an indication
 * on the indicating thread, so a ring thread hands the packets of some flows
 * to these threads (by chmlink_flow_hash) to run them on more cores. A flow
 * always maps to one worker, which indicates in arrival order. */
typedef struct CL_WORKER {
    struct CL_SESSION *Session;
    KSPIN_LOCK Lock;
    PNET_BUFFER_LIST First, Last;   /* queued, protected by Lock */
    ULONG Count;
    LONG InFlight;          /* queued or being indicated by this worker */
    KEVENT Event;           /* auto-reset: work or stop */
    PETHREAD Thread;
    BOOLEAN Stop;
} CL_WORKER;

typedef struct CL_SESSION {
    CL_ADAPTER *Adapter;
    CL_RING From;           /* driver produces packets Windows sent */
    /* Send path (ClSend, any CPU <= DISPATCH). */
    KSPIN_LOCK SendLock;
    uint32_t SendCursor;
    EX_RUNDOWN_REF SendRundown;
    /* Receive path: Rx[0] from REGISTER_RINGS, more from ADD_INJECT_RING.
     * RxCount changes under the adapter lock and ControlMutex. */
    CL_RXQ Rx[CHMLINK_INJECT_RINGS_MAX];
    ULONG RxCount;
    /* Flow buckets; with fewer than two (one processor), ring threads indicate
     * everything themselves. Fixed while ring threads run. */
    CL_WORKER Workers[CL_SPREAD_MAX];
    ULONG WorkerCount;
    BOOLEAN WorkersStop;
    KEVENT StopEvent;
    BOOLEAN Stop;
    BOOLEAN Broken;
} CL_SESSION;

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
    CL_SESSION *Session; /* protected by Lock */
    /* NBLs indicated to the stack and not yet returned, and the event that
     * turns signaled when the count reaches zero. On the adapter so pause
     * does not need the session. */
    LONG RxOutstanding;
    KEVENT RxDrained;
    BOOLEAN IoDrained; /* NDIS lifecycle callbacks are serialized */
    BOOLEAN Removed;
    /* Written under Lock, read without it where a stale value only lets one
     * more request run into the paused path. SurpriseRemoved is never cleared;
     * ShuttingDown is cleared by a return to D0. */
    BOOLEAN SurpriseRemoved;
    BOOLEAN ShuttingDown;
    NDIS_DEVICE_POWER_STATE PowerState; /* protected by Lock */
    ULONG InterfaceIndex;
    NET_LUID Luid;
    ULONG PacketFilter;
    ULONG Lookahead;
    ULONG MulticastCount;
    UCHAR Multicast[CL_MULTICAST_MAX][6];
    UCHAR Mac[6];
    chmlink_mode Mode;
    UCHAR Configuration[CHMLINK_CONFIG_SIZE];
    /* Offloads, all protected by Lock. */
    ULONG OffloadAllowed;
    ULONG OffloadSession;
    ULONG64 RscPackets, RscOctets, RscEvents;
    ULONG64 TxPackets, TxBytes, TxErrors;
    ULONG64 RxPackets, RxBytes, RxDiscards;
    ULONG64 TxQueueDrops;
    /* Largest IP packet, from the device configuration. */
    ULONG Mtu;
} CL_ADAPTER;

/* Mode and Mtu are fixed during ClInitialize and never change afterwards, so
 * these helpers are safe without holding the adapter lock. */
static __inline ULONG ClPacketLimit(const CL_ADAPTER *a)
{
    return a->Mode == CHMLINK_MODE_IP ? a->Mtu : a->Mtu + CHMLINK_ETHERNET_OVERHEAD;
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
void ClIndicateLink(CL_ADAPTER *adapter);
NDIS_STATUS ClRegisterControl(CL_ADAPTER *adapter, NET_IFINDEX index);
NDIS_STATUS ClReadConfiguration(NDIS_HANDLE adapter, UCHAR *configuration);

/* Session. ClSessionStateLocked runs under a->Lock and tells both rings the
 * adapter state; it also wakes the application and the session thread. */
NTSTATUS ClSessionRegister(CL_ADAPTER *adapter, PIRP irp, ULONG length);
NTSTATUS ClSessionAddRing(CL_ADAPTER *adapter, PIRP irp, ULONG length);
void ClSessionTeardown(CL_ADAPTER *adapter, CL_SESSION *session);
void ClSessionStateLocked(CL_ADAPTER *adapter);
void ClSessionReturnLists(CL_ADAPTER *adapter, PNET_BUFFER_LIST lists);

/* Task offloads (driver/offload.c). */
ULONG ClOffloadEffectiveLocked(const CL_ADAPTER *adapter);
ULONG ClOffloadGrantedLocked(const CL_ADAPTER *adapter);
ULONG ClOffloadHardware(const CL_ADAPTER *adapter);
NDIS_STATUS ClSetOffloadAttributes(CL_ADAPTER *adapter);
void ClIndicateOffload(CL_ADAPTER *adapter);
void ClOffloadFill(const CL_ADAPTER *adapter, NDIS_OFFLOAD *offload, ULONG fine);
NDIS_STATUS ClOffloadParametersLocked(CL_ADAPTER *adapter, const void *buffer, ULONG length, BOOLEAN *changed);
/* Fills the descriptor for one send NBL and rewrites IP length fields the
 * stack left for the offload. FALSE means the packet must be dropped. */
BOOLEAN ClDescribeSend(CL_ADAPTER *adapter, PNET_BUFFER_LIST nbl, UCHAR *packet,
    ULONG length, ULONG fine, chmlink_offload *descriptor);
/* Applies an injected descriptor to an NBL. FALSE means the caller must
 * refuse it (malformed, or an offload the session did not get). */
BOOLEAN ClPrepareInjected(CL_ADAPTER *adapter, PNET_BUFFER_LIST nbl, const UCHAR *packet,
    ULONG length, const chmlink_offload *descriptor, ULONG fine);

#endif
