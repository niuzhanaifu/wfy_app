/* uart_log — stream /dev/kmsg to /blackbox/uart_log/log.<N>.
 *
 * /dev/kmsg yields every kernel printk (== everything that goes to the
 * debug UART) including messages buffered from before we were launched.
 *
 * Per-boot rotation: slot = (prev % MAX_SLOTS) + 1, wipe that slot,
 * stream for the rest of the boot. index_now persists the current slot
 * so reboots resume the cycle instead of always overwriting log.1. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define LOG_TAG "uart_log"
#include "log.h"
#include "platform.h"

#include "uart_log.h"

#define LOG_DIR     SYSTEM_SERVICE_LOG_ROOT "/uart_log"
#define INDEX_FILE  LOG_DIR "/index_now"
#define MAX_SLOTS   20
#define SIZE_CAP    (50UL * 1024UL * 1024UL)  /* safety belt per slot */

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

/* Atomic replace so a crash mid-update never leaves index_now truncated. */
static void write_index(int idx)
{
	FILE *f = fopen(INDEX_FILE ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%d\n", idx);
	fclose(f);
	rename(INDEX_FILE ".tmp", INDEX_FILE);
}

void *uart_log_thread(void *arg)
{
	int prev, slot, outfd, kfd;
	char logpath[128];
	char buf[8192];
	unsigned long written = 0;

	(void)arg;

	system_service_mkdir_p(LOG_DIR, 0755);

#if SYSTEM_SERVICE_IS_A733
	if (geteuid() != 0) {
		LOGW("A733/Debian uart_log skipped: /dev/kmsg requires root or CAP_SYSLOG");
		return NULL;
	}
#endif

	prev = read_index();
	slot = prev + 1;
	if (slot > MAX_SLOTS)
		slot = 1;

	snprintf(logpath, sizeof(logpath), LOG_DIR "/log.%d", slot);
	unlink(logpath);

	outfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outfd < 0) {
		LOGE("open %s: %s", logpath, strerror(errno));
		return NULL;
	}

	/* Publish the new slot first thing, so even if we crash before the
	 * next read() the next boot advances correctly instead of looping
	 * on the same file. */
	write_index(slot);

	{
		char banner[256];
		time_t now = time(NULL);
		int n = snprintf(banner, sizeof(banner),
		                 "==== uart_log slot=%d boot_time=%ld (%s",
		                 slot, (long)now, ctime(&now));
		if (n > 0 && n < (int)sizeof(banner) - 6) {
			strcpy(banner + n - 1, ") ====\n");
			write(outfd, banner, strlen(banner));
		}
	}

	kfd = open("/dev/kmsg", O_RDONLY);
	if (kfd < 0) {
		LOGE("open /dev/kmsg: %s", strerror(errno));
		close(outfd);
		return NULL;
	}

	for (;;) {
		ssize_t r = read(kfd, buf, sizeof(buf));
		if (r > 0) {
			ssize_t w = 0;
			if (written >= SIZE_CAP) {
				/* Slot full — stop writing but keep the thread
				 * alive so the next reboot rotates cleanly. */
				continue;
			}
			while (w < r) {
				ssize_t n = write(outfd, buf + w, (size_t)(r - w));
				if (n < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				w += n;
			}
			written += (unsigned long)w;
			/* Flush every record — the whole point of a blackbox
			 * log is surviving a hard reboot. */
			fdatasync(outfd);
		} else if (r == 0) {
			usleep(100000);
		} else if (errno == EPIPE) {
			/* Kernel ring advanced past our position. Resume. */
			continue;
		} else if (errno == EINTR) {
			continue;
		} else {
			LOGE("read /dev/kmsg: %s", strerror(errno));
			break;
		}
	}

	close(kfd);
	close(outfd);
	return NULL;
}
