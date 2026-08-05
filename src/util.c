#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_level = ET_LOG_INFO;

void log_set_level(int level)
{
	g_level = level;
}

void log_msg(int level, const char *fmt, ...)
{
	static const char *tag[] = { "error", "warn", "info", "debug" };
	va_list ap;
	struct timespec ts;
	struct tm tm;
	char stamp[32];

	if (level > g_level)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

	flockfile(stderr);
	fprintf(stderr, "%s.%03d [%s] ", stamp, (int)(ts.tv_nsec / 1000000), tag[level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	funlockfile(stderr);
	fflush(stderr);
}

int sbuf_init(struct sbuf *b, size_t initial, size_t max)
{
	memset(b, 0, sizeof *b);
	b->p = malloc(initial);
	if (!b->p)
		return -1;
	b->cap = initial;
	b->max = max;
	return 0;
}

void sbuf_free(struct sbuf *b)
{
	free(b->p);
	memset(b, 0, sizeof *b);
}

/* Make room for n more bytes past off+len, compacting or growing as needed. */
uint8_t *sbuf_reserve(struct sbuf *b, size_t n)
{
	if (b->cap - b->off - b->len >= n)
		return b->p + b->off + b->len;

	if (b->off) {
		memmove(b->p, b->p + b->off, b->len);
		b->off = 0;
		if (b->cap - b->len >= n)
			return b->p + b->len;
	}

	size_t want = b->len + n;
	if (want > b->max)
		return NULL;

	size_t ncap = b->cap ? b->cap : 1024;
	while (ncap < want)
		ncap *= 2;
	if (ncap > b->max)
		ncap = b->max;

	uint8_t *np = realloc(b->p, ncap);
	if (!np)
		return NULL;
	b->p = np;
	b->cap = ncap;
	return b->p + b->off + b->len;
}

void sbuf_commit(struct sbuf *b, size_t n)
{
	b->len += n;
}

int sbuf_append(struct sbuf *b, const void *data, size_t n)
{
	uint8_t *dst = sbuf_reserve(b, n);
	if (!dst)
		return -1;
	memcpy(dst, data, n);
	b->len += n;
	return 0;
}

void sbuf_consume(struct sbuf *b, size_t n)
{
	if (n >= b->len) {
		b->off = 0;
		b->len = 0;
		return;
	}
	b->off += n;
	b->len -= n;
	if (b->off > b->cap / 2) {
		memmove(b->p, b->p + b->off, b->len);
		b->off = 0;
	}
}

uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char *fmt_mac(const uint8_t mac[6])
{
	static char ring[4][20];
	static int i;
	char *s = ring[i++ & 3];
	snprintf(s, 20, "%02x:%02x:%02x:%02x:%02x:%02x",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return s;
}

const char *fmt_ip(uint32_t be_addr)
{
	static char ring[8][16];
	static int i;
	char *s = ring[i++ & 7];
	const uint8_t *b = (const uint8_t *)&be_addr;
	snprintf(s, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	return s;
}

int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int set_cloexec(int fd)
{
	int fl = fcntl(fd, F_GETFD, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

int io_read_all(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = read(fd, p, n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

int io_write_all(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (w == 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}
