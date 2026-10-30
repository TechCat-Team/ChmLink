#ifndef CHMLINK_DEVICE_H
#define CHMLINK_DEVICE_H

/* Experimental Windows device contract. Include windows.h or ntddk.h first. */
#include "version.h"
#include "query.h"
#define CHMLINK_ETHERNET_MTU 1500u
#define CHMLINK_ETHERNET_LIMIT 1522u
#define CHMLINK_IP_MTU 1500u
#define CHMLINK_IP_LIMIT CHMLINK_IP_MTU
/* Per-adapter transmit queue (Windows -> user mode): a byte ring of
 * CHMLINK_QUEUE_BYTES holding at most CHMLINK_QUEUE_PACKETS packets. Small
 * packets are bounded by the count, large ones by the bytes (about 2900
 * full-size Ethernet frames). QUERY reports the packet bound as capacity. */
#define CHMLINK_QUEUE_PACKETS 16384u
#define CHMLINK_QUEUE_BYTES (4u * 1024u * 1024u)
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
/* QUERY has no input and returns the CHMLINK_INFO_SIZE-byte little-endian
 * response defined in query.h. Use chmlink_query_encode/decode; never
 * hand-code offsets. Counters are cumulative for the adapter instance.
 * A read-only handle may query without connecting the network link.
 */

/* Open \\.\ChmLink<interface-index> with read/write access for a data session,
 * or read-only access for inspection; allow read and write sharing. The
 * driver enforces one data session independently of read-only handles.
 * RECEIVE returns a protocol.h batch, or ERROR_NO_MORE_ITEMS (polling).
 * INJECT accepts a validated batch matching the adapter mode. IP packets have
 * no link-layer header. All requests are synchronous;
 * native structures and pointers never cross the device boundary.
 */

#endif
