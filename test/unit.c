/*
 * Unit tests for the parts of the protocol that are easy to get subtly wrong:
 * the record size arithmetic, checksums, ARP, and both directions of DHCP.
 *
 *     make check
 */
#include "testutil.h"

#include "../src/proto.h"
#include "../src/util.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                    \
	do {                                                                \
		if (!(cond)) {                                              \
			failures++;                                         \
			printf("  FAIL %s:%d  ", __func__, __LINE__);       \
			printf(__VA_ARGS__);                                \
			putchar('\n');                                      \
		}                                                           \
	} while (0)

/*
 * The vendor advances its buffer by (len + 5) & ~3, which is 2 + len rounded
 * up to a multiple of four.  Every value here was read back off the
 * disassembly of easytether-local and easytether-usb.
 */
static void test_record_size(void)
{
	struct { size_t len, want; } cases[] = {
		{ 0,    4    }, { 1,    4    }, { 2,    4    }, { 3,    8    },
		{ 14,   16   }, { 42,   44   }, { 46,   48   }, { 60,   64   },
		{ 1500, 1504 }, { 1513, 1516 }, { 1514, 1516 },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		size_t got = et_record_size(cases[i].len);
		CHECK(got == cases[i].want, "len %zu -> %zu, want %zu",
		      cases[i].len, got, cases[i].want);
		CHECK(got >= cases[i].len + 2, "record %zu cannot hold len %zu",
		      got, cases[i].len);
		CHECK(got % 4 == 0, "record %zu is not 4-byte aligned", got);
		CHECK(got - cases[i].len - 2 < 4, "padding for len %zu is %zu bytes",
		      cases[i].len, got - cases[i].len - 2);
	}
}

/* The worked example from the IPv4 header checksum literature. */
static void test_checksum(void)
{
	uint8_t hdr[20] = {
		0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00,
		0x40, 0x06, 0x00, 0x00, 0xac, 0x10, 0x0a, 0x63,
		0xac, 0x10, 0x0a, 0x0c
	};
	uint16_t ck = ip_checksum(hdr, sizeof hdr);

	CHECK(ck == 0xb1e6, "checksum 0x%04x, want 0xb1e6", ck);

	/* With the checksum in place the sum over the whole header is zero. */
	hdr[10] = (uint8_t)(ck >> 8);
	hdr[11] = (uint8_t)ck;
	CHECK(ip_checksum(hdr, sizeof hdr) == 0, "verify pass did not come out zero");
}

static void test_eth_build(void)
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t src[ETH_ALEN] = { 0x02, 1, 2, 3, 4, 5 };
	uint8_t dst[ETH_ALEN] = { 0x02, 9, 8, 7, 6, 5 };
	uint8_t payload[4] = { 0xde, 0xad, 0xbe, 0xef };
	size_t n;

	n = eth_build(frame, dst, src, ETHERTYPE_IPV4, payload, sizeof payload);
	CHECK(n == 60, "short frame padded to %zu, want 60", n);
	CHECK(memcmp(frame, dst, ETH_ALEN) == 0, "destination MAC wrong");
	CHECK(memcmp(frame + 6, src, ETH_ALEN) == 0, "source MAC wrong");
	CHECK(frame[12] == 0x08 && frame[13] == 0x00, "ethertype wrong");
	CHECK(memcmp(frame + 14, payload, 4) == 0, "payload wrong");
	for (size_t i = 18; i < 60; i++)
		CHECK(frame[i] == 0, "pad byte %zu is 0x%02x", i, frame[i]);

	uint8_t big[ET_MTU];
	memset(big, 0xa5, sizeof big);
	n = eth_build(frame, dst, src, ETHERTYPE_IPV4, big, sizeof big);
	CHECK(n == ET_MAX_FRAME, "full frame is %zu, want %d", n, ET_MAX_FRAME);
}

static void test_arp_roundtrip(void)
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t smac[ETH_ALEN] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint8_t tmac[ETH_ALEN] = { 0x02, 0xea, 0x51, 0, 0, 1 };
	struct arp_view v;
	size_t n;

	n = arp_build(frame, ARP_OP_REQUEST, smac, tu_ip4("192.168.117.2"),
	              (uint8_t[ETH_ALEN]){ 0 }, tu_ip4("192.168.117.1"));
	CHECK(n == 60, "ARP request is %zu bytes, want 60", n);
	CHECK(memcmp(frame, eth_broadcast, ETH_ALEN) == 0, "request not broadcast");
	CHECK(arp_parse(frame, n, &v) == 0, "cannot parse our own request");
	CHECK(v.op == ARP_OP_REQUEST, "op is %u", v.op);
	CHECK(v.sender_ip == tu_ip4("192.168.117.2"), "sender ip %s", fmt_ip(v.sender_ip));
	CHECK(v.target_ip == tu_ip4("192.168.117.1"), "target ip %s", fmt_ip(v.target_ip));
	CHECK(memcmp(v.sender_mac, smac, ETH_ALEN) == 0, "sender mac wrong");

	n = arp_build(frame, ARP_OP_REPLY, tmac, tu_ip4("192.168.117.1"),
	              smac, tu_ip4("192.168.117.2"));
	CHECK(memcmp(frame, smac, ETH_ALEN) == 0, "reply not unicast to requester");
	CHECK(arp_parse(frame, n, &v) == 0, "cannot parse our own reply");
	CHECK(v.op == ARP_OP_REPLY, "reply op is %u", v.op);

	/* Truncated frames must be rejected rather than read past the end. */
	CHECK(arp_parse(frame, ETH_HDR_LEN + 10, &v) != 0, "accepted a truncated ARP");
}

/* Walk the frame dhcp_build produced and verify every layer of it. */
static void test_dhcp_build(void)
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t mac[ETH_ALEN] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint32_t xid = 0x12345678;
	size_t n = dhcp_build(frame, DHCP_DISCOVER, mac, xid, 0, 0, 0, NULL, 0, "mac");
	const uint8_t *ip = frame + ETH_HDR_LEN;
	const uint8_t *udp, *dhcp;
	size_t ihl, iplen;
	int seen_msgtype = 0, seen_clientid = 0;

	CHECK(n > 300 && n <= ET_MAX_FRAME, "discover frame is %zu bytes", n);
	CHECK(memcmp(frame, eth_broadcast, ETH_ALEN) == 0, "discover not broadcast");
	CHECK(memcmp(frame + 6, mac, ETH_ALEN) == 0, "source mac wrong");
	CHECK(frame[12] == 0x08 && frame[13] == 0x00, "ethertype not IPv4");

	CHECK((ip[0] >> 4) == 4, "not IPv4");
	ihl = (size_t)(ip[0] & 0x0f) * 4;
	iplen = (size_t)((ip[2] << 8) | ip[3]);
	CHECK(ihl == 20, "unexpected IHL %zu", ihl);
	CHECK(ETH_HDR_LEN + iplen <= n, "IP length %zu overruns the frame", iplen);
	CHECK(ip_checksum(ip, ihl) == 0, "IP header checksum does not verify");
	CHECK(ip[9] == 17, "protocol is %u, want 17", ip[9]);
	CHECK(memcmp(ip + 16, &(uint32_t){ htonl(INADDR_BROADCAST) }, 4) == 0,
	      "destination is not 255.255.255.255");

	udp = ip + ihl;
	CHECK(((udp[0] << 8) | udp[1]) == 68, "source port %u", (udp[0] << 8) | udp[1]);
	CHECK(((udp[2] << 8) | udp[3]) == 67, "dest port %u", (udp[2] << 8) | udp[3]);
	CHECK((size_t)((udp[4] << 8) | udp[5]) == iplen - ihl,
	      "UDP length disagrees with IP");

	dhcp = udp + 8;
	CHECK(dhcp[0] == 1, "op is %u, want BOOTREQUEST", dhcp[0]);
	CHECK(dhcp[1] == 1 && dhcp[2] == ETH_ALEN, "htype/hlen wrong");
	CHECK(memcmp(dhcp + 4, &xid, 4) == 0, "xid not echoed into the packet");
	CHECK(((dhcp[10] << 8) | dhcp[11]) == 0x8000, "broadcast flag not set");
	CHECK(memcmp(dhcp + 28, mac, ETH_ALEN) == 0, "chaddr wrong");
	CHECK(ntohl(*(const uint32_t *)(const void *)(dhcp + 236)) == 0x63825363u,
	      "magic cookie wrong");

	const uint8_t *opt = dhcp + 240;
	const uint8_t *end = dhcp + (((udp[4] << 8) | udp[5]) - 8);
	while (opt < end) {
		uint8_t code = *opt++;
		if (code == 0)
			continue;
		if (code == 255)
			break;
		CHECK(opt < end, "option %u truncated", code);
		uint8_t l = *opt++;
		CHECK(opt + l <= end, "option %u overruns", code);
		if (code == 53) {
			seen_msgtype = 1;
			CHECK(l == 1 && opt[0] == DHCP_DISCOVER, "wrong message type");
		}
		if (code == 61) {
			seen_clientid = 1;
			CHECK(l == 7 && opt[0] == 1 && memcmp(opt + 1, mac, ETH_ALEN) == 0,
			      "client identifier wrong");
		}
		opt += l;
	}
	CHECK(seen_msgtype, "no message type option");
	CHECK(seen_clientid, "no client identifier option");
}

static void test_dhcp_parse(void)
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t mac[ETH_ALEN] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint8_t other[ETH_ALEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	uint32_t xid = 0xcafebabe;
	uint8_t gwmac[ETH_ALEN] = { 0x02, 0xea, 0x51, 0, 0, 1 };
	struct dhcp_lease l;
	struct tu_lease_opts o;
	size_t n;
	int mt;

	/* A lease with every option we claim to understand, built the way the
	 * phone builds it, so encode and decode are checked against each other. */
	memset(&o, 0, sizeof o);
	o.yiaddr     = tu_ip4("192.168.117.2");
	o.server     = tu_ip4("192.168.117.1");
	o.router     = tu_ip4("192.168.117.1");
	o.mask       = tu_ip4("255.255.255.0");
	o.dns[0]     = tu_ip4("192.168.117.1");
	o.dns[1]     = tu_ip4("8.8.8.8");
	o.ndns       = 2;
	o.lease_secs = 3600;
	o.mtu        = 1448;
	o.domain     = "local";

	n = tu_dhcp_reply(frame, DHCP_ACK, mac, xid, gwmac, &o);

	mt = dhcp_parse_frame(frame, n, mac, xid, &l);
	CHECK(mt == DHCP_ACK, "message type %d, want ACK", mt);
	CHECK(l.ip == tu_ip4("192.168.117.2"), "ip %s", fmt_ip(l.ip));
	CHECK(l.mask == tu_ip4("255.255.255.0"), "mask %s", fmt_ip(l.mask));
	CHECK(l.router == tu_ip4("192.168.117.1"), "router %s", fmt_ip(l.router));
	CHECK(l.server == tu_ip4("192.168.117.1"), "server %s", fmt_ip(l.server));
	CHECK(l.ndns == 2, "%d DNS servers, want 2", l.ndns);
	CHECK(l.dns[0] == tu_ip4("192.168.117.1"), "dns0 %s", fmt_ip(l.dns[0]));
	CHECK(l.dns[1] == tu_ip4("8.8.8.8"), "dns1 %s", fmt_ip(l.dns[1]));
	CHECK(l.lease_secs == 3600, "lease %u", l.lease_secs);
	CHECK(l.mtu == 1448, "mtu %u", l.mtu);
	CHECK(strcmp(l.domain, "local") == 0, "domain '%s'", l.domain);

	/* A reply for a different transaction or a different host is not ours. */
	CHECK(dhcp_parse_frame(frame, n, mac, xid ^ 1, &l) == 0, "accepted a foreign xid");
	CHECK(dhcp_parse_frame(frame, n, other, xid, &l) == 0, "accepted a foreign chaddr");

	/* Truncation must not be mistaken for a valid packet. */
	for (size_t cut = 1; cut < n; cut += 7)
		dhcp_parse_frame(frame, n - cut, mac, xid, &l);   /* must not crash */

	/* A non-DHCP IPv4 frame must be ignored, not misparsed. */
	uint8_t plain[ET_MAX_FRAME];
	uint8_t body[64];
	memset(body, 0, sizeof body);
	body[0] = 0x45;
	body[2] = 0; body[3] = 64;
	body[9] = 6;                                          /* TCP */
	size_t pn = eth_build(plain, mac, other, ETHERTYPE_IPV4, body, sizeof body);
	CHECK(dhcp_parse_frame(plain, pn, mac, xid, &l) == 0, "TCP frame parsed as DHCP");
}

/*
 * A RENEWING request must go unicast to the leasing server with ciaddr set,
 * no requested-address and no server-identifier option (RFC 2131 4.3.2).  The
 * destination address and the option-54 value are separate things; conflating
 * them addresses the datagram to 0.0.0.0, where it is silently dropped and
 * renewal can never succeed.
 */
static void test_dhcp_renew(void)
{
	uint8_t frame[ET_MAX_FRAME];
	uint8_t mac[ETH_ALEN] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint8_t gw_mac[ETH_ALEN] = { 0x02, 0xea, 0x51, 0, 0, 1 };
	uint32_t me = tu_ip4("192.168.117.2"), server = tu_ip4("192.168.117.1");
	size_t n = dhcp_build(frame, DHCP_REQUEST, mac, 0x99887766, me, 0, 0,
	                      gw_mac, server, "test");
	const uint8_t *ip = frame + ETH_HDR_LEN;
	const uint8_t *udp, *dhcp, *opt, *end;
	size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
	int saw_50 = 0, saw_54 = 0;
	uint32_t dst;

	CHECK(n > 300 && n <= ET_MAX_FRAME, "renewal frame is %zu bytes", n);
	CHECK(memcmp(frame, gw_mac, ETH_ALEN) == 0, "renewal is unicast to the gateway");

	memcpy(&dst, ip + 16, 4);
	CHECK(dst == server, "addressed to %s, want the server", fmt_ip(dst));
	CHECK(ip_checksum(ip, ihl) == 0, "IP checksum verifies");

	udp = ip + ihl;
	dhcp = udp + 8;
	CHECK(memcmp(dhcp + 12, &me, 4) == 0, "ciaddr is our address");
	CHECK(((dhcp[10] << 8) | dhcp[11]) == 0, "broadcast flag is clear when renewing");

	opt = dhcp + 240;
	end = dhcp + (((udp[4] << 8) | udp[5]) - 8);
	while (opt < end) {
		uint8_t code = *opt++;
		if (code == 0)
			continue;
		if (code == 255)
			break;
		if (opt >= end)
			break;
		uint8_t l = *opt++;
		if (opt + l > end)
			break;
		if (code == 50)
			saw_50 = 1;
		if (code == 54)
			saw_54 = 1;
		opt += l;
	}
	CHECK(!saw_50, "no requested-address option when renewing");
	CHECK(!saw_54, "no server-identifier option when renewing");
}

int main(void)
{
	printf("record size arithmetic\n");   test_record_size();
	printf("checksums\n");                test_checksum();
	printf("ethernet framing\n");         test_eth_build();
	printf("ARP\n");                      test_arp_roundtrip();
	printf("DHCP client output\n");       test_dhcp_build();
	printf("DHCP reply parsing\n");       test_dhcp_parse();
	printf("DHCP renewal\n");            test_dhcp_renew();

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
