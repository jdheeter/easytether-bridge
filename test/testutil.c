#include "testutil.h"

#include "../src/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define BOOTP_LEN        236
#define DHCP_COOKIE      0x63825363u
#define DHCP_MIN_PAYLOAD 300

unsigned long tu_padding_violations;

uint32_t tu_ip4(const char *s)
{
	struct in_addr a;

	if (inet_pton(AF_INET, s, &a) != 1)
		return 0;
	return a.s_addr;
}

/* ------------------------------------------------------------------ link */

int tu_send_frame(int fd, const uint8_t *frame, size_t len)
{
	uint8_t rec[ET_MAX_FRAME + 8];
	size_t n = et_record_encode(rec, frame, len);

	if (!n)
		return -1;
	return io_write_all(fd, rec, n);
}

void tu_stream_init(struct tu_stream *s, int fd)
{
	s->fd = fd;
	s->have = 0;
}

ssize_t tu_recv_frame(struct tu_stream *s, uint8_t *out, int timeout_ms)
{
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;

	for (;;) {
		const uint8_t *frame = NULL;
		size_t flen = 0, rec = 0;
		struct pollfd pf;
		ssize_t r;
		int left;

		switch (et_record_peek(s->buf, s->have, &frame, &flen, &rec)) {
		case ET_RECORD_FRAME:
			for (size_t i = 2 + flen; i < rec; i++)
				if (s->buf[i] != 0)
					tu_padding_violations++;
			memcpy(out, frame, flen);
			memmove(s->buf, s->buf + rec, s->have - rec);
			s->have -= rec;
			return (ssize_t)flen;

		case ET_RECORD_SKIP:
			memmove(s->buf, s->buf + rec, s->have - rec);
			s->have -= rec;
			continue;

		case ET_RECORD_DESYNC:
			return -1;

		case ET_RECORD_INCOMPLETE:
			break;
		}

		left = (int)(deadline - now_ms());
		if (left <= 0 || s->have == sizeof s->buf)
			return 0;

		pf.fd = s->fd;
		pf.events = POLLIN;
		pf.revents = 0;
		if (poll(&pf, 1, left) <= 0)
			return 0;

		r = read(s->fd, s->buf + s->have, sizeof s->buf - s->have);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		s->have += (size_t)r;
	}
}

ssize_t tu_recv_type(struct tu_stream *s, uint8_t *out, uint16_t ethertype, int timeout_ms)
{
	uint64_t deadline = now_ms() + (uint64_t)timeout_ms;

	for (;;) {
		int left = (int)(deadline - now_ms());
		ssize_t n;

		if (left <= 0)
			return 0;

		n = tu_recv_frame(s, out, left);
		if (n <= 0)
			return n;
		if (eth_type(out) == ethertype)
			return n;
	}
}

/* ------------------------------------------------------------------ DHCP */

size_t tu_dhcp_reply(uint8_t *out, uint8_t msgtype, const uint8_t client_mac[ETH_ALEN],
                     uint32_t xid, const uint8_t server_mac[ETH_ALEN],
                     const struct tu_lease_opts *o)
{
	uint8_t dg[576];
	uint8_t *dhcp = dg + 28;                /* 20 IPv4 + 8 UDP */
	uint8_t *p;
	size_t dhcp_len, udp_len, total;
	uint32_t cookie = htonl(DHCP_COOKIE);
	uint32_t lease_be = htonl(o->lease_secs);
	uint32_t bcast = htonl(INADDR_BROADCAST);
	uint16_t ck;

	memset(dg, 0, sizeof dg);

	dhcp[0] = 2;                            /* BOOTREPLY */
	dhcp[1] = 1;
	dhcp[2] = ETH_ALEN;
	memcpy(dhcp + 4, &xid, 4);
	memcpy(dhcp + 16, &o->yiaddr, 4);
	memcpy(dhcp + 20, &o->server, 4);       /* siaddr */
	memcpy(dhcp + 28, client_mac, ETH_ALEN);
	memcpy(dhcp + BOOTP_LEN, &cookie, 4);

	p = dhcp + BOOTP_LEN + 4;
	*p++ = 53; *p++ = 1; *p++ = msgtype;
	*p++ = 54; *p++ = 4; memcpy(p, &o->server, 4); p += 4;
	*p++ = 1;  *p++ = 4; memcpy(p, &o->mask, 4);   p += 4;
	*p++ = 3;  *p++ = 4; memcpy(p, &o->router, 4); p += 4;

	if (o->ndns > 0) {
		*p++ = 6;
		*p++ = (uint8_t)(o->ndns * 4);
		for (int i = 0; i < o->ndns; i++) {
			memcpy(p, &o->dns[i], 4);
			p += 4;
		}
	}

	*p++ = 51; *p++ = 4; memcpy(p, &lease_be, 4); p += 4;

	if (o->mtu) {
		*p++ = 26; *p++ = 2;
		put_be16(p, o->mtu);
		p += 2;
	}
	if (o->domain && *o->domain) {
		size_t dl = strlen(o->domain);
		if (dl > 63)
			dl = 63;
		*p++ = 15; *p++ = (uint8_t)dl;
		memcpy(p, o->domain, dl);
		p += dl;
	}

	*p++ = 255;

	dhcp_len = (size_t)(p - dhcp);
	if (dhcp_len < DHCP_MIN_PAYLOAD)
		dhcp_len = DHCP_MIN_PAYLOAD;

	udp_len = 8 + dhcp_len;
	total = 20 + udp_len;

	dg[0] = 0x45;
	put_be16(dg + 2, (uint16_t)total);
	dg[8] = 64;
	dg[9] = 17;
	memcpy(dg + 12, &o->server, 4);
	memcpy(dg + 16, &bcast, 4);
	ck = ip_checksum(dg, 20);
	put_be16(dg + 10, ck);

	put_be16(dg + 20, 67);
	put_be16(dg + 22, 68);
	put_be16(dg + 24, (uint16_t)udp_len);
	ck = udp_checksum(o->server, bcast, dg + 20, udp_len);
	put_be16(dg + 26, ck);

	return eth_build(out, eth_broadcast, server_mac, ETHERTYPE_IPV4, dg, total);
}

int tu_peek_dhcp(const uint8_t *frame, size_t len, uint8_t *msgtype, uint32_t *xid,
                 uint32_t *dst_ip)
{
	const uint8_t *ip = frame + ETH_HDR_LEN, *udp, *dhcp, *opt, *end;
	size_t ihl, iplen, udplen;

	*msgtype = 0;

	if (len < ETH_HDR_LEN + 20 + 8 + BOOTP_LEN + 4)
		return 0;
	if (eth_type(frame) != ETHERTYPE_IPV4)
		return 0;
	if ((ip[0] >> 4) != 4 || ip[9] != 17)
		return 0;

	ihl = (size_t)(ip[0] & 0x0f) * 4;
	iplen = get_be16(ip + 2);
	if (ihl < 20 || iplen > len - ETH_HDR_LEN || iplen < ihl + 8)
		return 0;

	udp = ip + ihl;
	if (get_be16(udp) != 68 || get_be16(udp + 2) != 67)
		return 0;

	udplen = get_be16(udp + 4);
	if (udplen < 8 + BOOTP_LEN + 4 || udplen > iplen - ihl)
		return 0;

	dhcp = udp + 8;
	if (dhcp[0] != 1)                       /* BOOTREQUEST */
		return 0;

	memcpy(xid, dhcp + 4, 4);
	if (dst_ip)
		memcpy(dst_ip, ip + 16, 4);

	opt = dhcp + BOOTP_LEN + 4;
	end = dhcp + (udplen - 8);
	while (opt < end) {
		uint8_t code = *opt++;
		uint8_t l;

		if (code == 0)
			continue;
		if (code == 255 || opt >= end)
			break;
		l = *opt++;
		if (opt + l > end)
			break;
		if (code == 53 && l >= 1)
			*msgtype = opt[0];
		opt += l;
	}

	return *msgtype != 0;
}
