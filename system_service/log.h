/* log — lightweight leveled + tagged logger for system_service.
 *
 * How to use from a .c file:
 *
 *     #define LOG_TAG "time_sync"    // module name shown in every line
 *     #include "log.h"
 *     ...
 *     LOGI("woke up, checking connectivity");
 *     LOGE("ntpd failed: %s", strerror(errno));
 *
 * Output format on stderr (one fputs per message, atomic across threads):
 *
 *     [HH:MM:SS.mmm] [I] [time_sync] woke up, checking connectivity
 *     [HH:MM:SS.mmm] [E] [time_sync] ntpd failed: ...
 *
 * Runtime filter: log_set_level(LOG_LEVEL_D) enables everything; the
 * default is LOG_LEVEL_I so LOGD() is off unless you opt in.
 *
 * If you don't set LOG_TAG before #include, the tag defaults to "?" so
 * the build still links — you'll see "[?]" in the log and know where.
 */

#ifndef SYSTEM_SERVICE_LOG_H
#define SYSTEM_SERVICE_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	LOG_LEVEL_E = 0,   /* error   — something broke */
	LOG_LEVEL_W = 1,   /* warning — recoverable oddity */
	LOG_LEVEL_I = 2,   /* info    — normal life-cycle events */
	LOG_LEVEL_D = 3,   /* debug   — verbose, off by default */
} log_level_t;

/* Initialise the logging backend. MUST be called from main() BEFORE
 * any worker thread is spawned. Any LOG* call made before log_init()
 * goes to stderr (fallback), which will NOT be captured in the log
 * file rotation — so don't log anything before this call.
 *
 *   debug != 0  -> output to stderr (the controlling terminal when
 *                  you run system_service by hand; /dev/console when
 *                  it is launched by S99system_service). Level raised
 *                  to LOG_LEVEL_D so LOGD() is also visible.
 *   debug == 0  -> output to /blackbox/system/system.log.<N> with
 *                  per-boot rotation: N = (prev % 10) + 1, wraps at
 *                  10 slots, current slot persisted in the sibling
 *                  file 'now_index'. A per-slot size cap prevents one
 *                  runaway boot from filling the partition.
 *
 * Returns 0 on success, -1 if the file backend could not be opened;
 * in that case the logger falls back to stderr so no messages are
 * silently dropped. */
int  log_init(int debug);

/* Flush and close the log file. Optional — SIGTERM from
 * start-stop-daemon will close fds anyway. Safe to call more than
 * once. */
void log_shutdown(void);

void        log_set_level(log_level_t level);
log_level_t log_get_level(void);

/* Low-level printer. Prefer the LOG* macros below so the module tag
 * comes from LOG_TAG automatically. */
void log_print(log_level_t level, const char *module, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#ifndef LOG_TAG
#define LOG_TAG "?"
#endif

#define LOGE(fmt, ...) log_print(LOG_LEVEL_E, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) log_print(LOG_LEVEL_W, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) log_print(LOG_LEVEL_I, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) log_print(LOG_LEVEL_D, LOG_TAG, fmt, ##__VA_ARGS__)

/* Escape hatch when you need to tag one call with a module name other
 * than the file's LOG_TAG (rare — e.g. a helper shared by two features). */
#define LOGE_TAG(tag, fmt, ...) log_print(LOG_LEVEL_E, (tag), fmt, ##__VA_ARGS__)
#define LOGW_TAG(tag, fmt, ...) log_print(LOG_LEVEL_W, (tag), fmt, ##__VA_ARGS__)
#define LOGI_TAG(tag, fmt, ...) log_print(LOG_LEVEL_I, (tag), fmt, ##__VA_ARGS__)
#define LOGD_TAG(tag, fmt, ...) log_print(LOG_LEVEL_D, (tag), fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif
