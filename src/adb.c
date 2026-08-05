#include "adb.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int connect_local(int port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&sa, 0, sizeof sa);
	sa.sin_len = sizeof sa;
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}

	/*
	 * The handshake below is blocking.  Without a timeout an ADB server that
	 * accepts the connection and then wedges would hang the daemon for good,
	 * out of reach of SIGTERM.
	 */
	{
		struct timeval tv = { .tv_sec = 10 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	}

	set_cloexec(fd);
	return fd;
}

/* Send a length-prefixed ADB host request. */
static int adb_send_request(int fd, const char *req)
{
	char hdr[5];
	size_t len = strlen(req);

	if (len > 0xffff)
		return -1;
	snprintf(hdr, sizeof hdr, "%04zx", len);
	if (io_write_all(fd, hdr, 4) < 0)
		return -1;
	return io_write_all(fd, req, len);
}

/*
 * Read the OKAY/FAIL status.  On FAIL the server sends a length-prefixed
 * reason, which is by far the most useful thing to show the user (it carries
 * "device unauthorized", "device offline", "no devices/emulators found").
 */
static int adb_read_status(int fd, char *err, size_t errlen)
{
	char status[4];

	if (io_read_all(fd, status, 4) < 0) {
		snprintf(err, errlen, "ADB server closed the connection");
		return -1;
	}
	if (memcmp(status, "OKAY", 4) == 0)
		return 0;

	if (memcmp(status, "FAIL", 4) == 0) {
		char lenbuf[5] = { 0 };
		unsigned long n;
		char msg[512];

		if (io_read_all(fd, lenbuf, 4) < 0) {
			snprintf(err, errlen, "ADB request failed (no reason given)");
			return -1;
		}
		n = strtoul(lenbuf, NULL, 16);
		if (n >= sizeof msg)
			n = sizeof msg - 1;
		if (n && io_read_all(fd, msg, n) < 0)
			n = 0;
		msg[n] = '\0';
		snprintf(err, errlen, "ADB: %s", n ? msg : "request failed");
		return -1;
	}

	snprintf(err, errlen, "unexpected ADB reply '%.4s'", status);
	return -1;
}

int adb_server_alive(int server_port)
{
	int fd = connect_local(server_port);
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

static int spawn_wait(const char *path, char *const argv[])
{
	pid_t pid;
	int status;
	posix_spawn_file_actions_t fa;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

	if (posix_spawn(&pid, path, &fa, NULL, argv, environ) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		return -1;
	}
	posix_spawn_file_actions_destroy(&fa);

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int adb_start_server(int server_port, const char *as_user, const char *adb_path)
{
	char portstr[16];

	if (adb_server_alive(server_port))
		return 0;

	snprintf(portstr, sizeof portstr, "%d", server_port);
	setenv("ANDROID_ADB_SERVER_PORT", portstr, 1);

	/*
	 * adb_path lands inside a single-quoted shell word below; a quote in it
	 * would break out of that word.  Such a path cannot be legitimate.
	 */
	if (strchr(adb_path, '\'') || strchr(adb_path, '\n')) {
		log_err("refusing to run an adb path containing a quote: %s", adb_path);
		return -1;
	}

	/*
	 * Only ever as a real user, never as root.  adb usually lives somewhere
	 * the admin group can write (/opt/homebrew/bin, an SDK under a home
	 * directory), and exec'ing that from a root daemon would hand root to
	 * anyone who can write it.  Running as the console user also means the
	 * server uses their ~/.android/adbkey, so the phone does not put up a
	 * second authorization prompt for root.
	 */
	if (!as_user || !*as_user) {
		log_err("no console user to start an ADB server as; run "
		        "`adb start-server` yourself and try again");
		return -1;
	}

	{
		char cmd[1024];
		char *const argv[] = { "/usr/bin/su", (char *)as_user, "-c", cmd, NULL };

		snprintf(cmd, sizeof cmd,
		         "ANDROID_ADB_SERVER_PORT=%d '%s' start-server", server_port, adb_path);
		log_info("starting ADB server as user %s", as_user);
		spawn_wait("/usr/bin/su", argv);
	}

	return adb_server_alive(server_port) ? 0 : -1;
}

int adb_open_stream(int server_port, const char *serial, char *err, size_t errlen)
{
	char transport[128];
	int fd, one = 1;

	fd = connect_local(server_port);
	if (fd < 0) {
		snprintf(err, errlen, "cannot reach ADB server on 127.0.0.1:%d (%s)",
		         server_port, strerror(errno));
		return -1;
	}

	if (serial && *serial)
		snprintf(transport, sizeof transport, "host:transport:%s", serial);
	else
		snprintf(transport, sizeof transport, "host:transport-any");

	if (adb_send_request(fd, transport) < 0) {
		snprintf(err, errlen, "cannot send ADB transport request: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (adb_read_status(fd, err, errlen) < 0) {
		close(fd);
		return -1;
	}

	if (adb_send_request(fd, ET_ADB_SERVICE) < 0) {
		snprintf(err, errlen, "cannot send ADB service request: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (adb_read_status(fd, err, errlen) < 0) {
		/*
		 * adbd refuses the OPEN when nothing is listening on the abstract
		 * socket, i.e. the EasyTether app is not running or USB tethering
		 * is not enabled inside it.
		 */
		size_t n = strlen(err);
		snprintf(err + n, errlen > n ? errlen - n : 0,
		         " -- is EasyTether running on the phone with USB tethering enabled?");
		close(fd);
		return -1;
	}

	{
		struct timeval none = { 0, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}
