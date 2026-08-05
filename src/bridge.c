/*
 * The tethering session.
 *
 * Everything here runs between "the tunnel opened" and "the tunnel closed":
 * the record framing, the ARP responder and DHCP client that terminate layer 2
 * in userspace, the forwarding path, and the event loop that drives them.
 *
 * See docs/ARCHITECTURE.md for the state machine and the rules it must hold.
 */
#include "adb.h"
#include "bridge.h"
#include "netcfg.h"
#include "proto.h"
#include "util.h"
#include "utun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IN_BUF_MAX   (1u << 20)
#define OUT_BUF_MAX  (2u << 20)
#define READ_CHUNK   16384

#define ST_DISCOVER  0
#define ST_REQUEST   1
#define ST_ARP       2
#define ST_UP        3

struct bridge {
	int              tunfd;
	struct utun      utun;
	struct sbuf      in;
	struct sbuf      out;

	uint8_t          mac[ETH_ALEN];
	uint32_t         xid;
	int              state;

	struct dhcp_lease lease;
	uint8_t          gw_mac[ETH_ALEN];
	int              gw_known;

	uint64_t         deadline;      /* next retransmit             */
	int              tries;
	uint64_t         renew_at;      /* 0 when no renewal scheduled */
	uint64_t         lease_expires; /* 0 when the lease never expires   */
	uint64_t         started;

	int              configured;    /* interface + routes are live */
	int              want_dns;
	int              want_route;
	int              mtu_override;  /* 0 == take it from the lease */
	int              route_installed;

	int              link_checked;  /* sanity-checked the framing yet   */

	unsigned long    rx_pkts, tx_pkts, tx_drops;
};
static volatile sig_atomic_t g_stop;

void bridge_request_stop(void)
{
	g_stop = 1;
}

int bridge_stop_requested(void)
{
	return g_stop != 0;
}

void bridge_clear_stale_dns(void)
{
	netcfg_clear_dns(ET_SERVICE_ID);
}


static uint32_t random_xid(void)
{
	uint32_t v;
	arc4random_buf(&v, sizeof v);
	return v;
}

/* --------------------------------------------------------------- framing */

static int queue_frame(struct bridge *b, const uint8_t *frame, size_t len)
{
	uint8_t *dst;
	size_t n;

	if (len < ET_MIN_FRAME || len > ET_MAX_FRAME)
		return -1;

	dst = sbuf_reserve(&b->out, et_record_size(len));
	if (!dst) {
		b->tx_drops++;
		return -1;
	}

	n = et_record_encode(dst, frame, len);
	sbuf_commit(&b->out, n);
	return 0;
}

static int flush_out(struct bridge *b)
{
	while (b->out.len) {
		ssize_t w = write(b->tunfd, sbuf_data(&b->out), b->out.len);
		if (w < 0) {
			if (errno == EAGAIN)
				return 0;
			if (errno == EINTR)
				continue;
			log_err("tunnel write: %s", strerror(errno));
			return -1;
		}
		if (w == 0)
			return -1;
		sbuf_consume(&b->out, (size_t)w);
	}
	return 0;
}

/* ------------------------------------------------------------------- ARP */

static void send_arp_request(struct bridge *b)
{
	uint8_t frame[ET_MAX_FRAME];
	static const uint8_t unknown[ETH_ALEN] = { 0 };
	size_t n = arp_build(frame, ARP_OP_REQUEST, b->mac, b->lease.ip,
	                     unknown, b->lease.router);
	queue_frame(b, frame, n);
}

/* Announce our address so the phone can reach us without asking first. */
static void send_gratuitous_arp(struct bridge *b)
{
	uint8_t frame[ET_MAX_FRAME];
	size_t n = arp_build(frame, ARP_OP_REQUEST, b->mac, b->lease.ip,
	                     eth_broadcast, b->lease.ip);
	queue_frame(b, frame, n);
}

/*
 * The default route goes in only once the gateway's hardware address is known.
 * Installing it any earlier would point the whole machine at an interface that
 * cannot yet send anything, black-holing all traffic for as long as ARP takes.
 */
static void bridge_ready(struct bridge *b)
{
	if (b->state == ST_UP)
		return;

	if (b->want_route && !b->route_installed) {
		if (netcfg_set_default_route(b->utun.name) == 0) {
			b->route_installed = 1;
		} else {
			/*
			 * Come up anyway: the link itself works and the phone is
			 * reachable, only the default route is missing.  Refusing
			 * to reach ST_UP here would strand us in ST_ARP with
			 * nothing left to retry.
			 */
			log_warn("%s is up but the default route was left alone; "
			         "traffic will keep using your other connection",
			         b->utun.name);
		}
	}

	b->state = ST_UP;
	log_info("tethering is up: %s via %s (%s)", b->utun.name,
	         fmt_ip(b->lease.router), fmt_mac(b->gw_mac));
}

static void handle_arp(struct bridge *b, const uint8_t *frame, size_t len)
{
	struct arp_view a;

	if (arp_parse(frame, len, &a) != 0)
		return;

	/* Learn the gateway's hardware address from anything it sends. */
	static const uint8_t zero_mac[ETH_ALEN] = { 0 };

	if (b->lease.router && a.sender_ip == b->lease.router &&
	    memcmp(a.sender_mac, eth_broadcast, ETH_ALEN) != 0 &&
	    memcmp(a.sender_mac, zero_mac, ETH_ALEN) != 0) {
		if (!b->gw_known || memcmp(b->gw_mac, a.sender_mac, ETH_ALEN) != 0) {
			memcpy(b->gw_mac, a.sender_mac, ETH_ALEN);
			b->gw_known = 1;
			log_dbg("gateway %s is at %s", fmt_ip(a.sender_ip), fmt_mac(a.sender_mac));
			/*
			 * Only from ST_ARP.  Promoting out of ST_DISCOVER or
			 * ST_REQUEST -- which happens after a NAK, when the gateway
			 * is still chattering at us -- would make the ACK that
			 * follows look like a renewal of a lease we no longer hold.
			 */
			if (b->state == ST_ARP)
				bridge_ready(b);
		}
	}

	if (a.op == ARP_OP_REQUEST && b->lease.ip && a.target_ip == b->lease.ip) {
		uint8_t reply[ET_MAX_FRAME];
		size_t n = arp_build(reply, ARP_OP_REPLY, b->mac, b->lease.ip,
		                     a.sender_mac, a.sender_ip);
		queue_frame(b, reply, n);
	}
}

/* ------------------------------------------------------------------ DHCP */

/*
 * server_opt and dst_ip are deliberately separate.  RFC 2131 RENEWING wants a
 * unicast datagram to the leasing server but *no* server-identifier option, so
 * deriving the destination from the option value would address the packet to
 * 0.0.0.0 and it would never be answered.  dst_ip of 0 means broadcast.
 */
static void send_dhcp(struct bridge *b, uint8_t type, uint32_t ciaddr,
                      uint32_t requested, uint32_t server_opt, uint32_t dst_ip)
{
	int unicast = dst_ip != 0 && b->gw_known;
	uint8_t frame[ET_MAX_FRAME];
	size_t n;

	n = dhcp_build(frame, type, b->mac, b->xid, ciaddr, requested, server_opt,
	               unicast ? b->gw_mac : NULL, unicast ? dst_ip : 0, "mac");
	queue_frame(b, frame, n);
}

static int backoff_ms(int tries)
{
	int ms = 1000 << (tries > 3 ? 3 : tries);
	return ms > 8000 ? 8000 : ms;
}

static void enter_discover(struct bridge *b)
{
	b->state = ST_DISCOVER;
	b->xid = random_xid();
	b->tries = 0;
	b->started = now_ms();          /* a restart gets its own 60s budget */
	b->renew_at = 0;                /* the old lease is gone */
	b->lease_expires = 0;
	memset(&b->lease, 0, sizeof b->lease);
	b->gw_known = 0;
	send_dhcp(b, DHCP_DISCOVER, 0, 0, 0, 0);
	b->deadline = now_ms() + (uint64_t)backoff_ms(0);
	log_info("asking the phone for an address (DHCP)");
}

/* T1 is half the lease; an infinite or absent lease never renews. */
static void schedule_renewal(struct bridge *b)
{
	uint32_t secs = b->lease.lease_secs;

	if (!secs || secs == 0xffffffffu) {
		b->renew_at = 0;
		b->lease_expires = 0;
	} else {
		b->renew_at = now_ms() + (uint64_t)secs * 500;
		b->lease_expires = now_ms() + (uint64_t)secs * 1000;
	}
}

static int apply_lease(struct bridge *b)
{
	struct dhcp_lease *l = &b->lease;

	if (!l->ip) {
		log_err("DHCP acknowledged without an address");
		return -1;
	}
	if (!l->mask)
		l->mask = htonl(0xffffff00u);
	if (!l->router) {
		/*
		 * Some builds of the phone app omit option 3; the DHCP server
		 * itself is the gateway in every EasyTether topology.
		 */
		l->router = l->server;
		if (l->router)
			log_warn("no router option; using the DHCP server %s", fmt_ip(l->router));
	}
	if (!l->router) {
		log_err("no gateway offered by the phone");
		return -1;
	}

	if (b->mtu_override)
		l->mtu = (uint16_t)b->mtu_override;

	log_info("lease: %s/%s gateway %s%s", fmt_ip(l->ip), fmt_ip(l->mask),
	         fmt_ip(l->router),
	         l->lease_secs == 0xffffffffu ? " (infinite lease)"
	                                      : l->lease_secs ? "" : " (no expiry given)");

	if (netcfg_configure(b->utun.name, l) != 0)
		return -1;
	if (b->want_dns)
		netcfg_set_dns(ET_SERVICE_ID, b->utun.name, l);

	b->configured = 1;

	send_gratuitous_arp(b);

	schedule_renewal(b);

	if (b->gw_known) {
		bridge_ready(b);
	} else {
		b->state = ST_ARP;
		b->tries = 0;
		send_arp_request(b);
		b->deadline = now_ms() + 500;
	}
	return 0;
}

static int handle_dhcp(struct bridge *b, int type, const struct dhcp_lease *l)
{
	switch (type) {
	case DHCP_OFFER:
		if (b->state != ST_DISCOVER)
			return 0;
		log_dbg("offer of %s from %s", fmt_ip(l->ip), fmt_ip(l->server));
		b->lease = *l;
		b->state = ST_REQUEST;
		b->tries = 0;
		send_dhcp(b, DHCP_REQUEST, 0, l->ip, l->server, 0);
		b->deadline = now_ms() + (uint64_t)backoff_ms(0);
		return 0;

	case DHCP_ACK: {
		uint32_t old_ip = b->lease.ip;

		if (b->state == ST_REQUEST) {
			b->lease = *l;
			return apply_lease(b) == 0 ? 0 : -1;
		}
		if (b->state == ST_UP || b->state == ST_ARP) {
			/*
			 * A renewal that changes our address needs the interface,
			 * routes and resolver rebuilt from scratch; reconfiguring in
			 * place would leave the old address aliased on the utun.
			 * Dropping the session makes the reconnect loop do it
			 * properly.
			 */
			if (l->ip && l->ip != old_ip) {
				log_warn("phone reassigned us %s (was %s); reconnecting",
				         fmt_ip(l->ip), fmt_ip(old_ip));
				return -1;
			}
			if (l->lease_secs)
				b->lease.lease_secs = l->lease_secs;
			schedule_renewal(b);
			log_dbg("lease renewed");
		}
		return 0;
	}

	case DHCP_NAK:
		log_warn("phone refused the lease; starting over");
		enter_discover(b);
		return 0;

	default:
		return 0;
	}
}

/* --------------------------------------------------------- frame ingress */

static void handle_ipv4(struct bridge *b, const uint8_t *frame, size_t len)
{
	const uint8_t *ip = frame + ETH_HDR_LEN;
	size_t avail = len - ETH_HDR_LEN;
	size_t total;

	if (avail < 20)
		return;

	/* Trim the Ethernet padding: hand the stack exactly the datagram. */
	total = get_be16(ip + 2);
	if (total < 20 || total > avail)
		total = avail;

	if (utun_write(&b->utun, AF_INET, ip, total) > 0)
		b->rx_pkts++;
}

/*
 * The vendor's macOS driver hands these records straight to an Ethernet
 * controller, so they are Ethernet frames.  Its newer Linux builds present an
 * L3 tun instead, which leaves a little doubt about whether the phone can also
 * speak raw IP on this socket.  Check the first real record rather than
 * silently dropping everything if that ever turns out to be the case.
 */
static void check_link_framing(struct bridge *b, const uint8_t *frame, size_t len)
{
	uint16_t type = eth_type(frame);

	b->link_checked = 1;

	if (type == ETHERTYPE_ARP || type == ETHERTYPE_IPV4 || type == ETHERTYPE_IPV6)
		return;

	if ((frame[0] >> 4) == 4 && get_be16(frame + 2) == len)
		log_err("the phone is sending raw IP, not Ethernet -- this build only "
		        "speaks the Ethernet framing the macOS driver used");
	else
		log_warn("unrecognised frame on the tunnel (ethertype 0x%04x, %zu bytes)",
		         type, len);
}

static int handle_frame(struct bridge *b, const uint8_t *frame, size_t len)
{
	uint16_t type;
	struct dhcp_lease l;
	int mt;

	if (len < ET_MIN_FRAME)
		return 0;

	if (!b->link_checked)
		check_link_framing(b, frame, len);

	/* Ignore unicast destined for somebody else on the phone's bridge. */
	if ((frame[0] & 1) == 0 && memcmp(frame, b->mac, ETH_ALEN) != 0)
		return 0;

	type = eth_type(frame);

	switch (type) {
	case ETHERTYPE_ARP:
		handle_arp(b, frame, len);
		return 0;

	case ETHERTYPE_IPV4:
		mt = dhcp_parse_frame(frame, len, b->mac, b->xid, &l);
		if (mt > 0)
			return handle_dhcp(b, mt, &l);
		if (b->configured)
			handle_ipv4(b, frame, len);
		return 0;

	default:
		/*
		 * IPv6 would need SLAAC and neighbour discovery up here; the
		 * phone's tether link is IPv4-only in practice, so drop it
		 * rather than half-support it.
		 */
		return 0;
	}
}

static int parse_incoming(struct bridge *b)
{
	for (;;) {
		const uint8_t *frame = NULL;
		size_t flen = 0, rec = 0;

		switch (et_record_peek(sbuf_data(&b->in), b->in.len, &frame, &flen, &rec)) {
		case ET_RECORD_INCOMPLETE:
			return 0;

		case ET_RECORD_DESYNC:
			/*
			 * There is no resync marker to hunt for -- the payload is
			 * arbitrary binary.  Drop the session, as the vendor does.
			 */
			log_err("tunnel desynchronised (record length %u)",
			        get_le16(sbuf_data(&b->in)));
			return -1;

		case ET_RECORD_FRAME:
			if (handle_frame(b, frame, flen) < 0)
				return -1;
			break;

		case ET_RECORD_SKIP:
			break;
		}

		sbuf_consume(&b->in, rec);
	}
}

static int pump_tunnel(struct bridge *b)
{
	/*
	 * Bounded so a fast peer cannot keep us in here indefinitely, starving
	 * the writer, the timers and our reaction to SIGTERM.  Whatever is left
	 * is still readable, so poll returns immediately next time round.
	 */
	for (int i = 0; i < 64; i++) {
		uint8_t *dst = sbuf_reserve(&b->in, READ_CHUNK);
		ssize_t r;

		if (!dst) {
			log_err("inbound buffer exhausted");
			return -1;
		}

		r = read(b->tunfd, dst, READ_CHUNK);
		if (r < 0) {
			if (errno == EAGAIN)
				return 0;
			if (errno == EINTR)
				continue;
			log_err("tunnel read: %s", strerror(errno));
			return -1;
		}
		if (r == 0) {
			log_err("the phone closed the tunnel");
			return -1;
		}

		sbuf_commit(&b->in, (size_t)r);
		if (parse_incoming(b) < 0)
			return -1;

		if (r < READ_CHUNK)
			return 0;
	}
	return 0;
}

/* ---------------------------------------------------------- utun egress */

static int pump_utun(struct bridge *b)
{
	uint8_t pkt[ET_MTU];
	uint8_t frame[ET_MAX_FRAME];

	for (int i = 0; i < 128; i++) {
		int af = 0;
		ssize_t n = utun_read(&b->utun, pkt, sizeof pkt, &af);

		if (n < 0)
			return -1;
		if (n == 0)
			return 0;
		if (af != AF_INET)
			continue;
		if (!b->gw_known) {
			b->tx_drops++;
			continue;
		}

		size_t flen = eth_build(frame, b->gw_mac, b->mac, ETHERTYPE_IPV4,
		                        pkt, (size_t)n);
		if (queue_frame(b, frame, flen) == 0)
			b->tx_pkts++;

		if (b->out.len > OUT_BUF_MAX / 2)
			return 0;       /* let the writer catch up */
	}
	return 0;
}

/* ----------------------------------------------------------------- timers */

static uint64_t next_deadline(const struct bridge *b)
{
	uint64_t d = b->deadline;

	if (b->renew_at && (!d || b->renew_at < d))
		d = b->renew_at;
	return d;
}

static int on_timer(struct bridge *b)
{
	uint64_t now = now_ms();

	if (b->lease_expires && now >= b->lease_expires) {
		/*
		 * Renewals have been going unanswered right up to the deadline.
		 * The address is no longer ours, so stop pretending and ask for
		 * a new one rather than renewing something we no longer hold.
		 */
		log_warn("the lease expired without being renewed; asking again");
		enter_discover(b);
		return 0;
	}

	if (b->renew_at && now >= b->renew_at) {
		log_dbg("renewing the lease");
		b->xid = random_xid();
		/*
		 * RENEWING per RFC 2131: ciaddr set, no requested-address and no
		 * server-identifier option, sent unicast to the leasing server.
		 */
		send_dhcp(b, DHCP_REQUEST, b->lease.ip, 0, 0, b->lease.server);
		/* Retry at a tenth of the remaining lease, floor 30s. */
		uint64_t retry = b->lease.lease_secs ? (uint64_t)b->lease.lease_secs * 100 : 60000;
		b->renew_at = now + (retry < 30000 ? 30000 : retry);
	}

	if (!b->deadline || now < b->deadline)
		return 0;

	switch (b->state) {
	case ST_DISCOVER:
		if (now - b->started > 60000) {
			log_err("no DHCP response after 60s -- is USB tethering switched "
			        "on inside the EasyTether app?");
			return -1;
		}
		b->tries++;
		send_dhcp(b, DHCP_DISCOVER, 0, 0, 0, 0);
		b->deadline = now + (uint64_t)backoff_ms(b->tries);
		break;

	case ST_REQUEST:
		b->tries++;
		if (b->tries > 4) {
			log_warn("no response to our DHCP request; starting over");
			enter_discover(b);
			break;
		}
		send_dhcp(b, DHCP_REQUEST, 0, b->lease.ip, b->lease.server, 0);
		b->deadline = now + (uint64_t)backoff_ms(b->tries);
		break;

	case ST_ARP:
		b->tries++;
		if (b->tries == 20)
			log_warn("gateway %s is not answering ARP yet", fmt_ip(b->lease.router));
		send_arp_request(b);
		b->deadline = now + (b->tries < 10 ? 500 : 3000);
		break;

	case ST_UP:
	default:
		b->deadline = 0;
		break;
	}

	return 0;
}

/* ------------------------------------------------------------ session run */

static int run_one(struct bridge *b, int adb_port, const char *serial)
{
	char err[512] = { 0 };
	uint32_t hello = ET_HELLO_MAGIC;
	uint8_t hello_bytes[4];
	int rc = -1;

	b->tunfd = adb_open_stream(adb_port, serial, err, sizeof err);
	if (b->tunfd < 0) {
		log_err("%s", err);
		return -1;
	}
	set_nonblock(b->tunfd);
	log_info("tunnel open to %s", ET_ADB_SERVICE);

	if (utun_open(&b->utun, 0) != 0)
		goto out;

	if (sbuf_init(&b->in, 65536, IN_BUF_MAX) != 0 ||
	    sbuf_init(&b->out, 65536, OUT_BUF_MAX) != 0) {
		log_err("out of memory");
		goto out;
	}

	/* The handshake the vendor's driver sends before any frame. */
	memcpy(hello_bytes, &hello, 4);
	sbuf_append(&b->out, hello_bytes, 4);

	b->started = now_ms();
	enter_discover(b);

	while (!g_stop) {
		struct pollfd pf[2];
		uint64_t d, now;
		int timeout;

		pf[0].fd = b->tunfd;
		pf[0].events = POLLIN | (b->out.len ? POLLOUT : 0);
		pf[0].revents = 0;
		pf[1].fd = b->utun.fd;
		pf[1].events = b->out.len < OUT_BUF_MAX / 2 ? POLLIN : 0;
		pf[1].revents = 0;

		now = now_ms();
		d = next_deadline(b);
		/*
		 * Capped at a second.  An infinite-lease renewal deadline is far
		 * enough out that the millisecond delta overflows an int, and a
		 * long sleep would also delay our reaction to SIGTERM.
		 */
		timeout = 1000;
		if (d) {
			uint64_t delta = d > now ? d - now : 0;
			if (delta < 1000)
				timeout = (int)delta;
		}

		if (poll(pf, 2, timeout) < 0) {
			if (errno == EINTR)
				continue;
			log_err("poll: %s", strerror(errno));
			goto out;
		}

		if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			log_err("the phone dropped the tunnel");
			goto out;
		}
		if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			log_err("%s went away", b->utun.name);
			goto out;
		}

		if (pf[1].revents & POLLIN) {
			if (pump_utun(b) < 0)
				goto out;
		}
		if (pf[0].revents & POLLIN) {
			if (pump_tunnel(b) < 0)
				goto out;
		}
		if (on_timer(b) < 0)
			goto out;
		if (flush_out(b) < 0)
			goto out;
	}

	rc = 0;

out:
	if (b->configured && b->want_dns)
		netcfg_clear_dns(ET_SERVICE_ID);
	if (b->utun.fd > 0)
		utun_close(&b->utun);
	if (b->tunfd >= 0)
		close(b->tunfd);
	b->tunfd = -1;
	sbuf_free(&b->in);
	sbuf_free(&b->out);

	if (b->rx_pkts || b->tx_pkts)
		log_info("session ended: %lu packets in, %lu out, %lu dropped",
		         b->rx_pkts, b->tx_pkts, b->tx_drops);
	return rc;
}

/* -------------------------------------------------------------------- main */
int bridge_run_session(const struct bridge_config *cfg, int *reached_lease)
{
	struct bridge b;
	int rc;

	/*
	 * A fresh, zeroed bridge per session.  State surviving a reconnect used
	 * to be a reliable source of bugs -- a stale "configured" flag made the
	 * daemon announce that tethering was up against a utun it had not
	 * configured yet.  Allocating here makes that structurally impossible
	 * rather than something a reset list has to remember.
	 */
	memset(&b, 0, sizeof b);
	b.tunfd = -1;
	b.utun.fd = -1;
	b.want_dns = cfg->want_dns;
	b.want_route = cfg->want_route;
	b.mtu_override = cfg->mtu_override;
	memcpy(b.mac, cfg->mac, ETH_ALEN);

	rc = run_one(&b, cfg->adb_port, cfg->serial);

	if (reached_lease)
		*reached_lease = b.configured;
	return rc;
}
