/*
 * mockphone -- stands in for an Android phone running EasyTether.
 *
 * It impersonates both halves of what the daemon talks to: the local ADB
 * server's request protocol, and then the EasyTether service on the other end
 * of the stream.  On the tether link it behaves the way the real thing is
 * documented to: hands out 192.168.117.2/24 over DHCP, is the gateway at
 * 192.168.117.1, answers ARP, and replies to pings aimed at the gateway.  (The
 * real app emulates that locally too -- ICMP to the wider internet is dropped,
 * which is a property of EasyTether, not of this mock.)
 *
 * This exercises every part of the daemon that does not need a phone: framing,
 * the hello, the DHCP exchange, ARP, interface configuration and a full round
 * trip.  It validates the daemon's output strictly and complains about
 * anything off-spec.
 *
 *     make mock
 *     ./test/mockphone 15037
 *     sudo ./easytether-bridge -p 15037 -D -R -v
 *     ping 192.168.117.1
 */
#include "testutil.h"

#include "../src/adb.h"
#include "../src/proto.h"
#include "../src/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define GW      "192.168.117.1"
#define CLIENT  "192.168.117.2"
#define NETMASK "255.255.255.0"

static const uint8_t gw_mac[ETH_ALEN] = { 0x02, 0xea, 0x51, 0x00, 0x00, 0x01 };

static int  problems;
static long frames_in, frames_out;

static void complain(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void complain(const char *fmt, ...)
{
	va_list ap;

	problems++;
	fputs("  !! ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
	va_list ap;

	fputs("  -> ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static void lease_opts(struct tu_lease_opts *o)
{
	memset(o, 0, sizeof *o);
	o->yiaddr     = tu_ip4(CLIENT);
	o->server     = tu_ip4(GW);
	o->router     = tu_ip4(GW);
	o->mask       = tu_ip4(NETMASK);
	o->dns[0]     = tu_ip4(GW);
	o->ndns       = 1;
	o->lease_secs = 3600;
}

static int send_frame(int fd, const uint8_t *frame, size_t len)
{
	frames_out++;
	return tu_send_frame(fd, frame, len);
}

/* ------------------------------------------------------- ADB server side */

static int adb_handshake(int fd)
{
	char lenbuf[5] = { 0 }, req[256];
	unsigned long n;

	for (int step = 0; step < 2; step++) {
		if (io_read_all(fd, lenbuf, 4) < 0)
			return -1;
		n = strtoul(lenbuf, NULL, 16);
		if (n >= sizeof req) {
			complain("absurd ADB request length %lu", n);
			return -1;
		}
		if (io_read_all(fd, req, n) < 0)
			return -1;
		req[n] = '\0';

		if (step == 0) {
			if (strncmp(req, "host:transport", 14) != 0)
				complain("expected a transport request, got '%s'", req);
			else
				note("ADB transport request: %s", req);
		} else {
			if (strcmp(req, ET_ADB_SERVICE) != 0)
				complain("expected '%s', got '%s'", ET_ADB_SERVICE, req);
			else
				note("ADB service request: %s", req);
		}
		if (io_write_all(fd, "OKAY", 4) < 0)
			return -1;
	}
	return 0;
}

/* -------------------------------------------------------------- the link */

/* Reply to an echo request aimed at the gateway, like the real app does. */
static int maybe_echo(int fd, const uint8_t *frame, size_t len, const uint8_t *client_mac)
{
	uint8_t out[ET_MAX_FRAME];
	uint8_t dg[ET_MTU];
	const uint8_t *ip = frame + ETH_HDR_LEN;
	size_t ihl, iplen;
	uint32_t gw = tu_ip4(GW), src, dst;
	uint16_t ck;

	if (len < ETH_HDR_LEN + 28 || eth_type(frame) != ETHERTYPE_IPV4)
		return 0;
	if ((ip[0] >> 4) != 4 || ip[9] != 1)
		return 0;

	ihl = (size_t)(ip[0] & 0x0f) * 4;
	iplen = get_be16(ip + 2);
	if (ihl < 20 || iplen > len - ETH_HDR_LEN || iplen < ihl + 8)
		return 0;

	memcpy(&src, ip + 12, 4);
	memcpy(&dst, ip + 16, 4);
	if (dst != gw || ip[ihl] != 8)          /* not an echo request for us */
		return 0;

	memcpy(dg, ip, iplen);
	memcpy(dg + 12, &dst, 4);
	memcpy(dg + 16, &src, 4);
	put_be16(dg + 10, 0);
	ck = ip_checksum(dg, ihl);
	put_be16(dg + 10, ck);

	dg[ihl] = 0;                            /* echo reply */
	put_be16(dg + ihl + 2, 0);
	ck = ip_checksum(dg + ihl, iplen - ihl);
	put_be16(dg + ihl + 2, ck);

	send_frame(fd, out, eth_build(out, client_mac, gw_mac, ETHERTYPE_IPV4, dg, iplen));
	return 1;
}

static void handle_arp(int fd, const uint8_t *frame, size_t len)
{
	struct arp_view a;
	uint8_t out[ET_MAX_FRAME];

	if (arp_parse(frame, len, &a) != 0) {
		complain("malformed ARP frame");
		return;
	}

	if (a.op == ARP_OP_REQUEST && a.target_ip == tu_ip4(GW)) {
		note("ARP who-has %s -> replying %s", fmt_ip(a.target_ip), fmt_mac(gw_mac));
		send_frame(fd, out, arp_build(out, ARP_OP_REPLY, gw_mac, tu_ip4(GW),
		                              a.sender_mac, a.sender_ip));
	} else if (a.op == ARP_OP_REQUEST && a.target_ip == a.sender_ip) {
		note("gratuitous ARP for %s", fmt_ip(a.sender_ip));
	}
}

static void handle_ipv4(int fd, const uint8_t *frame, size_t len,
                        const uint8_t *client_mac, int *pings)
{
	struct tu_lease_opts o;
	uint8_t out[ET_MAX_FRAME];
	uint8_t mt = 0;
	uint32_t xid = 0, dst = 0;

	if (!tu_peek_dhcp(frame, len, &mt, &xid, &dst)) {
		if (maybe_echo(fd, frame, len, client_mac) && ++*pings == 1)
			note("ping to the gateway answered");
		return;
	}

	/*
	 * A renewal is unicast to the server.  Addressing it to 0.0.0.0 -- which
	 * is what happens if the sender derives the destination from the
	 * server-identifier option it is required to omit -- produces a datagram
	 * no real phone would ever answer.
	 */
	if (dst == 0)
		complain("DHCP packet addressed to 0.0.0.0; nothing can answer it");

	lease_opts(&o);

	switch (mt) {
	case DHCP_DISCOVER:
		note("DHCP discover -> offering %s", CLIENT);
		send_frame(fd, out, tu_dhcp_reply(out, DHCP_OFFER, frame + 6, xid, gw_mac, &o));
		break;
	case DHCP_REQUEST:
		note("DHCP request -> ack %s gw %s", CLIENT, GW);
		send_frame(fd, out, tu_dhcp_reply(out, DHCP_ACK, frame + 6, xid, gw_mac, &o));
		break;
	default:
		note("DHCP message type %u", mt);
		break;
	}
}

static void serve(int fd)
{
	struct tu_stream st;
	uint8_t frame[ET_MAX_FRAME];
	uint8_t hello[4];
	uint32_t expect = ET_HELLO_MAGIC;
	uint8_t client_mac[ETH_ALEN];
	int have_client_mac = 0;
	int pings = 0;

	if (adb_handshake(fd) < 0) {
		complain("ADB handshake failed");
		return;
	}

	if (io_read_all(fd, hello, 4) < 0) {
		complain("no hello received");
		return;
	}
	if (memcmp(hello, &expect, 4) != 0)
		complain("hello was %02x %02x %02x %02x, expected 51 b7 04 00",
		         hello[0], hello[1], hello[2], hello[3]);
	else
		note("hello ok: 51 b7 04 00");

	tu_stream_init(&st, fd);

	for (;;) {
		ssize_t n = tu_recv_frame(&st, frame, 3600 * 1000);

		if (n < 0) {
			note("client closed the tunnel");
			break;
		}
		if (n == 0)
			continue;               /* idle, keep waiting */

		frames_in++;

		if (!have_client_mac) {
			memcpy(client_mac, frame + 6, ETH_ALEN);
			have_client_mac = 1;
			note("client MAC is %s", fmt_mac(client_mac));
		} else if (memcmp(client_mac, frame + 6, ETH_ALEN) != 0) {
			complain("source MAC changed to %s", fmt_mac(frame + 6));
		}

		switch (eth_type(frame)) {
		case ETHERTYPE_ARP:
			handle_arp(fd, frame, (size_t)n);
			break;
		case ETHERTYPE_IPV4:
			handle_ipv4(fd, frame, (size_t)n, client_mac, &pings);
			break;
		default:
			break;
		}
	}

	if (tu_padding_violations)
		complain("%lu record(s) arrived with non-zero padding", tu_padding_violations);

	printf("\n  frames in %ld, out %ld, pings answered %d\n", frames_in, frames_out, pings);
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 15037;
	struct sockaddr_in sa;
	int ls, one = 1;

	signal(SIGPIPE, SIG_IGN);

	ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("bind");
		return 1;
	}
	listen(ls, 4);

	printf("mock phone listening on 127.0.0.1:%d\n", port);
	printf("run:  sudo ./easytether-bridge -p %d -D -R -v\n\n", port);

	for (;;) {
		int fd = accept(ls, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			return 1;
		}
		printf("--- daemon connected ---\n");
		problems = 0;
		frames_in = frames_out = 0;
		tu_padding_violations = 0;
		serve(fd);
		close(fd);
		printf("--- session over: %d problem(s) ---\n\n", problems);
	}
}
