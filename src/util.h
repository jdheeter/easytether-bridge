/*
 * Small shared helpers: logging, a compacting byte FIFO, monotonic clock.
 */
#ifndef ET_UTIL_H
#define ET_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define ET_LOG_ERR  0
#define ET_LOG_WARN 1
#define ET_LOG_INFO 2
#define ET_LOG_DBG  3

void log_set_level(int level);
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_err(...)  log_msg(ET_LOG_ERR, __VA_ARGS__)
#define log_warn(...) log_msg(ET_LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(ET_LOG_INFO, __VA_ARGS__)
#define log_dbg(...)  log_msg(ET_LOG_DBG, __VA_ARGS__)

/*
 * Byte FIFO that compacts in place and grows up to a hard cap.  Used for the
 * partial reads and back-pressured writes on the ADB stream.
 */
struct sbuf {
	uint8_t *p;
	size_t   cap;  /* bytes allocated                   */
	size_t   max;  /* refuse to grow past this          */
	size_t   off;  /* read cursor within p              */
	size_t   len;  /* readable bytes at p + off         */
};

int      sbuf_init(struct sbuf *b, size_t initial, size_t max);
void     sbuf_free(struct sbuf *b);
int      sbuf_append(struct sbuf *b, const void *data, size_t n);
void     sbuf_consume(struct sbuf *b, size_t n);
/* Returns a pointer to at least n writable bytes, or NULL if the cap is hit. */
uint8_t *sbuf_reserve(struct sbuf *b, size_t n);
void     sbuf_commit(struct sbuf *b, size_t n);

static inline uint8_t *sbuf_data(struct sbuf *b) { return b->p + b->off; }

uint64_t now_ms(void);

/* Format a MAC or an IPv4 address (network byte order) into a static ring. */
const char *fmt_mac(const uint8_t mac[6]);
const char *fmt_ip(uint32_t be_addr);

int set_nonblock(int fd);
int set_cloexec(int fd);

/* Whole-buffer socket IO, retrying short transfers.  0 on success, -1 on error. */
int io_read_all(int fd, void *buf, size_t n);
int io_write_all(int fd, const void *buf, size_t n);

#endif /* ET_UTIL_H */
