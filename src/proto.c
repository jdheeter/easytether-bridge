#include "proto.h"
#include "util.h"

#include <arpa/inet.h>
#include <string.h>

const uint8_t eth_broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ------------------------------------------------------------------ misc */

uint16_t ip_checksum(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (uint32_t)(p[0] << 8);

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)~sum;
}

/* ---------------------------------------------------------------- records */

enum et_record et_record_peek(const uint8_t *buf, size_t have,
                              const uint8_t **frame, size_t *frame_len,
                              size_t *record)
{
	size_t len, size;

	if (have < 2)
		return ET_RECORD_INCOMPLETE;

	len = get_le16(buf);
	if (len >= ET_FRAME_LIMIT)
		return ET_RECORD_DESYNC;

	size = et_record_size(len);
	if (have < size)
		return ET_RECORD_INCOMPLETE;

	*record = size;
	if (len < ET_MIN_FRAME)
		return ET_RECORD_SKIP;

	*frame = buf + 2;
	*frame_len = len;
	return ET_RECORD_FRAME;
}

size_t et_record_encode(uint8_t *out, const uint8_t *frame, size_t len)
{
	size_t size;

	if (len < ET_MIN_FRAME || len > ET_MAX_FRAME)
		return 0;

	size = et_record_size(len);
	put_le16(out, (uint16_t)len);
	memcpy(out + 2, frame, len);
	if (size > len + 2)
		memset(out + 2 + len, 0, size - len - 2);

	return size;
}

/* -------------------------------------------------------------- ethernet */

size_t eth_build(uint8_t *out, const uint8_t dst[ETH_ALEN], const uint8_t src[ETH_ALEN],
                 uint16_t ethertype, const void *payload, size_t plen)
{
	size_t pad = 0;

	if (plen > ET_MTU)
		plen = ET_MTU;
	if (plen < ETH_MIN_PAYLOAD)
		pad = ETH_MIN_PAYLOAD - plen;

	memcpy(out, dst, ETH_ALEN);
	memcpy(out + ETH_ALEN, src, ETH_ALEN);
	put_be16(out + 12, ethertype);
	memcpy(out + ETH_HDR_LEN, payload, plen);
	if (pad)
		memset(out + ETH_HDR_LEN + plen, 0, pad);

	return ETH_HDR_LEN + plen + pad;
}

/* ------------------------------------------------------------------- ARP */

/* ARP payload layout for IPv4 over Ethernet, 28 bytes. */
#define ARP_PAYLOAD_LEN 28

int arp_parse(const uint8_t *frame, size_t flen, struct arp_view *out)
{
	const uint8_t *a;

	if (flen < ETH_HDR_LEN + ARP_PAYLOAD_LEN)
		return -1;

	a = frame + ETH_HDR_LEN;
	if (get_be16(a) != 1)              /* hardware type: Ethernet */
		return -1;
	if (get_be16(a + 2) != ETHERTYPE_IPV4)
		return -1;
	if (a[4] != ETH_ALEN || a[5] != 4)
		return -1;

	out->op = get_be16(a + 6);
	memcpy(out->sender_mac, a + 8, ETH_ALEN);
	memcpy(&out->sender_ip, a + 14, 4);
	memcpy(out->target_mac, a + 18, ETH_ALEN);
	memcpy(&out->target_ip, a + 24, 4);
	return 0;
}

size_t arp_build(uint8_t *out, uint16_t op,
                 const uint8_t smac[ETH_ALEN], uint32_t sip,
                 const uint8_t tmac[ETH_ALEN], uint32_t tip)
{
	uint8_t a[ARP_PAYLOAD_LEN];

	put_be16(a, 1);
	put_be16(a + 2, ETHERTYPE_IPV4);
	a[4] = ETH_ALEN;
	a[5] = 4;
	put_be16(a + 6, op);
	memcpy(a + 8, smac, ETH_ALEN);
	memcpy(a + 14, &sip, 4);
	memcpy(a + 18, tmac, ETH_ALEN);
	memcpy(a + 24, &tip, 4);

	return eth_build(out, op == ARP_OP_REQUEST ? eth_broadcast : tmac,
	                 smac, ETHERTYPE_ARP, a, sizeof a);
}

/* ------------------------------------------------------------------ DHCP */

#define BOOTP_LEN        236           /* fixed part, before the magic cookie */
#define DHCP_COOKIE      0x63825363u
#define DHCP_MIN_PAYLOAD 300           /* pad so ancient servers stay happy   */

#define UDP_PORT_SERVER 67
#define UDP_PORT_CLIENT 68

struct ipv4_hdr {
	uint8_t  ver_ihl;
	uint8_t  tos;
	uint16_t total_len;
	uint16_t id;
	uint16_t frag;
	uint8_t  ttl;
	uint8_t  proto;
	uint16_t check;
	uint32_t src;
	uint32_t dst;
};

struct udp_hdr {
	uint16_t sport;
	uint16_t dport;
	uint16_t len;
	uint16_t check;
};

uint16_t udp_checksum(uint32_t src, uint32_t dst, const uint8_t *udp, size_t len)
{
	uint32_t sum = 0;
	size_t i;

	sum += (ntohl(src) >> 16) & 0xffff;
	sum += ntohl(src) & 0xffff;
	sum += (ntohl(dst) >> 16) & 0xffff;
	sum += ntohl(dst) & 0xffff;
	sum += 17;                      /* IPPROTO_UDP */
	sum += (uint32_t)len;

	for (i = 0; i + 1 < len; i += 2)
		sum += (uint32_t)((udp[i] << 8) | udp[i + 1]);
	if (i < len)
		sum += (uint32_t)(udp[i] << 8);

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	uint16_t c = (uint16_t)~sum;
	return c ? c : 0xffff;          /* 0 means "no checksum" on the wire */
}

size_t dhcp_build(uint8_t *out, uint8_t msgtype, const uint8_t mac[ETH_ALEN],
                  uint32_t xid, uint32_t ciaddr, uint32_t requested, uint32_t server,
                  const uint8_t *unicast_to, uint32_t unicast_dst_ip, const char *hostname)
{
	uint8_t datagram[ET_MTU];
	uint8_t *dhcp = datagram + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);
	uint8_t *o;
	size_t dhcp_len, udp_len, total;
	struct ipv4_hdr ip;
	struct udp_hdr udp;
	uint32_t dst_ip = unicast_to ? unicast_dst_ip : htonl(INADDR_BROADCAST);
	uint32_t src_ip = ciaddr;

	memset(datagram, 0, sizeof datagram);

	dhcp[0] = 1;                    /* op: BOOTREQUEST */
	dhcp[1] = 1;                    /* htype: Ethernet */
	dhcp[2] = ETH_ALEN;             /* hlen */
	dhcp[3] = 0;                    /* hops */
	memcpy(dhcp + 4, &xid, 4);
	put_be16(dhcp + 8, 0);          /* secs */
	/*
	 * Ask for a broadcast reply while we have no address configured: the
	 * server would otherwise have to ARP for an address we cannot yet
	 * answer for.  Once we are renewing from a configured address we can
	 * take the reply unicast.
	 */
	put_be16(dhcp + 10, ciaddr ? 0x0000 : 0x8000);
	memcpy(dhcp + 12, &ciaddr, 4);  /* ciaddr */
	memcpy(dhcp + 28, mac, ETH_ALEN);
	memcpy(dhcp + BOOTP_LEN, &(uint32_t){ htonl(DHCP_COOKIE) }, 4);

	o = dhcp + BOOTP_LEN + 4;
	*o++ = 53; *o++ = 1; *o++ = msgtype;

	*o++ = 61; *o++ = 1 + ETH_ALEN; *o++ = 1;      /* client identifier */
	memcpy(o, mac, ETH_ALEN);
	o += ETH_ALEN;

	if (requested) {
		*o++ = 50; *o++ = 4;
		memcpy(o, &requested, 4);
		o += 4;
	}
	if (server) {
		*o++ = 54; *o++ = 4;
		memcpy(o, &server, 4);
		o += 4;
	}
	if (hostname && *hostname) {
		size_t hl = strlen(hostname);
		if (hl > 63)
			hl = 63;
		*o++ = 12; *o++ = (uint8_t)hl;
		memcpy(o, hostname, hl);
		o += hl;
	}

	*o++ = 55; *o++ = 7;            /* parameter request list */
	*o++ = 1;                       /* subnet mask     */
	*o++ = 3;                       /* router          */
	*o++ = 6;                       /* domain servers  */
	*o++ = 15;                      /* domain name     */
	*o++ = 26;                      /* interface MTU   */
	*o++ = 28;                      /* broadcast addr  */
	*o++ = 51;                      /* lease time      */

	*o++ = 57; *o++ = 2;            /* max message size */
	put_be16(o, ET_MTU - 68);
	o += 2;

	*o++ = 255;                     /* end */

	dhcp_len = (size_t)(o - dhcp);
	if (dhcp_len < DHCP_MIN_PAYLOAD)
		dhcp_len = DHCP_MIN_PAYLOAD;

	udp_len = sizeof(struct udp_hdr) + dhcp_len;
	total = sizeof(struct ipv4_hdr) + udp_len;

	memset(&udp, 0, sizeof udp);
	udp.sport = htons(UDP_PORT_CLIENT);
	udp.dport = htons(UDP_PORT_SERVER);
	udp.len = htons((uint16_t)udp_len);
	memcpy(datagram + sizeof(struct ipv4_hdr), &udp, sizeof udp);

	udp.check = htons(udp_checksum(src_ip, dst_ip,
	                               datagram + sizeof(struct ipv4_hdr), udp_len));
	memcpy(datagram + sizeof(struct ipv4_hdr), &udp, sizeof udp);

	memset(&ip, 0, sizeof ip);
	ip.ver_ihl = 0x45;
	ip.tos = 0x10;                  /* IPTOS_LOWDELAY, as dhclient sends */
	ip.total_len = htons((uint16_t)total);
	ip.id = 0;
	ip.frag = 0;
	ip.ttl = 64;
	ip.proto = 17;
	ip.src = src_ip;
	ip.dst = dst_ip;
	memcpy(datagram, &ip, sizeof ip);
	ip.check = htons(ip_checksum(datagram, sizeof ip));
	memcpy(datagram, &ip, sizeof ip);

	return eth_build(out, unicast_to ? unicast_to : eth_broadcast, mac,
	                 ETHERTYPE_IPV4, datagram, total);
}

int dhcp_parse_frame(const uint8_t *frame, size_t flen, const uint8_t mac[ETH_ALEN],
                     uint32_t xid, struct dhcp_lease *out)
{
	const uint8_t *ip, *udp, *dhcp, *opt, *end;
	size_t ihl, iplen, udplen, dhcplen;
	uint8_t msgtype = 0;
	uint32_t cookie;

	if (flen < ETH_HDR_LEN + 20 + 8 + BOOTP_LEN + 4)
		return 0;
	if (get_be16(frame + 12) != ETHERTYPE_IPV4)
		return 0;

	/* Accept broadcast or our own unicast; ignore anything else on the link. */
	if (memcmp(frame, eth_broadcast, ETH_ALEN) != 0 && memcmp(frame, mac, ETH_ALEN) != 0)
		return 0;

	ip = frame + ETH_HDR_LEN;
	if ((ip[0] >> 4) != 4)
		return 0;
	ihl = (size_t)(ip[0] & 0x0f) * 4;
	if (ihl < 20)
		return 0;
	iplen = get_be16(ip + 2);
	if (iplen < ihl || ETH_HDR_LEN + iplen > flen)
		return 0;
	if (ip[9] != 17)                          /* UDP */
		return 0;
	if (get_be16(ip + 6) & 0x3fff)            /* fragmented: not ours */
		return 0;

	udp = ip + ihl;
	if (iplen - ihl < 8)
		return 0;
	if (get_be16(udp) != UDP_PORT_SERVER || get_be16(udp + 2) != UDP_PORT_CLIENT)
		return 0;

	udplen = get_be16(udp + 4);
	if (udplen < 8 || udplen > iplen - ihl)
		return 0;

	dhcp = udp + 8;
	dhcplen = udplen - 8;
	if (dhcplen < BOOTP_LEN + 4)
		return -1;

	if (dhcp[0] != 2)                         /* BOOTREPLY */
		return 0;
	if (memcmp(dhcp + 4, &xid, 4) != 0)
		return 0;
	if (dhcp[2] == ETH_ALEN && memcmp(dhcp + 28, mac, ETH_ALEN) != 0)
		return 0;

	memcpy(&cookie, dhcp + BOOTP_LEN, 4);
	if (ntohl(cookie) != DHCP_COOKIE)
		return -1;

	memset(out, 0, sizeof *out);
	memcpy(&out->ip, dhcp + 16, 4);           /* yiaddr */

	opt = dhcp + BOOTP_LEN + 4;
	end = dhcp + dhcplen;

	while (opt < end) {
		uint8_t code = *opt++;
		uint8_t len;

		if (code == 0)                    /* pad */
			continue;
		if (code == 255)                  /* end */
			break;
		if (opt >= end)
			return -1;
		len = *opt++;
		if (opt + len > end)
			return -1;

		switch (code) {
		case 1:
			if (len >= 4)
				memcpy(&out->mask, opt, 4);
			break;
		case 3:
			if (len >= 4)
				memcpy(&out->router, opt, 4);
			break;
		case 6:
			for (int i = 0; i * 4 + 4 <= len && i < DHCP_MAX_DNS; i++) {
				memcpy(&out->dns[i], opt + i * 4, 4);
				out->ndns = i + 1;
			}
			break;
		case 15: {
			/*
			 * The domain ends up interpolated into a scutil script that
			 * runs as root, so keep only characters that can appear in a
			 * DNS name.  Sanitising here covers every consumer rather
			 * than trusting each one to remember.
			 */
			size_t n = len < sizeof out->domain - 1 ? len : sizeof out->domain - 1;
			size_t k = 0;

			for (size_t i = 0; i < n; i++) {
				uint8_t ch = opt[i];
				if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				    (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')
					out->domain[k++] = (char)ch;
			}
			out->domain[k] = '\0';
			break;
		}
		case 26:
			if (len >= 2)
				out->mtu = get_be16(opt);
			break;
		case 51:
			if (len >= 4) {
				uint32_t v;
				memcpy(&v, opt, 4);
				out->lease_secs = ntohl(v);
			}
			break;
		case 53:
			if (len >= 1)
				msgtype = opt[0];
			break;
		case 54:
			if (len >= 4)
				memcpy(&out->server, opt, 4);
			break;
		default:
			break;
		}
		opt += len;
	}

	if (!msgtype)
		return -1;

	/* siaddr is the fallback when the server omits option 54. */
	if (!out->server)
		memcpy(&out->server, dhcp + 20, 4);

	return msgtype;
}
