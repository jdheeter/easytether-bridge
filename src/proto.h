/*
 * The EasyTether wire protocol, plus the small amount of layer-2 the host has
 * to speak because that protocol carries raw Ethernet.
 *
 * Wire format (recovered from the vendor's easytether-usb / easytether-local
 * binaries, which agree byte for byte):
 *
 *   1. Immediately after the stream to `localabstract:easytetherx` opens, the
 *      host sends a four byte hello: 51 b7 04 00  (u32 little endian
 *      0x0004b751).
 *   2. Every packet after that, in both directions, is a record:
 *
 *          +--------+--------+---------------------------+---------+
 *          | len lo | len hi |  raw Ethernet frame       | padding |
 *          +--------+--------+---------------------------+---------+
 *
 *      len is little endian and counts only the frame.  The record occupies
 *      (len + 5) & ~3 bytes -- that is, 2 + len rounded up to a multiple of
 *      four -- so the padding is 0..3 zero bytes.  len must be < 0x5eb
 *      (1515); the vendor treats anything larger as a protocol error and
 *      tears the tunnel down.  Records with len <= 13 are too short to be an
 *      Ethernet frame and are skipped rather than delivered.
 *
 * The phone acts as an ordinary Ethernet peer on this link: it runs a DHCP
 * server and answers ARP.  The vendor's driver got that for free by handing
 * the frames to a real (virtual) NIC via IOUserEthernet.  We terminate layer 2
 * in this process instead, because IOUserEthernet is no longer available to
 * third-party code.
 */
#ifndef ET_PROTO_H
#define ET_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* ---- EasyTether framing ---- */

#define ET_HELLO_MAGIC   0x0004b751u
#define ET_MAX_FRAME     1514           /* 0x5ea: 1500 payload + 14 header  */
#define ET_FRAME_LIMIT   1515           /* 0x5eb: first rejected length     */
#define ET_MIN_FRAME     14             /* records <= 13 carry no frame     */
#define ET_MTU           1500

/* Bytes a record with a frame of len bytes occupies on the wire. */
static inline size_t et_record_size(size_t len) { return (len + 5) & ~(size_t)3; }

/* What a record at the head of a buffer turned out to be. */
enum et_record {
	ET_RECORD_INCOMPLETE = 0,  /* need more bytes before deciding      */
	ET_RECORD_FRAME,           /* a frame worth delivering             */
	ET_RECORD_SKIP,            /* len <= 13: no frame, step over it    */
	ET_RECORD_DESYNC,          /* len >= 1515: framing is lost         */
};

/*
 * Decodes the record at the head of buf without copying.  On FRAME or SKIP,
 * *record is how far to advance and, for FRAME, frame/frame_len point into buf.
 *
 * Every caller that walks this stream goes through here, so the length limit,
 * the skip threshold and the stride live in exactly one place.
 */
enum et_record et_record_peek(const uint8_t *buf, size_t have,
                              const uint8_t **frame, size_t *frame_len,
                              size_t *record);

/*
 * Writes one record into out, which must have room for et_record_size(len).
 * Returns the bytes written, or 0 if len is not a deliverable frame.
 */
size_t et_record_encode(uint8_t *out, const uint8_t *frame, size_t len);

/* ---- Ethernet ---- */

#define ETH_ALEN         6
#define ETH_HDR_LEN      14
#define ETH_MIN_PAYLOAD  46             /* pad short frames like a real NIC */
#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806
#define ETHERTYPE_IPV6   0x86dd

/* 255.255.255.255.  Being all-ones, its byte order does not matter. */
#define IPV4_BROADCAST   0xffffffffu

extern const uint8_t eth_broadcast[ETH_ALEN];

/*
 * Writes an Ethernet frame into out (which must hold ET_MAX_FRAME bytes) and
 * returns its length, padding the payload out to ETH_MIN_PAYLOAD.
 */
size_t eth_build(uint8_t *out, const uint8_t dst[ETH_ALEN], const uint8_t src[ETH_ALEN],
                 uint16_t ethertype, const void *payload, size_t plen);

/* ---- ARP ---- */

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_view {
	uint16_t op;
	uint8_t  sender_mac[ETH_ALEN];
	uint32_t sender_ip;    /* network byte order */
	uint8_t  target_mac[ETH_ALEN];
	uint32_t target_ip;    /* network byte order */
};

/* Parses the ARP payload of a frame.  Returns 0 on success. */
int arp_parse(const uint8_t *frame, size_t flen, struct arp_view *out);

/* Builds a complete Ethernet+ARP frame.  Returns its length. */
size_t arp_build(uint8_t *out, uint16_t op,
                 const uint8_t smac[ETH_ALEN], uint32_t sip,
                 const uint8_t tmac[ETH_ALEN], uint32_t tip);

/* ---- DHCP ---- */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_DECLINE  4
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

#define DHCP_MAX_DNS  4

struct dhcp_lease {
	uint32_t ip;                     /* all addresses in network byte order */
	uint32_t mask;
	uint32_t router;
	uint32_t server;
	uint32_t dns[DHCP_MAX_DNS];
	int      ndns;
	uint32_t lease_secs;             /* 0 == not supplied                   */
	uint16_t mtu;                    /* 0 == not supplied                   */
	char     domain[128];
};

/*
 * Builds a complete Ethernet+IPv4+UDP+DHCP frame.
 *
 * ciaddr/requested/server are network byte order; requested and server add
 * options 50 and 54 when non-zero.  When unicast_to is NULL the frame is
 * broadcast (both at layer 2 and to 255.255.255.255) and the broadcast flag is
 * set so the server does not need an ARP entry for us yet.
 */
size_t dhcp_build(uint8_t *out, uint8_t msgtype, const uint8_t mac[ETH_ALEN],
                  uint32_t xid, uint32_t ciaddr, uint32_t requested, uint32_t server,
                  const uint8_t *unicast_to, uint32_t unicast_dst_ip, const char *hostname);

/*
 * Inspects an Ethernet frame.  Returns the DHCP message type (>0) and fills
 * out when the frame is a DHCP reply addressed to us with a matching xid.
 * Returns 0 when it is not a DHCP reply for us, or -1 when it is malformed.
 */
int dhcp_parse_frame(const uint8_t *frame, size_t flen, const uint8_t mac[ETH_ALEN],
                     uint32_t xid, struct dhcp_lease *out);

/* ---- checksums ---- */

uint16_t ip_checksum(const void *data, size_t len);

/* UDP checksum over the pseudo-header plus the datagram; never returns 0. */
uint16_t udp_checksum(uint32_t src_be, uint32_t dst_be, const uint8_t *udp, size_t len);

/* ---- byte order ----
 *
 * Packet fields are read and written a byte at a time rather than through a
 * cast, so nothing here depends on the host's alignment rules or violates
 * strict aliasing.  Keep new parsing code to the same discipline.
 */

static inline void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static inline uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* The ethertype of a frame long enough to have one. */
static inline uint16_t eth_type(const uint8_t *frame) { return get_be16(frame + 12); }

#endif /* ET_PROTO_H */
