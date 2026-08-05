#include "netcfg.h"
#include "util.h"

#include <SystemConfiguration/SystemConfiguration.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define IFCONFIG "/sbin/ifconfig"
#define ROUTE    "/sbin/route"

/* Runs a command, optionally feeding it stdin.  Returns its exit status. */
static int run(const char *path, char *const argv[], const char *stdin_text)
{
	posix_spawn_file_actions_t fa;
	pid_t pid;
	int pipefd[2] = { -1, -1 };
	int status, rc;

	if (stdin_text && pipe(pipefd) < 0) {
		log_err("pipe: %s", strerror(errno));
		return -1;
	}

	posix_spawn_file_actions_init(&fa);
	if (stdin_text) {
		posix_spawn_file_actions_adddup2(&fa, pipefd[0], STDIN_FILENO);
		posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	}
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

	rc = posix_spawn(&pid, path, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);

	if (rc != 0) {
		log_err("spawn %s: %s", path, strerror(rc));
		if (pipefd[0] >= 0) {
			close(pipefd[0]);
			close(pipefd[1]);
		}
		return -1;
	}

	if (stdin_text) {
		close(pipefd[0]);
		size_t left = strlen(stdin_text);
		const char *p = stdin_text;
		signal(SIGPIPE, SIG_IGN);
		while (left) {
			ssize_t w = write(pipefd[1], p, left);
			if (w <= 0) {
				if (w < 0 && errno == EINTR)
					continue;
				break;
			}
			p += w;
			left -= (size_t)w;
		}
		close(pipefd[1]);
	}

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int netcfg_configure(const char *ifname, const struct dhcp_lease *l)
{
	char addr[16], peer[16], mtu[8];
	int rc;

	/*
	 * A utun is point to point, so it takes a /32 and a peer address rather
	 * than the lease's subnet mask -- handing a /24 to a P2P interface is
	 * how you get a confusing SIOCAIFADDR failure.  Naming the gateway as
	 * the peer is what installs the host route that makes it reachable;
	 * nothing else exists on this link, so no subnet route is needed.
	 */
	snprintf(addr, sizeof addr, "%s", fmt_ip(l->ip));
	snprintf(peer, sizeof peer, "%s", fmt_ip(l->router ? l->router : l->ip));
	/* Option 26 comes off the wire; an absurd value would break the link. */
	snprintf(mtu, sizeof mtu, "%u",
	         l->mtu >= 576 && l->mtu <= ET_MTU ? l->mtu : ET_MTU);

	{
		char *const argv[] = { IFCONFIG, (char *)ifname, "inet", addr, peer,
		                       "netmask", "255.255.255.255",
		                       "mtu", mtu, "up", NULL };
		rc = run(IFCONFIG, argv, NULL);
	}
	if (rc != 0) {
		log_err("ifconfig %s failed (status %d)", ifname, rc);
		return -1;
	}

	log_info("%s: %s peer %s mtu %s", ifname, addr, peer, mtu);
	return 0;
}

int netcfg_set_default_route(const char *ifname)
{
	static const char *halves[] = { "0.0.0.0/1", "128.0.0.0/1" };

	for (size_t i = 0; i < sizeof halves / sizeof halves[0]; i++) {
		char *const argv[] = { ROUTE, "-q", "-n", "add", "-inet",
		                       (char *)halves[i], "-interface", (char *)ifname, NULL };
		if (run(ROUTE, argv, NULL) != 0) {
			/*
			 * Deliberately do NOT fall back to `route change`.  Our own
			 * routes die with the utun when the process exits, so a
			 * surviving 0.0.0.0/1 belongs to somebody else -- typically a
			 * VPN using the same split-default trick.  Taking it would
			 * break their tunnel with no way to give it back.
			 */
			log_err("%s is already routed elsewhere (a VPN?); refusing to "
			        "take it over. Traffic will not go through the phone.",
			        halves[i]);
			return -1;
		}
	}

	log_info("default route now goes through %s", ifname);
	return 0;
}

/*
 * Publishing the resolver has to happen from inside this process, not by
 * shelling out to scutil.
 *
 * Values in the SCDynamicStore belong to the session that set them, and configd
 * discards them the moment that session goes away.  Running `scutil` as a child
 * process therefore sets the keys and then destroys them microseconds later
 * when scutil exits -- the command appears to succeed and leaves nothing
 * behind.  Holding one session open for the lifetime of the daemon is what
 * makes the entries stick, and it gives exactly the cleanup semantics we want:
 * if this process dies for any reason, including SIGKILL, the resolver reverts
 * on its own.
 */
static SCDynamicStoreRef g_store;

static SCDynamicStoreRef store(void)
{
	if (!g_store) {
		g_store = SCDynamicStoreCreate(NULL, CFSTR("easytether-bridge"), NULL, NULL);
		if (!g_store)
			log_err("cannot open the SystemConfiguration store");
	}
	return g_store;
}

static CFStringRef service_key(const char *service_id, CFStringRef entity)
{
	CFStringRef svc = CFStringCreateWithCString(NULL, service_id, kCFStringEncodingUTF8);
	CFStringRef key;

	if (!svc)
		return NULL;
	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainState,
	                                                  svc, entity);
	CFRelease(svc);
	return key;
}

static CFStringRef cf_ip(uint32_t addr)
{
	return CFStringCreateWithCString(NULL, fmt_ip(addr), kCFStringEncodingUTF8);
}

static CFArrayRef cf_one(CFStringRef s)
{
	const void *v[1] = { s };
	return CFArrayCreate(NULL, v, 1, &kCFTypeArrayCallBacks);
}

int netcfg_set_dns(const char *service_id, const char *ifname, const struct dhcp_lease *l)
{
	SCDynamicStoreRef s = store();
	CFMutableArrayRef servers;
	CFMutableDictionaryRef dict;
	CFStringRef key, cfstr;
	int ok = 0;

	if (!s)
		return -1;
	if (l->ndns <= 0) {
		log_warn("phone offered no DNS servers; leaving the resolver alone");
		return 0;
	}

	/* --- the resolver itself --- */
	servers = CFArrayCreateMutable(NULL, l->ndns, &kCFTypeArrayCallBacks);
	for (int i = 0; i < l->ndns; i++) {
		cfstr = cf_ip(l->dns[i]);
		if (cfstr) {
			CFArrayAppendValue(servers, cfstr);
			CFRelease(cfstr);
		}
	}

	dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	                                 &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(dict, kSCPropNetDNSServerAddresses, servers);

	/*
	 * Register as a catch-all *supplemental* resolver as well.
	 *
	 * configd builds the global resolver from the services listed in
	 * Setup:/Network/Global/IPv4:ServiceOrder, and a utun published by a
	 * plain daemon is not in that list -- so ServerAddresses alone is
	 * quietly ignored and DNS keeps coming from whichever real interface is
	 * primary.  That looks fine until you switch Wi-Fi off, at which point
	 * the resolver leaves with it and nothing resolves any more.
	 *
	 * A supplemental resolver is merged regardless of service order, and an
	 * empty match domain matches every name.  This is the same mechanism
	 * VPNs use for split DNS.  Sending every lookup to the phone is the
	 * consistent choice here because every packet is already going there.
	 */
	{
		int order = 100;
		CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &order);
		CFArrayRef domains = cf_one(CFSTR(""));
		const void *ov[1] = { num };
		CFArrayRef orders = CFArrayCreate(NULL, ov, 1, &kCFTypeArrayCallBacks);

		CFDictionarySetValue(dict, kSCPropNetDNSSupplementalMatchDomains, domains);
		CFDictionarySetValue(dict, kSCPropNetDNSSupplementalMatchOrders, orders);

		CFRelease(orders);
		CFRelease(num);
		CFRelease(domains);
	}

	if (l->domain[0]) {
		cfstr = CFStringCreateWithCString(NULL, l->domain, kCFStringEncodingUTF8);
		if (cfstr) {
			CFArrayRef one = cf_one(cfstr);
			CFDictionarySetValue(dict, kSCPropNetDNSSearchDomains, one);
			CFDictionarySetValue(dict, kSCPropNetDNSDomainName, cfstr);
			CFRelease(one);
			CFRelease(cfstr);
		}
	}

	key = service_key(service_id, kSCEntNetDNS);
	if (key) {
		ok = SCDynamicStoreSetValue(s, key, dict);
		CFRelease(key);
	}
	CFRelease(dict);
	CFRelease(servers);

	if (!ok) {
		log_err("cannot publish the resolver into SystemConfiguration");
		return -1;
	}

	/*
	 * configd only folds a service's resolver into the global configuration
	 * when that service also has IPv4 state naming a real interface, so
	 * publish that as well.  With every other interface down this is also
	 * what lets the tether become the primary service, so the rest of the
	 * system stops believing the machine is offline.
	 */
	dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	                                 &kCFTypeDictionaryValueCallBacks);
	cfstr = cf_ip(l->ip);
	if (cfstr) {
		CFArrayRef one = cf_one(cfstr);
		CFDictionarySetValue(dict, kSCPropNetIPv4Addresses, one);
		CFRelease(one);
		CFRelease(cfstr);
	}
	cfstr = CFStringCreateWithCString(NULL, ifname, kCFStringEncodingUTF8);
	if (cfstr) {
		CFDictionarySetValue(dict, kSCPropInterfaceName, cfstr);
		CFRelease(cfstr);
	}
	if (l->router) {
		cfstr = cf_ip(l->router);
		if (cfstr) {
			CFDictionarySetValue(dict, kSCPropNetIPv4Router, cfstr);
			CFRelease(cfstr);
		}
	}

	key = service_key(service_id, kSCEntNetIPv4);
	if (key) {
		if (!SCDynamicStoreSetValue(s, key, dict))
			log_warn("cannot publish the IPv4 service entity");
		CFRelease(key);
	}
	CFRelease(dict);

	{
		char list[256] = { 0 };
		for (int i = 0; i < l->ndns; i++)
			snprintf(list + strlen(list), sizeof list - strlen(list),
			         "%s%s", i ? ", " : "", fmt_ip(l->dns[i]));
		log_info("DNS: %s%s%s", list, l->domain[0] ? " domain " : "", l->domain);
	}
	return 0;
}

int netcfg_clear_dns(const char *service_id)
{
	CFStringRef key;

	if (!g_store)
		return 0;

	key = service_key(service_id, kSCEntNetDNS);
	if (key) {
		SCDynamicStoreRemoveValue(g_store, key);
		CFRelease(key);
	}
	key = service_key(service_id, kSCEntNetIPv4);
	if (key) {
		SCDynamicStoreRemoveValue(g_store, key);
		CFRelease(key);
	}

	CFRelease(g_store);
	g_store = NULL;
	return 0;
}
