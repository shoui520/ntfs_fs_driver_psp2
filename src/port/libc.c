/*
 * Minimal C library for libntfs-3g running inside a PS Vita kernel module.
 *
 * Only what libntfs-3g references is provided.  Everything that ends up in
 * SceSysclibForDriver (memcpy, strlen, snprintf, ...) is imported from there;
 * the rest lives here.  All libntfs-3g calls are made from the single ntfsfs
 * worker thread, so errno and the PRNG state are plain globals.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/reent.h>

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/rtc.h>

#include "../ntfsfs_log.h"

/* ---- heap ---------------------------------------------------------------- */

static SceUID g_heap = -1;

/* Every block carries its size so realloc() can copy. 8 bytes keeps alignment. */
typedef struct {
	size_t size;
	uint32_t magic;
} blk_hdr;

#define BLK_MAGIC 0x4e544653 /* 'NTFS' */

int ntfsfs_heap_init(void)
{
	SceKernelHeapCreateOpt opt;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_HEAP_ATTR_HAS_AUTO_EXTEND;
	g_heap = ksceKernelCreateHeap("SceNtfsfsHeap", 0x40000, &opt);
	return g_heap < 0 ? g_heap : 0;
}

void ntfsfs_heap_fini(void)
{
	if (g_heap >= 0)
		ksceKernelDeleteHeap(g_heap);
	g_heap = -1;
}

void *malloc(size_t size)
{
	blk_hdr *h;

	if (size > 0x7ffffff0) {
		errno = ENOMEM;
		return NULL;
	}
	h = ksceKernelAllocHeapMemory(g_heap, sizeof(*h) + (size ? size : 1));
	if (!h) {
		errno = ENOMEM;
		return NULL;
	}
	h->size = size;
	h->magic = BLK_MAGIC;
	return h + 1;
}

void free(void *p)
{
	blk_hdr *h;

	if (!p)
		return;
	h = (blk_hdr *)p - 1;
	if (h->magic != BLK_MAGIC) {
		NTFSFS_LOG("free: bad block %p\n", p);
		return;
	}
	h->magic = 0;
	ksceKernelFreeHeapMemory(g_heap, h);
}

void *calloc(size_t n, size_t size)
{
	void *p;

	if (size && n > (size_t)-1 / size) {
		errno = ENOMEM;
		return NULL;
	}
	p = malloc(n * size);
	if (p)
		memset(p, 0, n * size);
	return p;
}

void *realloc(void *p, size_t size)
{
	blk_hdr *h;
	void *n;

	if (!p)
		return malloc(size);
	if (!size) {
		free(p);
		return NULL;
	}
	h = (blk_hdr *)p - 1;
	if (h->size >= size) {
		h->size = size;
		return p;
	}
	n = malloc(size);
	if (!n)
		return NULL;
	memcpy(n, p, h->size);
	free(p);
	return n;
}

/* ---- errno / reent ------------------------------------------------------- */

static int g_errno;

int *__errno(void)
{
	return &g_errno;
}

/* newlib's stdout/stderr macros dereference _impure_ptr; give them a zeroed
 * reent so they evaluate to NULL FILE pointers, which our stdio ignores. */
static struct _reent g_reent;
struct _reent *_impure_ptr = &g_reent;

/* ---- string helpers missing from SceSysclibForDriver --------------------- */

char *strcpy(char *d, const char *s)
{
	char *r = d;

	while ((*d++ = *s++))
		;
	return r;
}

char *stpcpy(char *d, const char *s)
{
	while ((*d = *s++))
		d++;
	return d;
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}

int atoi(const char *s)
{
	return (int)strtol(s, NULL, 10);
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, 0x1000, fmt, ap);
	va_end(ap);
	return r;
}

char *strerror(int err)
{
	static char buf[24];

	snprintf(buf, sizeof(buf), "errno %d", err);
	return buf;
}

/* ---- stdio: route everything to the kernel debug log ---------------------- */

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	char buf[256];
	int r;

	(void)f;
	r = vsnprintf(buf, sizeof(buf), fmt, ap);
	ksceKernelPrintf("%s", buf);
	return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vfprintf(f, fmt, ap);
	va_end(ap);
	return r;
}

int fflush(FILE *f)
{
	(void)f;
	return 0;
}

/* ---- time ---------------------------------------------------------------- */

/* SceRtc ticks are microseconds since 0001-01-01; Unix epoch is 1970-01-01. */
#define RTC_UNIX_EPOCH_US 62135596800000000ULL

time_t time(time_t *t)
{
	SceRtcTick tick;
	time_t now = 0;

	if (ksceRtcGetCurrentTick(&tick) >= 0)
		now = (time_t)((tick.tick - RTC_UNIX_EPOCH_US) / 1000000ULL);
	if (t)
		*t = now;
	return now;
}

/* ---- locale / multibyte: libntfs-3g is forced to its built-in UTF-8 path - */

char *setlocale(int category, const char *locale)
{
	(void)category;
	(void)locale;
	return "C.UTF-8";
}

int __locale_mb_cur_max(void)
{
	return 1;
}

size_t mbstowcs(wchar_t *d, const char *s, size_t n)
{
	(void)d; (void)s; (void)n;
	errno = EILSEQ;
	return (size_t)-1;
}

int mbtowc(wchar_t *d, const char *s, size_t n)
{
	(void)d; (void)s; (void)n;
	errno = EILSEQ;
	return -1;
}

int wctomb(char *s, wchar_t wc)
{
	(void)s; (void)wc;
	errno = EILSEQ;
	return -1;
}

/* ---- POSIX identity / files: unused on the Vita, fail politely ------------ */

typedef unsigned int uid_t_;
typedef unsigned int gid_t_;

uid_t_ getuid(void) { return 0; }
gid_t_ getgid(void) { return 0; }
int getpid(void) { return 1; }

void *getpwnam(const char *n) { (void)n; return NULL; }
void *getgrnam(const char *n) { (void)n; return NULL; }
void *getpwuid(uid_t_ u) { (void)u; return NULL; }
void *getgrgid(gid_t_ g) { (void)g; return NULL; }

int open(const char *path, int flags, ...)
{
	(void)path; (void)flags;
	errno = ENOENT;
	return -1;
}

int read(int fd, void *buf, size_t n)
{
	(void)fd; (void)buf; (void)n;
	errno = EBADF;
	return -1;
}

int close(int fd)
{
	(void)fd;
	errno = EBADF;
	return -1;
}

/* ---- PRNG (used for security descriptor ids only) ------------------------- */

static uint32_t g_rand = 0x1234567;

long random(void)
{
	g_rand = g_rand * 1103515245u + 12345u;
	return (long)((g_rand >> 1) & 0x7fffffff);
}

void srandom(unsigned int seed)
{
	g_rand = seed;
}
