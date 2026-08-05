/*
 * One tethering session: connect, get a lease, forward packets until something
 * ends it.  main.c owns the process; this owns the connection.
 */
#ifndef ET_BRIDGE_H
#define ET_BRIDGE_H

#include <stdint.h>

#include "proto.h"

/* Identifies us to the system's network configuration database. */
#define ET_SERVICE_ID "com.mobile-stream.easytether-bridge"

struct bridge_config {
	int         adb_port;
	const char *serial;        /* NULL: whichever device is attached */
	int         want_dns;
	int         want_route;
	int         mtu_override;  /* 0: take it from the lease          */
	uint8_t     mac[ETH_ALEN];
};

/*
 * Runs one session to completion.  Returns 0 when a stop was requested and -1
 * when the session failed.  *reached_lease, if given, reports whether DHCP got
 * far enough to configure the interface -- the caller uses that to tell "the
 * phone went away" apart from "we never had a phone".
 */
int bridge_run_session(const struct bridge_config *cfg, int *reached_lease);

/* Async-signal-safe: ask the running session to wind up. */
void bridge_request_stop(void);

/* True once a stop has been requested. */
int bridge_stop_requested(void);

/* Drop resolver entries a previous run may have left behind after a SIGKILL. */
void bridge_clear_stale_dns(void);

#endif /* ET_BRIDGE_H */
