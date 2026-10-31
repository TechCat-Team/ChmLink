#ifndef CHMLINK_OFFLOAD_H
#define CHMLINK_OFFLOAD_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Software fallbacks for packets that carry a chmlink_offload descriptor, for
 * applications that need plain, wire-ready packets. Portable and allocation
 * free. Packets are IP packets (CHMLINK_MODE_IP) or Ethernet frames whose
 * EtherType is IPv4 or IPv6 (CHMLINK_MODE_ETHERNET). */

/* One's complement sum of bytes, folded to 16 bits and not inverted; `seed`
 * continues an earlier sum. */
uint16_t chmlink_checksum_add(const uint8_t *data, size_t length, uint16_t seed);

/* Fill in the L4 checksum of a NEEDS_CSUM packet without segmentation, in
 * place: the pseudo-header sum plus the L4 bytes from csum_start, stored at
 * csum_start + csum_offset. Packets without NEEDS_CSUM are left unchanged. */
chmlink_result chmlink_offload_complete(chmlink_mode mode, uint8_t *packet, size_t length,
    const chmlink_offload *offload);

/* Called once per segment, in order. The segment is valid only during the
 * call. Return non-zero to continue. */
typedef int (*chmlink_segment_callback)(const uint8_t *segment, size_t length, void *context);

/* Cut a GSO TCP packet into wire packets of at most gso_size payload bytes,
 * each with its own IP length, IPv4 identification (incremented per segment,
 * kept within 0x0000-0x7fff), IPv4 header checksum, TCP sequence number and
 * full TCP checksum. FIN and PSH stay on the last segment only, CWR on the
 * first only. `scratch` must hold hdr_len + gso_size bytes. A packet without
 * a GSO type is passed through chmlink_offload_complete and delivered as one
 * segment (it is modified in place). */
chmlink_result chmlink_gso_segment(chmlink_mode mode, uint8_t *packet, size_t length,
    const chmlink_offload *offload, uint8_t *scratch, size_t scratch_size,
    chmlink_segment_callback callback, void *context);

#ifdef __cplusplus
}
#endif

#endif
