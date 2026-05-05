/* time_sync — set system timezone, probe Internet reachability, sync
 * system clock via busybox ntpd, persist to RTC. Retries every 30 s. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LOG_TAG "time_sync"
#include "log.h"

#include "time_sync.h"

/* Switch the system timezone at runtime by retargeting /etc/localtime
 * at /usr/share/zoneinfo/<tz_name>. Requires BR2_TARGET_TZ_INFO=y so
 * that the full zoneinfo database is installed on the rootfs.
 *
 * Already-running processes keep their old TZ until they call tzset()
 * again; new processes pick it up automatically. */
static int set_system_timezone(const char *tz_name)
{
	char zonepath[256];
	FILE *f;

	snprintf(zonepath, sizeof(zonepath), "/usr/share/zoneinfo/%s", tz_name);

	/* Guard against typos so we never end up with a dangling symlink. */
	if (access(zonepath, R_OK) != 0) {
		LOGE("timezone '%s' not found at %s: %s",
		     tz_name, zonepath, strerror(errno));
		return -1;
	}

	/* Atomic replace: symlink /etc/localtime.new then rename over
	 * /etc/localtime, so there is no window where /etc/localtime is
	 * missing for other processes that may look it up concurrently. */
	unlink("/etc/localtime.new");
	if (symlink(zonepath, "/etc/localtime.new") != 0) {
		LOGE("symlink /etc/localtime.new -> %s: %s",
		     zonepath, strerror(errno));
		return -1;
	}
	if (rename("/etc/localtime.new", "/etc/localtime") != 0) {
		LOGE("rename /etc/localtime: %s", strerror(errno));
		unlink("/etc/localtime.new");
		return -1;
	}

	/* Some tools (systemd, logrotate) read /etc/timezone as the canonical
	 * name of the active zone. Keep it in sync; failure here is non-fatal. */
	f = fopen("/etc/timezone", "w");
	if (f) {
		fprintf(f, "%s\n", tz_name);
		fclose(f);
	}

	setenv("TZ", tz_name, 1);
	tzset();

	LOGI("system timezone set to %s", tz_name);
	return 0;
}

/* Short TCP connect to a well-known public endpoint. 223.5.5.5:53 (AliDNS)
 * is used because it is reachable from mainland China where this board
 * is primarily deployed. */
static int check_internet(void)
{
	const char *hosts[] = { "223.5.5.5", "119.29.29.29", "114.114.114.114" };
	const int port = 53;
	const int timeout_sec = 3;
	size_t i;

	for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
		int sock, flags, ret, err;
		socklen_t len;
		struct sockaddr_in addr;
		fd_set wfds;
		struct timeval tv;

		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0)
			continue;

		flags = fcntl(sock, F_GETFL, 0);
		fcntl(sock, F_SETFL, flags | O_NONBLOCK);

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		inet_pton(AF_INET, hosts[i], &addr.sin_addr);

		ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
		if (ret == 0) {
			close(sock);
			return 1;
		}
		if (errno != EINPROGRESS) {
			close(sock);
			continue;
		}

		FD_ZERO(&wfds);
		FD_SET(sock, &wfds);
		tv.tv_sec = timeout_sec;
		tv.tv_usec = 0;
		ret = select(sock + 1, NULL, &wfds, NULL, &tv);
		if (ret <= 0) {
			close(sock);
			continue;
		}

		err = 0;
		len = sizeof(err);
		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
			close(sock);
			return 1;
		}
		close(sock);
	}
	return 0;
}

/* -q: quit after first successful update; -n: foreground so system() waits. */
static int sync_time_from_ntp(void)
{
	const char *cmds[] = {
		"ntpd -g -q -n -p ntp.aliyun.com -p ntp1.aliyun.com >/dev/null 2>&1",
		"ntpd -g -q -n -p cn.pool.ntp.org >/dev/null 2>&1",
		"ntpd -g -q -n -p pool.ntp.org >/dev/null 2>&1",
	};
	size_t i;

	system("killall -q ntpd");
	usleep(100000);

	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		if (system(cmds[i]) == 0)
			return 0;
	}
	return -1;
}

void *time_sync_thread(void *arg)
{
	(void)arg;

	set_system_timezone("Asia/Shanghai");

	for (;;) {
		if (!check_internet()) {
			LOGI("no Internet connectivity, skip time sync");
		} else {
			LOGI("Internet reachable, syncing via NTP...");
			if (sync_time_from_ntp() == 0) {
				time_t now = time(NULL);
				char tbuf[64];
				struct tm lt;
				localtime_r(&now, &lt);
				strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
				LOGI("system time updated to %s", tbuf);
				/* Persist wall clock back to the hardware RTC so
				 * the board keeps time across reboots without
				 * network. Ignore failure on boards without RTC. */
				if (system("hwclock -w >/dev/null 2>&1") != 0)
					LOGW("failed to write RTC");
			} else {
				LOGE("NTP sync failed");
			}
		}

		sleep(30);
	}

	return NULL;
}
