#ifndef CHMLINK_DEVICE_H
#define CHMLINK_DEVICE_H

/* Experimental Windows device contract. Include windows.h or ntddk.h first. */
#include "version.h"
#include "query.h"
#define CHMLINK_ETHERNET_MTU 1500u      /* the default; configurable per device */
/* An Ethernet frame is its MTU plus a header with up to two VLAN tags, without FCS. */
#define CHMLINK_ETHERNET_OVERHEAD 22u
#define CHMLINK_ETHERNET_LIMIT (CHMLINK_ETHERNET_MTU + CHMLINK_ETHERNET_OVERHEAD)
/* The MTU is configured per device (chmlink/config.h: 1500 by default; IP 1280
 * to 65535, Ethernet 1280 to 9000). QUERY reports it as mtu, and as
 * packet_limit the largest packet: the MTU for IP, MTU + 22 for Ethernet. */
/* Packets move through two shared-memory rings per session (chmlink/ring.h):
 * one the driver produces for Windows traffic, one the application produces
 * for injection. The driver registers both with CHMLINK_IOCTL_REGISTER_RINGS
 * and reports their capacity and the unread bytes in QUERY; there is no
 * per-batch IOCTL on the data path. */
#define CHMLINK_IOCTL_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA)
#define CHMLINK_IOCTL_REGISTER_RINGS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_WRITE_DATA)
#define CHMLINK_IOCTL_SET_OFFLOAD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_WRITE_DATA)
/* ADD_INJECT_RING: chmlink/session.h. Fails with STATUS_INVALID_DEVICE_STATE
 * before REGISTER_RINGS or when the session has CHMLINK_INJECT_RINGS_MAX. */
#define CHMLINK_IOCTL_ADD_INJECT_RING CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_WRITE_DATA)
/* REGISTER_RINGS: input is the CHMLINK_SESSION_SIZE-byte request from
 * chmlink/session.h, no output. Data session handles only, at most once per
 * handle; a second registration fails with STATUS_INVALID_DEVICE_STATE. The
 * driver resets both ring headers, keeps the pages locked and the events
 * referenced until the handle is closed, and reports ring state through the
 * ring's state field. */
/* SET_OFFLOAD: input one little-endian uint32 of CHMLINK_OFFLOAD_* bits the
 * session can handle, output one uint32 of the bits now in effect (a subset:
 * what the adapter supports and the system has not switched off). Data
 * session handles only; may be sent again to change the set, and is reset
 * when the session ends. With any bit in effect:
 *   The from-Windows ring returns descriptors saying what the session must
 *     still do: NEEDS_CSUM (TX_CHECKSUM) and GSO (TX_LSO).
 *   The to-Windows ring accepts descriptors: DATA_VALID marks checksums as
 *     verified (RX_CHECKSUM); a GSO TCP packet is indicated as one coalesced
 *     segment (RX_RSC, only while Windows has receive segment coalescing
 *     enabled for that IP version; otherwise the packet is dropped).
 * The CHMLINK_OFFLOAD_* bits are in version.h. */
/* QUERY has no input and returns the CHMLINK_INFO_SIZE-byte little-endian
 * response defined in query.h. Use chmlink_query_encode/decode; never
 * hand-code offsets. Counters are cumulative for the adapter instance.
 * A read-only handle may query without connecting the network link.
 */

/* Open \\.\ChmLink<interface-index> with read/write access for a data session,
 * or read-only access for inspection; allow read and write sharing. The
 * driver enforces one data session independently of read-only handles.
 * All requests are synchronous; native structures and pointers never cross
 * the device boundary. IP packets have no link-layer header. */

#endif
