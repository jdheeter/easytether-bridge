#include "utun.h"
#include "util.h"

#include <errno.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/uio.h>
#include <unistd.h>

int utun_open(struct utun *u, unsigned unit)
{
	struct ctl_info ci;
	struct sockaddr_ctl sc;
	socklen_t namelen;
	int fd;

	memset(u, 0, sizeof *u);

	fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
	if (fd < 0) {
		log_err("socket(PF_SYSTEM): %s", strerror(errno));
		return -1;
	}

	memset(&ci, 0, sizeof ci);
	strlcpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof ci.ctl_name);
	if (ioctl(fd, CTLIOCGINFO, &ci) < 0) {
		log_err("CTLIOCGINFO(%s): %s", UTUN_CONTROL_NAME, strerror(errno));
		close(fd);
		return -1;
	}

	memset(&sc, 0, sizeof sc);
	sc.sc_len = sizeof sc;
	sc.sc_family = AF_SYSTEM;
	sc.ss_sysaddr = AF_SYS_CONTROL;
	sc.sc_id = ci.ctl_id;
	sc.sc_unit = unit;              /* 0 == kernel picks; else utun(unit-1) */

	if (connect(fd, (struct sockaddr *)&sc, sizeof sc) < 0) {
		log_err("connect(utun unit %u): %s%s", unit, strerror(errno),
		        errno == EPERM ? " (are you root?)" : "");
		close(fd);
		return -1;
	}

	namelen = sizeof u->name;
	if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, u->name, &namelen) < 0) {
		log_err("UTUN_OPT_IFNAME: %s", strerror(errno));
		close(fd);
		return -1;
	}

	set_cloexec(fd);
	set_nonblock(fd);
	u->fd = fd;
	log_info("created %s", u->name);
	return 0;
}

void utun_close(struct utun *u)
{
	if (u->fd > 0)
		close(u->fd);
	u->fd = -1;
}

ssize_t utun_read(struct utun *u, uint8_t *buf, size_t cap, int *af)
{
	uint32_t proto;
	struct iovec iov[2];
	ssize_t n;

	iov[0].iov_base = &proto;
	iov[0].iov_len = sizeof proto;
	iov[1].iov_base = buf;
	iov[1].iov_len = cap;

	n = readv(u->fd, iov, 2);
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		log_err("read(%s): %s", u->name, strerror(errno));
		return -1;
	}
	if (n < (ssize_t)sizeof proto)
		return 0;

	*af = (int)ntohl(proto);
	return n - (ssize_t)sizeof proto;
}

ssize_t utun_write(struct utun *u, int af, const uint8_t *pkt, size_t len)
{
	uint32_t proto = htonl((uint32_t)af);
	struct iovec iov[2];
	ssize_t n;

	iov[0].iov_base = &proto;
	iov[0].iov_len = sizeof proto;
	iov[1].iov_base = (void *)pkt;
	iov[1].iov_len = len;

	n = writev(u->fd, iov, 2);
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR || errno == ENOBUFS)
			return 0;
		log_err("write(%s): %s", u->name, strerror(errno));
		return -1;
	}
	return n;
}
