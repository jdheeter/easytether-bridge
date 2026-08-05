/*
 * utun: the supported way to get a packet interface from userspace on macOS.
 *
 * A utun is layer 3 and point to point, so every packet carries a four byte
 * address-family prefix in network byte order (AF_INET / AF_INET6) ahead of
 * the IP header.  Creating one needs root but no entitlement and no kext,
 * which is the whole reason this daemon can exist on Apple Silicon.
 */
#ifndef ET_UTUN_H
#define ET_UTUN_H

#include <stddef.h>
#include <stdint.h>
#include <net/if.h>

struct utun {
	int  fd;
	char name[IFNAMSIZ];
};

/* unit 0 lets the kernel pick the first free utunN. */
int  utun_open(struct utun *u, unsigned unit);
void utun_close(struct utun *u);

/*
 * Reads one packet.  On success returns its length and sets *af to AF_INET or
 * AF_INET6; the four byte prefix is stripped.  Returns 0 on EAGAIN and -1 on
 * a fatal error.
 */
ssize_t utun_read(struct utun *u, uint8_t *buf, size_t cap, int *af);

/* Writes one packet, adding the address-family prefix. */
ssize_t utun_write(struct utun *u, int af, const uint8_t *pkt, size_t len);

#endif /* ET_UTUN_H */
