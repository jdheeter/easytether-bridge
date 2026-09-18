/*
 * easytether-bridge -- a native macOS host driver for EasyTether.
 *
 * The vendor's driver creates its virtual network interface with
 * IOEthernetControllerCreate (IOUserEthernet).  That interface is gated behind
 * an Apple-private entitlement, so on Apple Silicon the daemon dies at startup
 * with "cannot create user_ethernet instance (lack of permissions?)" and no
 * tethering ever happens.  This replaces it, presenting the same tunnel on a
 * utun, which needs nothing but root.
 *
 * This file is the process: arguments, privileges, signals, locating the ADB
 * server, and the reconnect loop.  The session itself lives in bridge.c.
 *
 * See docs/ for the protocol, the architecture and a porting guide.
 */
#include "adb.h"
#include "bridge.h"
#include "proto.h"
#include "util.h"

#include <getopt.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <uuid/uuid.h>
#endif
#ifdef __linux__
#include <dirent.h>
#endif

/* ------------------------------------------------------------ addressing */
/*
 * A stable locally-administered MAC, derived from the host UUID, so the phone
 * hands out the same lease across restarts instead of leaking addresses.
 */
static void make_mac(uint8_t mac[ETH_ALEN])
{
	uint8_t host[32];
	uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
	size_t n = 0;

	memset(host, 0x5a, sizeof host);
#ifdef __APPLE__
	{
		uuid_t uuid;
		struct timespec wait = { 0, 0 };
		if (gethostuuid(uuid, &wait) == 0) {
			memcpy(host, uuid, sizeof uuid);
			n = sizeof uuid;
		}
	}
#endif
	if (n == 0) {
		FILE *f = fopen("/etc/machine-id", "r");
		if (f) {
			n = fread(host, 1, sizeof host, f);
			fclose(f);
		}
	}
	if (n == 0)
		n = sizeof host;

	for (size_t i = 0; i < n; i++) {
		h ^= host[i];
		h *= 1099511628211ULL;
	}

	mac[0] = 0x02;                          /* locally administered, unicast */
	mac[1] = (uint8_t)(h >> 40);
	mac[2] = (uint8_t)(h >> 32);
	mac[3] = (uint8_t)(h >> 24);
	mac[4] = (uint8_t)(h >> 16);
	mac[5] = (uint8_t)(h >> 8);
}

/*
 * Whose ADB server should we use?  Under sudo that is obvious; started by
 * launchd it is whoever owns the login session, because the phone's
 * authorization is tied to that user's ~/.android/adbkey.  Running one as root
 * would use root's key and pop a fresh RSA prompt on the phone.
 */
static const struct passwd *console_user(void)
{
	const char *env = getenv("SUDO_USER");
	struct stat st;

	if (env && *env) {
		const struct passwd *pw = getpwnam(env);
		if (pw)
			return pw;
	}
	if (stat("/dev/console", &st) == 0 && st.st_uid != 0)
		return getpwuid(st.st_uid);
	return NULL;
}

/*
 * Linux: ADB is USB class 255 / subclass 66 / protocol 1.  After reboot the
 * phone is often plugged in before adbd is ready (still locked, or the USB
 * mode notification has not been accepted).  launchd/udev only start us on
 * the *add* event, so giving up while that interface still exists leaves the
 * daemon dead until the next unplug.  Keep retrying in that case.
 */
static int adb_usb_iface_present(void)
{
#ifdef __linux__
	DIR *dir = opendir("/sys/bus/usb/devices");
	struct dirent *de;

	if (!dir)
		return 0;
	while ((de = readdir(dir)) != NULL) {
		char path[512];
		unsigned cls = 0, sub = 0, proto = 0;
		FILE *f;
		int n = 0;

		if (!strchr(de->d_name, ':'))
			continue;
		n = snprintf(path, sizeof path,
		             "/sys/bus/usb/devices/%s/bInterfaceClass", de->d_name);
		if (n < 0 || (size_t)n >= sizeof path)
			continue;
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%x", &cls) != 1) {
			fclose(f);
			continue;
		}
		fclose(f);
		snprintf(path, sizeof path,
		         "/sys/bus/usb/devices/%s/bInterfaceSubClass", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%x", &sub) != 1) {
			fclose(f);
			continue;
		}
		fclose(f);
		snprintf(path, sizeof path,
		         "/sys/bus/usb/devices/%s/bInterfaceProtocol", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%x", &proto) != 1) {
			fclose(f);
			continue;
		}
		fclose(f);
		if (cls == 0xff && sub == 0x42 && proto == 0x01) {
			closedir(dir);
			return 1;
		}
	}
	closedir(dir);
#endif
	return 0;
}

static const char *find_adb(const struct passwd *pw)
{
	static char path[PATH_MAX];
	static const char *fixed[] = {
		"/usr/bin/adb",
		"/usr/lib/android-sdk/platform-tools/adb",
		"/opt/homebrew/bin/adb",
		"/usr/local/bin/adb",
		"/opt/local/bin/adb",
	};

	if (pw && pw->pw_dir) {
		static const char *homes[] = {
			"%s/Library/Android/sdk/platform-tools/adb",
			"%s/Android/Sdk/platform-tools/adb",
		};
		for (size_t i = 0; i < sizeof homes / sizeof homes[0]; i++) {
			snprintf(path, sizeof path, homes[i], pw->pw_dir);
			if (access(path, X_OK) == 0)
				return path;
		}
	}
	for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
		if (access(fixed[i], X_OK) == 0) {
			snprintf(path, sizeof path, "%s", fixed[i]);
			return path;
		}
	}
	return "adb";           /* fall back to PATH */
}

/* -------------------------------------------------------------------- main */

static void on_signal(int sig)
{
	(void)sig;
	bridge_request_stop();
}

static void usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s [-s serial] [-p adb-port] [-A adb-path] [-m mtu]\n"
	        "       %*s [-D] [-R] [-1] [-v] [-q]\n"
	        "\n"
	        "  -s serial     use a specific device (as shown by `adb devices`)\n"
	        "  -p port       port of the local ADB server (default %d)\n"
	        "  -A path       path to the adb binary used to start a server\n"
	        "  -D            do not touch the system DNS configuration\n"
	        "  -R            do not send the default route through the phone\n"
	        "  -m mtu        override the link MTU (default: from the lease, else 1500)\n"
	        "  -1            run one session and exit instead of reconnecting\n"
	        "  -v            verbose\n"
	        "  -q            errors only\n"
	        "\n"
	        "Needs root: creating a tun/utun and editing routes are privileged.\n",
	        prog, (int)strlen(prog), "", ET_ADB_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
	struct bridge_config cfg;
	const char *adb_path = NULL;         /* NULL: search the usual places */
	int once = 0;
	int level = ET_LOG_INFO;
	int backoff = 1;
	int dead_tries = 0;
	int c;

	memset(&cfg, 0, sizeof cfg);
	cfg.adb_port = ET_ADB_DEFAULT_PORT;
	cfg.want_dns = 1;
	cfg.want_route = 1;

	while ((c = getopt(argc, argv, "s:p:A:m:DR1vqh")) != -1) {
		switch (c) {
		case 's': cfg.serial = optarg; break;
		case 'p': cfg.adb_port = atoi(optarg); break;
		case 'A': adb_path = optarg; break;
		case 'D': cfg.want_dns = 0; break;
		case 'R': cfg.want_route = 0; break;
		case 'm': cfg.mtu_override = atoi(optarg); break;
		case '1': once = 1; break;
		case 'v': level = ET_LOG_DBG; break;
		case 'q': level = ET_LOG_ERR; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	log_set_level(level);

	if (geteuid() != 0) {
		log_err("must run as root (creating a tun/utun and setting routes are privileged)");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (!adb_server_alive(cfg.adb_port)) {
		const struct passwd *pw = console_user();

		if (!adb_path)
			adb_path = find_adb(pw);

		if (adb_start_server(cfg.adb_port, pw ? pw->pw_name : NULL, adb_path) != 0) {
			log_err("no ADB server on 127.0.0.1:%d and could not start one "
			        "with %s; run `adb start-server` yourself first",
			        cfg.adb_port, adb_path);
			return 1;
		}
	}

	/* A previous run killed with SIGKILL may have left resolver keys set. */
	if (cfg.want_dns)
		bridge_clear_stale_dns();

	make_mac(cfg.mac);
	log_info("host address on the tether link is %s", fmt_mac(cfg.mac));

	for (;;) {
		int reached_lease = 0;

		if (bridge_run_session(&cfg, &reached_lease) == 0)
			break;                  /* a stop was requested */
		if (once || bridge_stop_requested())
			return 1;

		if (reached_lease) {
			/* We had a working tether; this is a fresh problem. */
			backoff = 1;
			dead_tries = 0;
		} else if (++dead_tries >= 6) {
			/*
			 * Six connects in a row that never got as far as a lease.
			 * If the ADB USB interface is still there, adbd is just
			 * not ready yet (locked phone, USB mode not confirmed).
			 * Keep going.  Only exit when the cable is actually gone,
			 * so launchd/udev can start us on the next add.
			 */
			if (adb_usb_iface_present()) {
				log_info("ADB USB is present but the phone is not "
				         "ready; retrying");
				dead_tries = 3;
			} else {
				log_info("giving up until the phone is plugged in again");
				return 0;
			}
		}

		log_info("retrying in %ds", backoff);
		for (int i = 0; i < backoff * 10 && !bridge_stop_requested(); i++)
			usleep(100000);
		if (bridge_stop_requested())
			break;
		backoff = backoff < 15 ? backoff * 2 : 15;
	}

	log_info("stopped");
	return 0;
}
