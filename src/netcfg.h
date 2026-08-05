/*
 * Interface, route and DNS configuration.
 *
 * These all shell out to the system tools (ifconfig, route, scutil) via
 * posix_spawn with an argument vector, which is what every VPN client on this
 * platform ends up doing: there is no stable public C API for the routing and
 * DNS pieces, and configd owns the resolver.
 */
#ifndef ET_NETCFG_H
#define ET_NETCFG_H

#include <stdint.h>
#include "proto.h"

/*
 * Brings the interface up with the address from the lease.  The utun is point
 * to point, so we give it the router as the peer address; the phone is the
 * only thing on the far side.
 */
int netcfg_configure(const char *ifname, const struct dhcp_lease *l);

/*
 * Installs 0.0.0.0/1 and 128.0.0.0/1 through the interface.  Two halves beat
 * the existing default route on specificity without deleting it, so Wi-Fi
 * comes straight back when this interface disappears.  Safe here in a way it
 * would not be for a VPN: our transport is USB, not IP, so there is no
 * possibility of routing the tunnel through itself.
 */
int netcfg_set_default_route(const char *ifname);

/*
 * Publishes the lease's resolvers into the SCDynamicStore so configd merges
 * them into the system resolver, and registers the interface as a network
 * service so it can win the DNS ordering.
 */
int netcfg_set_dns(const char *service_id, const char *ifname, const struct dhcp_lease *l);
int netcfg_clear_dns(const char *service_id);

#endif /* ET_NETCFG_H */
