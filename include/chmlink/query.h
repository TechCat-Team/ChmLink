#ifndef CHMLINK_QUERY_H
#define CHMLINK_QUERY_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"
#include "version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire layout of the 128-byte QUERY response. All fields are explicitly
 * little-endian; native structures never cross the device boundary. Both the
 * driver (encode) and the SDK (decode) use these definitions exclusively.
 * Gaps between fields are reserved and must be zero when encoded.
 */
#define CHMLINK_INFO_SIZE 128u

#define CHMLINK_INFO_VERSION          0u   /* uint32 */
#define CHMLINK_INFO_LENGTH           4u   /* uint32, always CHMLINK_INFO_SIZE */
#define CHMLINK_INFO_CAPABILITIES     8u   /* uint32 */
#define CHMLINK_INFO_STATE           12u   /* uint32 */
#define CHMLINK_INFO_MODE            16u   /* uint32 */
#define CHMLINK_INFO_INTERFACE_INDEX 20u   /* uint32 */
#define CHMLINK_INFO_MTU             24u   /* uint32 */
#define CHMLINK_INFO_PACKET_LIMIT    28u   /* uint32 */
#define CHMLINK_INFO_QUEUE_CAPACITY  32u   /* uint32 */
#define CHMLINK_INFO_QUEUED_PACKETS  36u   /* uint32 */
#define CHMLINK_INFO_PACKET_FILTER   40u   /* uint32; 44..47 reserved */
#define CHMLINK_INFO_MAC             48u   /* uint8[6]; 54..55 reserved */
#define CHMLINK_INFO_MAC_SIZE         6u
#define CHMLINK_INFO_LUID            56u   /* uint64; 64..71 reserved */
#define CHMLINK_INFO_TX_PACKETS      72u   /* uint64 counters from here on */
#define CHMLINK_INFO_TX_BYTES        80u
#define CHMLINK_INFO_TX_ERRORS       88u
#define CHMLINK_INFO_RX_PACKETS      96u
#define CHMLINK_INFO_RX_BYTES       104u
#define CHMLINK_INFO_RX_DISCARDS    112u
#define CHMLINK_INFO_TX_QUEUE_DROPS 120u

/* Decoded QUERY fields, independent of any Windows type. */
typedef struct chmlink_query_info {
    uint32_t version, capabilities, state, mode;
    uint32_t interface_index, mtu, packet_limit, queue_capacity, queued_packets;
    uint32_t packet_filter;
    uint8_t mac[CHMLINK_INFO_MAC_SIZE];
    uint64_t luid;
    uint64_t tx_packets, tx_bytes, tx_errors;
    uint64_t rx_packets, rx_bytes, rx_discards, tx_queue_drops;
} chmlink_query_info;

/* Writes exactly CHMLINK_INFO_SIZE bytes, including zeroed reserved fields.
 * The version and length fields are always the current contract values. */
void chmlink_query_encode(uint8_t *out, const chmlink_query_info *info);

/* Decode and validate a response. Returns CHMLINK_UNSUPPORTED_VERSION for a
 * wrong length/version (an incompatible driver), CHMLINK_INVALID_QUERY for an
 * internally inconsistent response, and leaves out untouched on failure. */
chmlink_result chmlink_query_decode(chmlink_query_info *out,
                                    const void *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
