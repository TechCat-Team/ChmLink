#ifndef CHMLINK_DEVICE_H
#define CHMLINK_DEVICE_H

/* Experimental Windows device contract. Include windows.h or ntddk.h first. */
#include "version.h"
#define CHMLINK_ETHERNET_MTU 1500u
#define CHMLINK_ETHERNET_LIMIT 1522u
#define CHMLINK_QUEUE_PACKETS 256u
#define CHMLINK_IOCTL_RECEIVE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
#define CHMLINK_IOCTL_INJECT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)
#define CHMLINK_IOCTL_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA)
#define CHMLINK_IOCTL_REGISTER_READ_EVENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_WRITE_DATA)
/* REGISTER_READ_EVENT takes exactly one little-endian uint64 user event
 * handle, no output. One registration per data session, in caller context.
 * The driver validates access/type and references the event until cleanup.
 * The SDK supplies a private manual-reset event; applications must not alter
 * it. Signaled means data available OR a lifecycle transition needs checking.
 * This request itself never waits and does not retain an IRP.
 */
#define CHMLINK_INFO_SIZE 128u

/* QUERY has no input and returns 128 explicitly little-endian bytes:
 * 0 version, 4 size, 8 capabilities, 12 state, 16 mode, 20 ifIndex,
 * 24 MTU, 28 packet limit, 32 queue capacity, 36 queued packets,
 * 40 filter, 44 reserved, 48 MAC[6], 54 reserved[2], 56 NET_LUID[8],
 * 64 reserved[8], 72 TxPackets, 80 TxBytes, 88 TxErrors,
 * 96 RxPackets, 104 RxBytes, 112 RxDiscards, 120 TxQueueDrops.
 * Offsets 72..120 are uint64 counters; other numeric fields are uint32
 * except NET_LUID. Counters are cumulative for the adapter instance.
 * A read-only handle may query without connecting the network link.
 */

/* Open \\.\ChmLink<interface-index> with read/write access for a data session,
 * or read-only access for inspection; allow read and write sharing. The
 * driver enforces one data session independently of read-only handles.
 * RECEIVE returns a protocol.h batch, or ERROR_NO_MORE_ITEMS (polling).
 * INJECT accepts a validated Ethernet batch. All requests are synchronous;
 * native structures and pointers never cross the device boundary.
 */

#endif
