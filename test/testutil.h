/*
 * Helpers shared by the three test programs.
 *
 * Anything that more than one of unit.c, protocol.c and mockphone.c needs
 * lives here, so the framing rules and the DHCP server side exist in exactly
 * one place each rather than being reimplemented per test.
 */
#ifndef ET_TESTUTIL_H
#define ET_TESTUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../src/proto.h"

/* Parse a dotted quad into network byte order.  Invalid input yields 0. */
uint32_t tu_ip4(const char *s);

/* ---- the tether link over a socket ---- */

/* Encodes one record and writes it whole.  0 on success, -1 on error. */
int tu_send_frame(int fd, const uint8_t *frame, size_t len);

/*
 * A buffered reader for the record stream.  Records arrive split across TCP
 * reads, so every reader needs this; keeping the buffer in the struct is what
 * lets the framing walk live in one function.
 */
struct tu_stream {
	int      fd;
	uint8_t  buf[65536];
	size_t   have;
};

void tu_stream_init(struct tu_stream *s, int fd);

/*
 * Waits up to timeout_ms for one frame.  Returns its length, 0 on timeout, or
 * -1 if the peer desynchronised or went away.  Records too short to hold a
 * frame are skipped, as the protocol requires.
 */
ssize_t tu_recv_frame(struct tu_stream *s, uint8_t *out, int timeout_ms);

/*
 * Counts records that arrived with non-zero padding.  The vendor zeroes it, so
 * anything else is worth flagging; checking it here means both sides of the
 * test suite police each other.
 */
extern unsigned long tu_padding_violations;

/* As above, but discards frames that are not the requested ethertype. */
ssize_t tu_recv_type(struct tu_stream *s, uint8_t *out, uint16_t ethertype, int timeout_ms);

/* ---- the phone's half of DHCP ---- */

struct tu_lease_opts {
	uint32_t    yiaddr;        /* all addresses in network byte order */
	uint32_t    server;
	uint32_t    mask;
	uint32_t    router;
	uint32_t    dns[DHCP_MAX_DNS];
	int         ndns;
	uint32_t    lease_secs;
	uint16_t    mtu;           /* 0: omit option 26 */
	const char *domain;        /* NULL: omit option 15 */
};

/*
 * Builds a DHCPOFFER or DHCPACK the way the phone does: a complete
 * Ethernet+IPv4+UDP+BOOTP reply, broadcast to the client.  Returns its length.
 */
size_t tu_dhcp_reply(uint8_t *out, uint8_t msgtype, const uint8_t client_mac[ETH_ALEN],
                     uint32_t xid, const uint8_t server_mac[ETH_ALEN],
                     const struct tu_lease_opts *o);

/*
 * Inspects a frame from the client.  Returns 1 and fills msgtype/xid when it
 * is a DHCP request, 0 otherwise.
 */
int tu_peek_dhcp(const uint8_t *frame, size_t len, uint8_t *msgtype, uint32_t *xid,
                 uint32_t *dst_ip);

#endif /* ET_TESTUTIL_H */
