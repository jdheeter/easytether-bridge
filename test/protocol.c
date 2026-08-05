/*
 * End-to-end protocol test.
 *
 * Drives a complete conversation against test/mockphone -- ADB handshake,
 * hello, DHCP discover/offer/request/ack, ARP resolution, and an ICMP round
 * trip -- using the same src/adb.c and src/proto.c the daemon uses, but
 * against an independent implementation of the other side.
 *
 * This covers everything except utun and the interface configuration, so it
 * runs without root.
 *
 *     make protocol-test
 */
#include "testutil.h"

#include "../src/adb.h"
#include "../src/proto.h"
#include "../src/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GW  "192.168.117.1"
#define ME  "192.168.117.2"

static int failures;
static uint8_t mac[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static int fd;
static struct tu_stream st;

#define CHECK(cond, ...)                                            \
	do {                                                        \
		if (cond) {                                         \
			printf("  ok   ");                          \
		} else {                                            \
			failures++;                                 \
			printf("  FAIL ");                          \
		}                                                   \
		printf(__VA_ARGS__);                                \
		putchar('\n');                                      \
		fflush(stdout);                                     \
	} while (0)

static void test_dhcp(struct dhcp_lease *lease)
{
	uint8_t frame[ET_MAX_FRAME];
	uint32_t xid = 0xa5a5f00d;
	size_t n;
	int mt;

	n = dhcp_build(frame, DHCP_DISCOVER, mac, xid, 0, 0, 0, NULL, 0, "test");
	tu_send_frame(fd, frame, n);

	for (;;) {
		n = (size_t)tu_recv_type(&st, frame, ETHERTYPE_IPV4, 3000);
		if (!n) {
			CHECK(0, "no DHCP offer arrived");
			return;
		}
		mt = dhcp_parse_frame(frame, n, mac, xid, lease);
		if (mt > 0)
			break;
	}
	CHECK(mt == DHCP_OFFER, "offer received (type %d)", mt);
	CHECK(lease->ip == tu_ip4(ME), "offered %s", fmt_ip(lease->ip));

	n = dhcp_build(frame, DHCP_REQUEST, mac, xid, 0, lease->ip, lease->server,
	               NULL, 0, "test");
	tu_send_frame(fd, frame, n);

	for (;;) {
		n = (size_t)tu_recv_type(&st, frame, ETHERTYPE_IPV4, 3000);
		if (!n) {
			CHECK(0, "no DHCP ack arrived");
			return;
		}
		mt = dhcp_parse_frame(frame, n, mac, xid, lease);
		if (mt > 0)
			break;
	}
	CHECK(mt == DHCP_ACK, "ack received (type %d)", mt);
	CHECK(lease->ip == tu_ip4(ME), "leased %s", fmt_ip(lease->ip));
	CHECK(lease->router == tu_ip4(GW), "gateway %s", fmt_ip(lease->router));
	CHECK(lease->mask == tu_ip4("255.255.255.0"), "netmask %s", fmt_ip(lease->mask));
	CHECK(lease->ndns >= 1, "%d DNS server(s) offered", lease->ndns);
	/* The real phone hands out an infinite lease; the mock uses an hour. */
	CHECK(lease->lease_secs != 0, "lease %u seconds%s", lease->lease_secs,
	      lease->lease_secs == 0xffffffffu ? " (infinite)" : "");

	printf("       server %s", fmt_ip(lease->server));
	for (int i = 0; i < lease->ndns; i++)
		printf(", dns %s", fmt_ip(lease->dns[i]));
	if (lease->mtu)
		printf(", mtu %u", lease->mtu);
	if (lease->domain[0])
		printf(", domain %s", lease->domain);
	printf("\n");
}

static void test_arp(uint8_t gw_mac[ETH_ALEN])
{
	uint8_t frame[ET_MAX_FRAME];
	struct arp_view v;
	size_t n;

	n = arp_build(frame, ARP_OP_REQUEST, mac, tu_ip4(ME),
	              (uint8_t[ETH_ALEN]){ 0 }, tu_ip4(GW));
	tu_send_frame(fd, frame, n);

	n = (size_t)tu_recv_type(&st, frame, ETHERTYPE_ARP, 3000);
	if (!n) {
		CHECK(0, "no ARP reply arrived");
		return;
	}
	CHECK(arp_parse(frame, n, &v) == 0, "ARP reply parses");
	CHECK(v.op == ARP_OP_REPLY, "it is a reply (op %u)", v.op);
	CHECK(v.sender_ip == tu_ip4(GW), "from the gateway %s", fmt_ip(v.sender_ip));
	memcpy(gw_mac, v.sender_mac, ETH_ALEN);
	printf("       gateway is at %s\n", fmt_mac(gw_mac));
}

/* A real packet round trip: echo request out, echo reply back. */
static void test_icmp(const uint8_t gw_mac[ETH_ALEN])
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t dg[64];
	size_t n;
	uint32_t me = tu_ip4(ME), gw = tu_ip4(GW);
	uint16_t ck;

	memset(dg, 0, sizeof dg);
	dg[0] = 0x45;
	dg[2] = 0; dg[3] = 32;          /* 20 IP + 12 ICMP */
	dg[8] = 64;
	dg[9] = 1;                      /* ICMP */
	memcpy(dg + 12, &me, 4);
	memcpy(dg + 16, &gw, 4);
	ck = ip_checksum(dg, 20);
	dg[10] = (uint8_t)(ck >> 8);
	dg[11] = (uint8_t)ck;

	dg[20] = 8;                     /* echo request */
	dg[24] = 0xbe; dg[25] = 0xef;   /* identifier   */
	dg[26] = 0x00; dg[27] = 0x01;   /* sequence     */
	ck = ip_checksum(dg + 20, 12);
	dg[22] = (uint8_t)(ck >> 8);
	dg[23] = (uint8_t)ck;

	n = eth_build(frame, gw_mac, mac, ETHERTYPE_IPV4, dg, 32);
	tu_send_frame(fd, frame, n);

	for (int i = 0; i < 8; i++) {
		n = (size_t)tu_recv_type(&st, frame, ETHERTYPE_IPV4, 3000);
		if (!n)
			break;
		const uint8_t *ip = frame + ETH_HDR_LEN;
		if (ip[9] != 1)
			continue;
		size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
		CHECK(ip[ihl] == 0, "echo reply type is %u", ip[ihl]);
		CHECK(memcmp(ip + 12, &gw, 4) == 0, "reply is from %s", GW);
		CHECK(memcmp(ip + 16, &me, 4) == 0, "reply is addressed to %s", ME);
		CHECK(ip_checksum(ip, ihl) == 0, "reply IP checksum verifies");
		CHECK(ip[ihl + 4] == 0xbe && ip[ihl + 5] == 0xef,
		      "identifier came back intact");
		return;
	}
	CHECK(0, "no ICMP echo reply arrived");
}

/*
 * Renew the way the daemon does: ciaddr set, no requested-address and no
 * server-identifier option, unicast to the leasing server.  Getting the
 * destination wrong here is invisible until a lease actually expires.
 */
static void test_renew(const uint8_t gw_mac[ETH_ALEN], const struct dhcp_lease *lease)
{
	uint8_t frame[ET_MAX_FRAME];
	struct dhcp_lease got;
	uint32_t xid = 0x5eeded01;
	size_t n;
	int mt;

	n = dhcp_build(frame, DHCP_REQUEST, mac, xid, lease->ip, 0, 0,
	               gw_mac, lease->server, "test");
	CHECK(memcmp(frame, gw_mac, ETH_ALEN) == 0, "renewal is unicast to the gateway");
	{
		uint32_t dst;
		memcpy(&dst, frame + ETH_HDR_LEN + 16, 4);
		CHECK(dst == lease->server, "addressed to %s", fmt_ip(dst));
	}
	tu_send_frame(fd, frame, n);

	for (int i = 0; i < 8; i++) {
		n = (size_t)tu_recv_type(&st, frame, ETHERTYPE_IPV4, 3000);
		if (!n)
			break;
		mt = dhcp_parse_frame(frame, n, mac, xid, &got);
		if (mt <= 0)
			continue;
		CHECK(mt == DHCP_ACK, "renewal answered with type %d", mt);
		CHECK(got.ip == lease->ip, "still %s", fmt_ip(got.ip));
		return;
	}
	CHECK(0, "renewal went unanswered");
}

/*
 * Ask the phone's resolver to look up a name.  This is the only test here that
 * leaves the phone: it proves the app is really NATing UDP out to the network,
 * not just emulating the local link.
 */
static void test_dns(const uint8_t gw_mac[ETH_ALEN], const struct dhcp_lease *lease)
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t dg[256];
	static const uint8_t query[] = {
		0x5e, 0xa1,              /* id                            */
		0x01, 0x00,              /* standard query, recursion on  */
		0x00, 0x01,              /* one question                  */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0,
		0x00, 0x01,              /* A     */
		0x00, 0x01,              /* IN    */
	};
	size_t qlen = sizeof query;
	size_t udp_len = 8 + qlen;
	size_t total = 20 + udp_len;
	uint32_t me = tu_ip4(ME);
	uint32_t dns = lease->dns[0];
	uint16_t ck;
	size_t n;

	memset(dg, 0, sizeof dg);
	dg[0] = 0x45;
	dg[2] = (uint8_t)(total >> 8);
	dg[3] = (uint8_t)total;
	dg[8] = 64;
	dg[9] = 17;
	memcpy(dg + 12, &me, 4);
	memcpy(dg + 16, &dns, 4);
	ck = ip_checksum(dg, 20);
	dg[10] = (uint8_t)(ck >> 8);
	dg[11] = (uint8_t)ck;

	dg[20] = 0x9c; dg[21] = 0x40;      /* source port 40000 */
	dg[22] = 0x00; dg[23] = 0x35;      /* destination port 53 */
	dg[24] = (uint8_t)(udp_len >> 8);
	dg[25] = (uint8_t)udp_len;
	memcpy(dg + 28, query, qlen);
	ck = udp_checksum(me, dns, dg + 20, udp_len);
	dg[26] = (uint8_t)(ck >> 8);
	dg[27] = (uint8_t)ck;

	printf("       querying %s for example.com\n", fmt_ip(dns));
	n = eth_build(frame, gw_mac, mac, ETHERTYPE_IPV4, dg, total);
	tu_send_frame(fd, frame, n);

	for (int i = 0; i < 16; i++) {
		n = (size_t)tu_recv_type(&st, frame, ETHERTYPE_IPV4, 5000);
		if (!n)
			break;

		const uint8_t *ip = frame + ETH_HDR_LEN;
		if (ip[9] != 17)
			continue;
		size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
		const uint8_t *udp = ip + ihl;
		if (((udp[0] << 8) | udp[1]) != 53 || ((udp[2] << 8) | udp[3]) != 40000)
			continue;

		const uint8_t *d = udp + 8;
		int ancount = (d[6] << 8) | d[7];
		int rcode = d[3] & 0x0f;

		CHECK(d[0] == 0x5e && d[1] == 0xa1, "response matches our query id");
		CHECK((d[2] & 0x80) != 0, "it is flagged as a response");
		CHECK(rcode == 0, "rcode %d", rcode);
		CHECK(ancount > 0, "%d answer record(s) -- the phone reached the internet",
		      ancount);
		return;
	}
	CHECK(0, "no DNS response came back through the tunnel");
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 15037;
	char err[512] = { 0 };
	uint32_t hello = ET_HELLO_MAGIC;
	struct dhcp_lease lease;
	uint8_t gw_mac[ETH_ALEN] = { 0 };

	signal(SIGPIPE, SIG_IGN);
	log_set_level(ET_LOG_ERR);

	printf("connecting through the ADB service request\n");
	fd = adb_open_stream(port, NULL, err, sizeof err);
	if (fd < 0) {
		printf("  FAIL %s\n", err);
		return 1;
	}
	CHECK(1, "ADB handshake accepted");
	tu_stream_init(&st, fd);

	if (write(fd, &hello, 4) != 4) {
		printf("  FAIL could not send the hello\n");
		return 1;
	}
	CHECK(1, "hello sent (51 b7 04 00)");

	printf("DHCP\n");
	memset(&lease, 0, sizeof lease);
	test_dhcp(&lease);

	printf("ARP\n");
	test_arp(gw_mac);

	printf("DHCP renewal\n");
	if (gw_mac[0] || gw_mac[1])
		test_renew(gw_mac, &lease);
	else
		CHECK(0, "skipped: never learned the gateway MAC");

	printf("ICMP round trip\n");
	if (gw_mac[0] || gw_mac[1])
		test_icmp(gw_mac);
	else
		CHECK(0, "skipped: never learned the gateway MAC");

	if (argc > 2 && strcmp(argv[2], "--dns") == 0) {
		printf("DNS through the phone\n");
		if (lease.ndns > 0 && (gw_mac[0] || gw_mac[1]))
			test_dns(gw_mac, &lease);
		else
			CHECK(0, "skipped: no resolver or no gateway MAC");
	}

	close(fd);

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nprotocol conversation completed cleanly\n");
	return 0;
}
