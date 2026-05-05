/* log — see log.h for usage.
 *
 * Destination is chosen once at log_init():
 *   - debug mode  -> stderr (terminal / /dev/console)
 *   - normal mode -> /blackbox/system/system.log.<N>, rotated per boot
 *
 * Rotation (normal mode):
 *   slot number lives in /blackbox/system/now_index; at log_init() we
 *   read it, compute next = (prev % MAX_SLOTS) + 1, unlink the target
 *   slot, reopen fresh, persist the new slot number (atomic rename),
 *   and write a boot-marker line. Per-slot size cap stops runaway
 *   writes within a single boot.
 *
 * Thread safety:
 *   log_print() serialises under flockfile() on the output stream,
 *   which is set once during log_init() and never changed. Byte-count
 *   and "full" flag are also read/written only inside that lock.
 */

#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LOG_DIR        "/blackbox/system"
#define INDEX_FILE     LOG_DIR "/now_index"
#define MAX_SLOTS      10
#define PER_SLOT_CAP   (10UL * 1024UL * 1024UL)   /* 10 MiB per slot */

static log_level_t    g_min_level = LOG_LEVEL_I;
static FILE          *g_out       = NULL;    /* stderr after log_init */
static int            g_to_file   = 0;       /* 1 when g_out is our fopen'd file */
static int            g_slot      = 0;
static unsigned long  g_written   = 0;
static int            g_full      = 0;

void log_set_level(log_level_t level)
{
	g_min_level = level;
}

log_level_t log_get_level(void)
{
	return g_min_level;
}

static char level_char(log_level_t level)
{
	switch (level) {
	case LOG_LEVEL_E: return 'E';
	case LOG_LEVEL_W: return 'W';
	case LOG_LEVEL_I: return 'I';
	case LOG_LEVEL_D: return 'D';
	}
	return '?';
}

static int read_index(void)
{
	FILE *f = fopen(INDEX_FILE, "r");
	int idx = 0;
	if (f) {
		if (fscanf(f, "%d", &idx) != 1)
			idx = 0;
		fclose(f);
	}
	if (idx < 0 || idx > MAX_SLOTS)
		idx = 0;
	return idx;
}

/* Atomic replace so a crash mid-update never leaves now_index truncated. */
static void write_index(int idx)
{
	FILE *f = fopen(INDEX_FILE ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%d\n", idx);
	fclose(f);
	rename(INDEX_FILE ".tmp", INDEX_FILE);
}

int log_init(int debug)
{
	FILE *f;
	int   prev, slot;
	char  path[128];
	time_t now;
	struct tm lt;
	char  tbuf[32];

	/* Idempotent: second call is a no-op. */
	if (g_out)
		return 0;

	if (debug) {
		g_out       = stderr;
		g_to_file   = 0;
		g_min_level = LOG_LEVEL_D;
		return 0;
	}

	/* File mode. Create the directory first — /blackbox is a separate
	 * partition mounted by S21mountall.sh; we only need the sub-dir. */
	mkdir(LOG_DIR, 0755);

	prev = read_index();
	slot = prev + 1;
	if (slot > MAX_SLOTS) slot = 1;
	if (slot < 1)         slot = 1;

	snprintf(path, sizeof(path), "%s/system.log.%d", LOG_DIR, slot);
	unlink(path);

	f = fopen(path, "w");
	if (!f) {
		/* Can't open the file — degrade gracefully to stderr so
		 * early diagnostics are at least visible on the console. */
		fprintf(stderr,
		        "[log_init] fopen %s: %s — falling back to stderr\n",
		        path, strerror(errno));
		g_out     = stderr;
		g_to_file = 0;
		return -1;
	}

	/* Line buffering: every log line ends with '\n', so newline
	 * auto-triggers flush. Also cheaper than fflush() per message. */
	setvbuf(f, NULL, _IOLBF, 0);

	/* Publish the new slot BEFORE writing any real log line — so that
	 * even if we crash mid-boot, the next boot still advances to the
	 * next slot instead of rewriting the same one. */
	write_index(slot);

	now = time(NULL);
	localtime_r(&now, &lt);
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
	fprintf(f, "==== system_service boot slot=%d time=%s cap=%lu MiB ====\n",
	        slot, tbuf, PER_SLOT_CAP / (1024UL * 1024UL));
	fflush(f);

	g_out     = f;
	g_to_file = 1;
	g_slot    = slot;
	g_written = 0;
	g_full    = 0;
	return 0;
}

void log_shutdown(void)
{
	FILE *f = g_out;

	if (g_to_file && f) {
		fflush(f);
		fclose(f);
	}
	g_out     = NULL;
	g_to_file = 0;
	g_slot    = 0;
	g_written = 0;
	g_full    = 0;
}

void log_print(log_level_t level, const char *module, const char *fmt, ...)
{
	struct timeval tv;
	struct tm      lt;
	char           line[1280];
	int            n, wrote;
	FILE          *out;
	va_list        ap;

	if (level > g_min_level)
		return;
	if (!module) module = "?";
	if (!fmt)    fmt    = "";

	/* If log_init() hasn't run yet, fall back to stderr directly so
	 * nothing is lost. Shouldn't happen in a correct call order but
	 * we don't want to crash if it does. */
	out = g_out ? g_out : stderr;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &lt);

	n = snprintf(line, sizeof(line),
	             "[%02d:%02d:%02d.%03ld] [%c] [%s] ",
	             lt.tm_hour, lt.tm_min, lt.tm_sec,
	             (long)(tv.tv_usec / 1000),
	             level_char(level), module);
	if (n < 0) n = 0;
	if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);

	if (n < 0) n = 0;
	if (n >= (int)sizeof(line) - 1) n = (int)sizeof(line) - 2;
	line[n]     = '\n';
	line[n + 1] = '\0';
	wrote = n + 1;   /* bytes incl. trailing newline */

	/* Serialise across threads; also guards reads/writes of
	 * g_written and g_full which are only touched here. */
	flockfile(out);

	if (g_to_file && g_full) {
		funlockfile(out);
		return;
	}

	fputs(line, out);

	if (g_to_file) {
		g_written += (unsigned long)wrote;
		if (!g_full && g_written >= PER_SLOT_CAP) {
			fputs("==== log slot full — further messages dropped"
			      " until next boot ====\n", out);
			g_full = 1;
		}
		/* setvbuf(_IOLBF) already flushed on '\n'; no fflush. */
	} else {
		/* stderr may be fully buffered if redirected; force flush. */
		fflush(out);
	}

	funlockfile(out);
}
