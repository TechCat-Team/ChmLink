#ifndef CHMLINK_DEVICE_H
#define CHMLINK_DEVICE_H

/* Experimental Windows device contract. Include windows.h or ntddk.h first. */
#define CHMLINK_DEVICE_VERSION 1u
#define CHMLINK_ETHERNET_MTU 1500u
#define CHMLINK_ETHERNET_LIMIT 1522u
#define CHMLINK_QUEUE_PACKETS 256u
#define CHMLINK_IOCTL_RECEIVE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
#define CHMLINK_IOCTL_INJECT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)

/* Open \\.\ChmLink<interface-index> with read/write access and no sharing.
 * RECEIVE returns a protocol.h batch, or ERROR_NO_MORE_ITEMS (polling).
 * INJECT accepts a validated Ethernet batch. All requests are synchronous;
 * no pointer, event handle, or native structure crosses the device boundary.
 */

#endif
