/* wifi_switch - boot-time STA/AP switch for the TAISHAN PAI board.
 *
 * Single-interface workflow:
 *   1. Read HOME_SSID / HOME_PASSWORD from /etc/taishan_home_wifi.conf.
 *   2. Wait for wlan0 to appear and the radio to accept a scan trigger.
 *   3. Try wlan0 as STA. If association + DHCP succeeds, stay STA-only.
 *   4. If STA fails, stop STA state and start hostapd + dnsmasq on wlan0.
 *   5. When BLE provisioning supplies credentials, stop AP on wlan0, try
 *      STA on wlan0, and restore AP only if STA fails.
 *
 * wlan1 is intentionally not created or used for AP/STA coexistence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>

#define LOG_TAG "wifi_switch"
#include "log.h"

#include "wifi_switch.h"

/* Serializes the actual switch between the boot-time thread and any
 * on-demand caller (e.g. BLE provisioning). The boot thread takes it with
 * a blocking lock (it can afford to wait); wifi_switch_run_now() takes it
 * with trylock so BLE can report WIFI_SWITCH_ERR_BUSY immediately rather
 * than hang the phone's GATT write. */
static pthread_mutex_t s_switch_mutex = PTHREAD_MUTEX_INITIALIZER;

static int enter_ap_mode(void);
static void exit_ap_mode(void);

#define HOME_WIFI_CONF "/etc/taishan_home_wifi.conf"
#define WPA_CONF       "/etc/wpa_supplicant.conf"
#define HOSTAPD_CONF   "/etc/hostapd.conf"
#define DNSMASQ_CONF   "/etc/dnsmasq.conf"
#define BAND_CONF      "/etc/wifi_band.conf"
#define STA_IFACE      "wlan0"
#define AP_IFACE       "wlan0"

typedef enum {
	BAND_24 = 0,
	BAND_5  = 1,
} band_t;

static const char *band_str(band_t b)
{
	return (b == BAND_5) ? "5G" : "2.4G";
}

/* ============================================================
 *   Small I/O helpers
 * ============================================================ */

/* Write NUL-terminated 'data' to 'path', truncating. 0/-1. */
static int write_file(const char *path, const char *data)
{
	FILE *f = fopen(path, "w");
	size_t len, wrote;
	int rc;

	if (!f) {
		LOGE("open %s for write: %s", path, strerror(errno));
		return -1;
	}
	len = strlen(data);
	wrote = fwrite(data, 1, len, f);
	rc = fclose(f);
	if (wrote != len || rc != 0) {
		LOGE("write %s: short write or close failed", path);
		return -1;
	}
	return 0;
}

/* Read whole file into 'buf', NUL-terminate. Returns bytes read or -1.
 * Silently truncates if the file is larger than buf_sz-1. */
static int read_file_all(const char *path, char *buf, size_t buf_sz)
{
	FILE *f;
	size_t n;

	if (buf_sz == 0) return -1;
	f = fopen(path, "r");
	if (!f) return -1;
	n = fread(buf, 1, buf_sz - 1, f);
	buf[n] = '\0';
	fclose(f);
	return (int)n;
}

/* system() wrapper. Returns the command's exit status (0 on success).
 * Non-zero is logged at DEBUG only; callers decide the severity since
 * several commands here (killall with no target, iptables -F with no
 * rules) are expected to occasionally fail and must not spam the log. */
static int exec_cmd(const char *cmdline)
{
	int rc;

	LOGD("EXEC: %s", cmdline);
	rc = system(cmdline);
	if (rc == -1) {
		LOGE("  system() failed for '%s': %s", cmdline, strerror(errno));
		return -1;
	}
	if (WIFEXITED(rc)) {
		int status = WEXITSTATUS(rc);
		if (status != 0)
			LOGD("  '%s' exited with %d", cmdline, status);
		return status;
	}
	LOGD("  '%s' did not exit normally (rc=0x%x)", cmdline, rc);
	return -1;
}

/* popen()-based capture of stdout. 'buf' is NUL-terminated, truncated to
 * buf_sz-1 if longer. Returns exit status (0 on success), -1 on spawn
 * failure. */
static int exec_capture(const char *cmdline, char *buf, size_t buf_sz)
{
	FILE *fp;
	size_t n;
	int rc;

	if (buf_sz == 0) return -1;
	buf[0] = '\0';

	LOGD("CAPTURE: %s", cmdline);
	fp = popen(cmdline, "r");
	if (!fp) {
		LOGE("popen failed for '%s': %s", cmdline, strerror(errno));
		return -1;
	}
	n = fread(buf, 1, buf_sz - 1, fp);
	buf[n] = '\0';
	rc = pclose(fp);
	if (rc == -1) return -1;
	if (WIFEXITED(rc)) return WEXITSTATUS(rc);
	return -1;
}

/* ============================================================
 *   Config parsers
 * ============================================================ */

static char *lstrip(char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	return s;
}

static char *rstrip(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
	                 s[n-1] == '\r' || s[n-1] == '\n')) {
		s[--n] = '\0';
	}
	return s;
}

/* Strip one layer of matching " or ' around s, in-place. */
static void strip_quotes(char *s)
{
	size_t n = strlen(s);
	if (n >= 2 && ((s[0] == '"'  && s[n-1] == '"') ||
	               (s[0] == '\'' && s[n-1] == '\''))) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

/* Load HOME_SSID / HOME_PASSWORD. Empty password is allowed (open AP). */
static int read_home_wifi(char *ssid, size_t ssid_sz,
                          char *pwd,  size_t pwd_sz)
{
	char buf[2048];
	char *saveptr = NULL;
	char *line;
	int have_ssid = 0;

	ssid[0] = '\0';
	pwd[0]  = '\0';

	if (read_file_all(HOME_WIFI_CONF, buf, sizeof(buf)) < 0) {
		LOGE("read %s: %s", HOME_WIFI_CONF, strerror(errno));
		return -1;
	}

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = rstrip(lstrip(line));
		char *eq, *k, *v;
		if (!*t || *t == '#') continue;
		eq = strchr(t, '=');
		if (!eq) continue;
		*eq = '\0';
		k = rstrip(t);
		v = lstrip(eq + 1);
		rstrip(v);
		strip_quotes(v);
		if (strcmp(k, "HOME_SSID") == 0) {
			snprintf(ssid, ssid_sz, "%s", v);
			have_ssid = 1;
		} else if (strcmp(k, "HOME_PASSWORD") == 0) {
			snprintf(pwd, pwd_sz, "%s", v);
		}
	}

	if (!have_ssid || ssid[0] == '\0') {
		LOGE("HOME_SSID missing or empty in %s", HOME_WIFI_CONF);
		return -1;
	}
	return 0;
}

/* Atomic file write: writes 'data' to '<path>.new', then renames over
 * '<path>'. If we crash mid-write, the original file stays intact. */
static int atomic_write_file(const char *path, const char *data)
{
	char tmp[512];
	int n = snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		LOGE("atomic_write: path too long: %s", path);
		return -1;
	}

	if (write_file(tmp, data) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		LOGE("atomic_write: rename %s -> %s: %s",
		     tmp, path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

int wifi_switch_persist_home_wifi(const char *ssid, const char *password)
{
	char body[1024];
	int n;

	if (!ssid || !*ssid) {
		LOGE("persist_home_wifi: empty SSID, refusing");
		return -1;
	}
	/* read_home_wifi() does strip_quotes() but not unescape — embedded
	 * '"' would round-trip incorrectly. Reject up front. */
	if (strchr(ssid, '"') || (password && strchr(password, '"'))) {
		LOGE("persist_home_wifi: SSID/password contains '\"' — "
		     "cannot encode safely, refusing");
		return -1;
	}

	n = snprintf(body, sizeof(body),
	    "# Taishan home WiFi credentials.\n"
	    "# Managed by system_service; rewritten when BLE provisioning succeeds.\n"
	    "HOME_SSID=\"%s\"\n"
	    "HOME_PASSWORD=\"%s\"\n",
	    ssid, password ? password : "");
	if (n < 0 || (size_t)n >= sizeof(body)) {
		LOGE("persist_home_wifi: rendered config exceeds buffer");
		return -1;
	}

	if (atomic_write_file(HOME_WIFI_CONF, body) != 0) return -1;

	LOGI("persist_home_wifi: %s updated (ssid='%s', pwd_len=%zu)",
	     HOME_WIFI_CONF, ssid, password ? strlen(password) : 0);
	return 0;
}

/* Missing/malformed -> 2.4G, matching S48ap's load_band(). */
static band_t read_band(void)
{
	/* 2 KiB: wifi_band.conf is heavily commented (~870 B), and a 256 B
	 * buffer would truncate before reaching the BAND= line at the bottom,
	 * silently falling back to 2.4G. Keep this comfortably oversized. */
	char buf[2048];
	char *saveptr = NULL;
	char *line;

	if (read_file_all(BAND_CONF, buf, sizeof(buf)) < 0) {
		LOGI("[read_band] %s missing/unreadable -> defaulting to 2.4G",
		     BAND_CONF);
		return BAND_24;
	}

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = rstrip(lstrip(line));
		char *eq, *k, *v;
		if (!*t || *t == '#') continue;
		eq = strchr(t, '=');
		if (!eq) continue;
		*eq = '\0';
		k = rstrip(t);
		v = lstrip(eq + 1);
		rstrip(v);
		strip_quotes(v);
		if (strcmp(k, "BAND") == 0) {
			band_t result;
			if (strcmp(v, "5G") == 0 || strcmp(v, "5g") == 0)
				result = BAND_5;
			else
				result = BAND_24;
			LOGI("[read_band] %s: BAND='%s' -> %s",
			     BAND_CONF, v, band_str(result));
			return result;
		}
	}
	LOGI("[read_band] %s: no BAND= line found -> defaulting to 2.4G",
	     BAND_CONF);
	return BAND_24;
}

/* Atomically rewrite /etc/wifi_band.conf so its BAND= line reflects 'band'.
 * Comments and unrelated keys are preserved (line-by-line copy). If BAND=
 * is missing the line is appended; if the whole file is missing, a minimal
 * one is created. No-op when the file already encodes the requested band,
 * which keeps repeated provisioning from churning flash needlessly.
 *
 * The file is also consumed by /etc/init.d/S48ap and app/myrust, so the
 * single-quoted shell-friendly form `BAND='5G'` is preserved on rewrite. */
static int persist_band(band_t band)
{
	char in[2048];
	char out[3072];
	size_t opos = 0;
	int replaced = 0;
	int have_file;
	char *saveptr = NULL;
	char *line;
	const char *new_v = (band == BAND_5) ? "5G" : "2.4G";

	if (read_band() == band) {
		LOGI("persist_band: %s already on %s, skipping rewrite",
		     BAND_CONF, new_v);
		return 0;
	}

	have_file = (read_file_all(BAND_CONF, in, sizeof(in)) >= 0);
	if (have_file) {
		for (line = strtok_r(in, "\n", &saveptr); line;
		     line = strtok_r(NULL, "\n", &saveptr)) {
			char *t = lstrip(line);
			int written;
			if (strncmp(t, "BAND=", 5) == 0 ||
			    strncmp(t, "BAND =", 6) == 0) {
				written = snprintf(out + opos, sizeof(out) - opos,
				                   "BAND='%s'\n", new_v);
				replaced = 1;
			} else {
				written = snprintf(out + opos, sizeof(out) - opos,
				                   "%s\n", line);
			}
			if (written < 0 || (size_t)written >= sizeof(out) - opos) {
				LOGE("persist_band: rewrite overflow");
				return -1;
			}
			opos += (size_t)written;
		}
	}
	if (!replaced) {
		int w = snprintf(out + opos, sizeof(out) - opos,
		                 "BAND='%s'\n", new_v);
		if (w < 0 || (size_t)w >= sizeof(out) - opos) {
			LOGE("persist_band: append overflow");
			return -1;
		}
		opos += (size_t)w;
	}

	if (atomic_write_file(BAND_CONF, out) != 0)
		return -1;
	LOGI("persist_band: %s -> BAND='%s'", BAND_CONF, new_v);
	return 0;
}

/* ============================================================
 *   Channel selection
 * ============================================================ */

static int freq_to_channel(unsigned int freq, band_t band)
{
	if (band == BAND_24) {
		if (freq >= 2412 && freq <= 2472) return (int)((freq - 2407) / 5);
		if (freq == 2484) return 14;
		return -1;
	}
	/* BAND_5 — skip DFS (52–144); accept UNII-1 and UNII-3 CN only. */
	if (freq >= 5180 && freq <= 5240) return (int)((freq - 5000) / 5);
	if (freq >= 5745 && freq <= 5825) return (int)((freq - 5000) / 5);
	return -1;
}

/* Scan on wlan0 up to 3 times and decide which band 'ssid' is actually on.
 * Does NOT trust /etc/wifi_band.conf — inspects every BSS in the scan and
 * classifies by frequency:
 *   2400..2499 MHz -> 2.4G
 *   5000..5999 MHz -> 5G
 * On hit fills *out_band / *out_ch and returns 0. When the same SSID is
 * visible on both bands (genuinely dual-band same-SSID router), 5G is
 * preferred so the SDK auto-picks the faster band. -1 means the SSID was
 * not seen at all in 3 attempts (either not in range, or the router is
 * not broadcasting it). */
static int scan_target_band(const char *ssid, band_t *out_band, int *out_ch)
{
	const size_t BUF_SZ = 256 * 1024;
	const int MAX_ATTEMPTS = 6;
	char *buf = malloc(BUF_SZ);
	int attempt;

	if (!buf) {
		LOGE("scan_target_band: OOM");
		return -1;
	}

	/* Force managed mode in case the radio is in a leftover state from a
	 * previous AP run — bcmdhd will silently refuse `iw scan` while the
	 * vif is still in __ap mode. Idempotent on a clean managed iface. */
	exec_cmd("iw dev " STA_IFACE " set type managed 2>/dev/null");

	/* Probe for radio readiness. Right after `ip link set up`, bcmdhd
	 * needs a couple of seconds before it accepts scan requests; until
	 * then `iw scan` returns instantly with "Network is down" /
	 * "Operation not supported" / "Device or resource busy", which
	 * looks identical to a clean miss if we don't capture stderr.
	 * Loop on `scan trigger` (cheap, async) until the kernel takes it. */
	for (attempt = 0; attempt < 5; attempt++) {
		char probe[256] = "";
		int rc = exec_capture("iw dev " STA_IFACE
		                      " scan trigger 2>&1",
		                      probe, sizeof(probe));
		if (rc == 0 || strstr(probe, "Device or resource busy"))
			break;
		LOGW("  scan_target_band: radio not ready yet "
		     "(rc=%d, msg=%.120s)", rc, probe);
		sleep(1);
	}
	/* Let the trigger we just fired (or the in-flight one) dwell on
	 * every channel — 5G has the most channels and is the slowest. */
	sleep(3);

	for (attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
		unsigned int cur_freq = 0;
		unsigned int hit_24   = 0;
		unsigned int hit_5    = 0;
		char *saveptr = NULL;
		char *line;
		int rc;

		/* 2>&1: keep the driver's actual error visible in 'buf' on
		 * failure, otherwise we'd parse an empty buffer and conclude
		 * the SSID isn't on air when really `iw` itself bailed. */
		rc = exec_capture("iw dev " STA_IFACE " scan 2>&1",
		                  buf, BUF_SZ);
		if (rc != 0 || !strstr(buf, "BSS ")) {
			char head[200];
			snprintf(head, sizeof(head), "%.196s", buf);
			LOGW("  scan attempt %d/%d unusable (rc=%d): %s",
			     attempt, MAX_ATTEMPTS, rc,
			     head[0] ? head : "(empty output)");
			if (attempt < MAX_ATTEMPTS) sleep(3);
			continue;
		}

		for (line = strtok_r(buf, "\n", &saveptr); line;
		     line = strtok_r(NULL, "\n", &saveptr)) {
			char *t;
			if (strncmp(line, "BSS ", 4) == 0) {
				cur_freq = 0;
				continue;
			}
			t = lstrip(line);
			if (strncmp(t, "freq:", 5) == 0) {
				cur_freq = (unsigned int)strtoul(lstrip(t + 5), NULL, 10);
			} else if (strncmp(t, "SSID:", 5) == 0) {
				char *rest = lstrip(t + 5);
				rstrip(rest);
				if (cur_freq != 0 && strcmp(rest, ssid) == 0) {
					if (cur_freq >= 2400 && cur_freq < 2500) {
						if (!hit_24) hit_24 = cur_freq;
					} else if (cur_freq >= 5000 && cur_freq < 6000) {
						if (!hit_5) hit_5 = cur_freq;
					}
				}
			}
		}

		if (hit_5 || hit_24) {
			band_t band       = hit_5 ? BAND_5 : BAND_24;
			unsigned int freq = hit_5 ? hit_5 : hit_24;
			int ch = freq_to_channel(freq, band);
			if (ch <= 0) {
				/* Frequency landed in a slot we don't whitelist
				 * (DFS / regdom). Refuse rather than try to
				 * associate on an unsupported channel. */
				LOGW("scan_target_band: '%s' on %u MHz is "
				     "outside our supported channel set "
				     "(DFS or regdom limit) — cannot use",
				     ssid, freq);
				free(buf);
				return -1;
			}
			LOGI("scan_target_band: '%s' on %s (%u MHz, ch%d)%s",
			     ssid, band_str(band), freq, ch,
			     (hit_5 && hit_24)
			         ? " [also seen on 2.4G — preferring 5G]" : "");
			*out_band = band;
			*out_ch   = ch;
			free(buf);
			return 0;
		}

		LOGW("  scan attempt %d/%d: '%s' not in scan results",
		     attempt, MAX_ATTEMPTS, ssid);
		if (attempt < MAX_ATTEMPTS) sleep(3);
	}

	free(buf);
	LOGE("scan_target_band: '%s' not seen on any band after %d scans "
	     "(out of range, hidden, router not broadcasting, or driver "
	     "rejected scan — check DEBUG log for actual `iw scan` error)",
	     ssid, MAX_ATTEMPTS);
	return -1;
}

/* hostapd's hw_mode value for a given band. Used both when reading
 * (to compare against the band the rest of the workflow has chosen)
 * and when rewriting the conf. */
static const char *band_to_hw_mode(band_t b)
{
	return (b == BAND_5) ? "a" : "g";
}

int wifi_switch_is_connected(void)
{
	char buf[8192];
	char *saveptr = NULL;
	char *line;
	int has_ipv4 = 0;

	/* `ip -4 addr show wlan0` enumerates the IPv4 addresses on the
	 * STA interface. A non-link-local inet line plus an associated
	 * cfg80211 link means assoc + DHCP both succeeded. */
	if (exec_capture("ip -4 addr show " STA_IFACE, buf, sizeof(buf)) != 0)
		return 0;

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = lstrip(line);
		if (strncmp(t, "inet ", 5) == 0) {
			char *p = lstrip(t + 5);
			/* 169.254.0.0/16 is the auto-config fallback when
			 * DHCP gave up — definitely NOT "connected to home
			 * wifi" from a user's point of view. */
			if (strncmp(p, "169.254.", 8) == 0)
				continue;
			has_ipv4 = 1;
			break;
		}
	}
	if (!has_ipv4)
		return 0;

	if (exec_capture("iw dev " STA_IFACE " link 2>/dev/null",
	                 buf, sizeof(buf)) != 0)
		return 0;
	return strstr(buf, "Connected to") ? 1 : 0;
}

int wifi_switch_get_ap_credentials(char *ssid_out, size_t ssid_sz,
                                   char *pwd_out,  size_t pwd_sz)
{
	char buf[8192];
	char *saveptr = NULL;
	char *line;
	int got_ssid = 0;

	if (ssid_sz) ssid_out[0] = '\0';
	if (pwd_sz)  pwd_out[0]  = '\0';

	if (read_file_all(HOSTAPD_CONF, buf, sizeof(buf)) < 0) {
		LOGE("get_ap_credentials: read %s: %s",
		     HOSTAPD_CONF, strerror(errno));
		return -1;
	}

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = lstrip(line);
		if (!*t || *t == '#') continue;

		/* Match "ssid=..." but not "ssid2=" or "bssid=". */
		if (strncmp(t, "ssid=", 5) == 0) {
			char *v = lstrip(t + 5);
			rstrip(v);
			snprintf(ssid_out, ssid_sz, "%s", v);
			got_ssid = 1;
		} else if (strncmp(t, "wpa_passphrase=", 15) == 0) {
			char *v = lstrip(t + 15);
			rstrip(v);
			snprintf(pwd_out, pwd_sz, "%s", v);
		}
	}

	if (!got_ssid) {
		LOGW("get_ap_credentials: no ssid= line in %s", HOSTAPD_CONF);
		return -1;
	}
	return 0;
}

/* Rewrite hostapd.conf so both `channel=` and `hw_mode=` agree with the
 * band the caller wants. Either or both lines may be missing — we append
 * what's needed. Without this BOTH-field rewrite hostapd silently exits
 * with "Configured channel (N) not found from the channel list of
 * current mode (2) IEEE 802.11a" whenever `hw_mode=a` is held over from
 * a previous 5G config but the home router is on a 2.4G channel (or the
 * other way round). */
static int rewrite_hostapd_channel(int new_ch, band_t band)
{
	char in[8192];
	char out[9216];
	size_t opos = 0;
	int replaced_ch = 0;
	int replaced_hw = 0;
	char *saveptr = NULL;
	char *line;
	const char *new_hw = band_to_hw_mode(band);

	if (read_file_all(HOSTAPD_CONF, in, sizeof(in)) < 0) {
		LOGE("read %s: %s", HOSTAPD_CONF, strerror(errno));
		return -1;
	}

	for (line = strtok_r(in, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = lstrip(line);
		int written;
		if (strncmp(t, "channel=", 8) == 0) {
			written = snprintf(out + opos, sizeof(out) - opos,
			                   "channel=%d\n", new_ch);
			replaced_ch = 1;
		} else if (strncmp(t, "hw_mode=", 8) == 0) {
			written = snprintf(out + opos, sizeof(out) - opos,
			                   "hw_mode=%s\n", new_hw);
			replaced_hw = 1;
		} else {
			written = snprintf(out + opos, sizeof(out) - opos,
			                   "%s\n", line);
		}
		if (written < 0 || (size_t)written >= sizeof(out) - opos) {
			LOGE("hostapd.conf rewrite overflow");
			return -1;
		}
		opos += (size_t)written;
	}

	if (!replaced_ch) {
		int w = snprintf(out + opos, sizeof(out) - opos,
		                 "channel=%d\n", new_ch);
		if (w < 0 || (size_t)w >= sizeof(out) - opos) {
			LOGE("hostapd.conf append overflow (channel)");
			return -1;
		}
		opos += (size_t)w;
	}
	if (!replaced_hw) {
		int w = snprintf(out + opos, sizeof(out) - opos,
		                 "hw_mode=%s\n", new_hw);
		if (w < 0 || (size_t)w >= sizeof(out) - opos) {
			LOGE("hostapd.conf append overflow (hw_mode)");
			return -1;
		}
		opos += (size_t)w;
	}

	return write_file(HOSTAPD_CONF, out);
}

/* ============================================================
 *   wpa_supplicant + association
 * ============================================================ */

static void escape_quotes(const char *s, char *dst, size_t dst_sz)
{
	size_t o = 0;
	size_t i;
	for (i = 0; s[i] && o + 2 < dst_sz; i++) {
		if (s[i] == '"') dst[o++] = '\\';
		dst[o++] = s[i];
	}
	dst[o] = '\0';
}

static int write_wpa_config(const char *ssid, const char *password)
{
	char ssid_esc[256], pwd_esc[256];
	char body[1024];
	band_t band = read_band();
	const char *freq_list = (band == BAND_5)
	    ? "5180 5200 5220 5240 5745 5765 5785 5805 5825"
	    : "2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462";
	int n;

	escape_quotes(ssid, ssid_esc, sizeof(ssid_esc));
	escape_quotes(password, pwd_esc, sizeof(pwd_esc));
	LOGI("write_wpa_config: limiting STA scan to %s frequencies",
	     band_str(band));

	if (password[0] == '\0') {
		n = snprintf(body, sizeof(body),
		    "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
		    "update_config=1\n"
		    "country=CN\n"
		    "\n"
		    "network={\n"
		    "    ssid=\"%s\"\n"
		    "    key_mgmt=NONE\n"
		    "    scan_ssid=1\n"
		    "    freq_list=%s\n"
		    "}\n",
		    ssid_esc, freq_list);
	} else {
		n = snprintf(body, sizeof(body),
		    "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
		    "update_config=1\n"
		    "country=CN\n"
		    "\n"
		    "network={\n"
		    "    ssid=\"%s\"\n"
		    "    psk=\"%s\"\n"
		    "    key_mgmt=WPA-PSK\n"
		    "    scan_ssid=1\n"
		    "    freq_list=%s\n"
		    "}\n",
		    ssid_esc, pwd_esc, freq_list);
	}
	if (n < 0 || (size_t)n >= sizeof(body)) {
		LOGE("wpa config too long for buffer");
		return -1;
	}
	LOGD("wpa config: %d bytes", n);
	return write_file(WPA_CONF, body);
}

static int is_associated(const char *iface)
{
	char cmd[128];
	char out[4096];
	snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", iface);
	if (exec_capture(cmd, out, sizeof(out)) < 0) return 0;
	return strstr(out, "Connected to") ? 1 : 0;
}

/* ============================================================
 *   Environment
 * ============================================================ */

static void disable_ipv6(void)
{
	LOGI("Disabling IPv6 globally");
	write_file("/proc/sys/net/ipv6/conf/all/disable_ipv6",     "1\n");
	write_file("/proc/sys/net/ipv6/conf/default/disable_ipv6", "1\n");
	write_file("/proc/sys/net/ipv6/conf/" STA_IFACE "/disable_ipv6", "1\n");
}

static void print_environment(void)
{
	const char *tools[] = { "wpa_supplicant", "udhcpc", "iw", "iptables", "ping" };
	char out[8192];
	size_t i;

	LOGI("--- Current environment ---");
	if (exec_capture("ip -br addr", out, sizeof(out)) == 0)
		LOGI("Network interfaces:\n%s", out);
	if (exec_capture("iw dev", out, sizeof(out)) == 0)
		LOGI("iw dev:\n%s", out);

	for (i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
		char cmd[96];
		snprintf(cmd, sizeof(cmd), "which %s 2>/dev/null", tools[i]);
		if (exec_capture(cmd, out, sizeof(out)) == 0 && out[0])
			LOGD("  tool '%s' : OK", tools[i]);
		else
			LOGW("  tool '%s' : NOT FOUND", tools[i]);
	}
	LOGI("--- End of environment ---");
}

/* ============================================================
 *   Workflow
 * ============================================================ */

/* Returns how many seconds hostapd has been running, or -1 if hostapd is
 * not running / its uptime can't be read. Used to skip the 5s "let it
 * stabilise" sleep when we know hostapd has already been up for ages
 * (typical case for BLE re-provisioning, where Phase B has been running
 * for minutes). Reads /proc/<pid>/stat field 22 (starttime in clock
 * ticks since boot) and subtracts from /proc/uptime. */
static int hostapd_uptime_seconds(void)
{
	char pid_buf[64];
	char stat_path[64];
	char stat_buf[1024];
	char uptime_buf[64];
	char *last_paren;
	char *p;
	int  pid;
	int  i;
	long ticks_per_sec;
	long long start_ticks;
	double uptime_s;

	if (exec_capture("pidof hostapd 2>/dev/null",
	                 pid_buf, sizeof(pid_buf)) != 0
	    || pid_buf[0] == '\0')
		return -1;
	pid = atoi(pid_buf);
	if (pid <= 0) return -1;

	snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
	if (read_file_all(stat_path, stat_buf, sizeof(stat_buf)) <= 0)
		return -1;

	/* /proc/<pid>/stat layout:
	 *   pid (comm) state ppid pgrp session ... starttime ...
	 * comm can in principle contain ')' so the canonical robust parse
	 * is to find the LAST ')' as the end of comm; everything after that
	 * is space-separated single-token fields. We need field 22
	 * (starttime), which is 19 fields past comm. */
	last_paren = strrchr(stat_buf, ')');
	if (!last_paren) return -1;
	p = last_paren + 1;
	for (i = 3; i < 22; i++) {
		while (*p == ' ') p++;
		while (*p && *p != ' ' && *p != '\n') p++;
	}
	while (*p == ' ') p++;
	start_ticks = strtoll(p, NULL, 10);
	if (start_ticks <= 0) return -1;

	if (read_file_all("/proc/uptime", uptime_buf, sizeof(uptime_buf)) <= 0)
		return -1;
	uptime_s = atof(uptime_buf);

	ticks_per_sec = sysconf(_SC_CLK_TCK);
	if (ticks_per_sec <= 0) return -1;

	return (int)(uptime_s - (double)start_ticks / (double)ticks_per_sec);
}

static int wait_for_system_ready(void)
{
	int i;

	LOGI("[INIT] Waiting for system readiness");

	LOGI("  [1/3] Waiting for wlan0 interface...");
	for (i = 1; i <= 30; i++) {
		if (access("/sys/class/net/" STA_IFACE, F_OK) == 0) {
			LOGI("  wlan0 present (after %ds)", i);
			break;
		}
		sleep(1);
		if (i == 30) {
			LOGE("wlan0 never appeared");
			return -1;
		}
	}

	LOGI("  [2/3] Waiting for WiFi radio ready...");
	for (i = 1; i <= 20; i++) {
		char out[256];
		int rc = exec_capture("iw dev " STA_IFACE " scan trigger 2>&1",
		                      out, sizeof(out));
		if (rc == 0 || strstr(out, "Device or resource busy")) {
			LOGI("  Radio ready (after %ds)", i);
			break;
		}
		sleep(1);
	}

	LOGI("  [3/3] Waiting for hostapd to stabilize...");
	{
		const int STABLE_WINDOW = 5;
		int up = hostapd_uptime_seconds();
		if (up < 0) {
			LOGI("  no hostapd, skipping");
		} else if (up >= STABLE_WINDOW) {
			/* Hostapd has been alive long enough that the
			 * channel/regdom/CSA shuffles at startup are
			 * definitely settled — typical case for BLE
			 * re-provisioning, where Phase B has been up
			 * for minutes already. Skip the sleep entirely. */
			LOGI("  hostapd up for %ds (>= %ds) — already stable, skipping wait",
			     up, STABLE_WINDOW);
		} else {
			int remaining = STABLE_WINDOW - up;
			LOGI("  hostapd up for %ds, waiting %ds more for stability",
			     up, remaining);
			sleep(remaining);
		}
	}

	LOGI("[INIT] System ready");
	return 0;
}

/* Extracts "192.168.x.y/24" from `ip -4 addr show wlan0` output. */
static int extract_inet4(const char *ip_show_out, char *out_ip, size_t out_ip_sz)
{
	char tmp[8192];
	char *saveptr = NULL;
	char *line;

	snprintf(tmp, sizeof(tmp), "%s", ip_show_out);

	for (line = strtok_r(tmp, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = lstrip(line);
		if (strncmp(t, "inet ", 5) == 0) {
			char *p = lstrip(t + 5);
			char *space = strchr(p, ' ');
			if (space) *space = '\0';
			snprintf(out_ip, out_ip_sz, "%s", p);
			return 0;
		}
	}
	return -1;
}

/* Probe wpa_supplicant to tell a wrong PSK apart from a generic failure.
 * The two signals we trust:
 *   - `wpa_cli status` sitting in 4WAY_HANDSHAKE long after we gave up on
 *     association means the AP accepted the assoc but rejected the PSK.
 *   - `wpa_cli list_networks` flagging the profile [TEMP-DISABLED] means
 *     wpa_supplicant auto-disabled it after repeated auth failures — again
 *     the canonical WRONG_KEY signature.
 * Everything else (never got to assoc, driver error, timeout in SCANNING,
 * ...) falls through to CONNECT_FAIL. */
static int field_value(const char *text, const char *key,
                       char *value, size_t value_sz)
{
	size_t key_len = strlen(key);
	const char *p = text;

	if (value_sz == 0) return -1;
	value[0] = '\0';

	while (p && *p) {
		const char *line_end = strchr(p, '\n');
		size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);

		if (line_len > key_len && strncmp(p, key, key_len) == 0
		    && p[key_len] == '=') {
			size_t n = line_len - key_len - 1;
			if (n >= value_sz) n = value_sz - 1;
			memcpy(value, p + key_len + 1, n);
			value[n] = '\0';
			return 0;
		}
		p = line_end ? line_end + 1 : NULL;
	}
	return -1;
}

static int scan_results_find_ssid(const char *scan, const char *ssid,
                                  unsigned int *out_freq,
                                  int *out_signal)
{
	char tmp[16384];
	char *saveptr = NULL;
	char *line;

	snprintf(tmp, sizeof(tmp), "%s", scan);
	for (line = strtok_r(tmp, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *freq_s;
		char *signal_s;
		char *flags_s;
		char *ssid_s;
		unsigned int freq;
		int signal;

		/* wpa_cli scan_results:
		 * bssid / frequency / signal level / flags / ssid
		 */
		freq_s = strchr(line, '\t');
		if (!freq_s) continue;
		*freq_s++ = '\0';
		signal_s = strchr(freq_s, '\t');
		if (!signal_s) continue;
		*signal_s++ = '\0';
		flags_s = strchr(signal_s, '\t');
		if (!flags_s) continue;
		*flags_s++ = '\0';
		ssid_s = strchr(flags_s, '\t');
		if (!ssid_s) continue;
		*ssid_s++ = '\0';

		if (strcmp(ssid_s, ssid) != 0)
			continue;

		freq = (unsigned int)strtoul(freq_s, NULL, 10);
		signal = (int)strtol(signal_s, NULL, 10);
		if (out_freq) *out_freq = freq;
		if (out_signal) *out_signal = signal;
		return 1;
	}
	return 0;
}

/* Probe wpa_supplicant and print a concrete association failure reason.
 * The return value stays compatible with the old helper: 1 means likely
 * bad PSK, 0 means generic connect failure. */
static int explain_sta_assoc_failure(const char *ssid)
{
	char status[4096];
	char networks[4096];
	char scans[16384];
	char wpa_state[64];
	int bad_psk = 0;

	exec_capture("wpa_cli -i " STA_IFACE " status 2>/dev/null",
	             status, sizeof(status));
	LOGI("  wpa_cli status after timeout:\n%s", status);
	if (field_value(status, "wpa_state", wpa_state, sizeof(wpa_state)) != 0)
		snprintf(wpa_state, sizeof(wpa_state), "UNKNOWN");
	if (strstr(status, "4WAY_HANDSHAKE"))
		bad_psk = 1;

	exec_capture("wpa_cli -i " STA_IFACE " list_networks 2>/dev/null",
	             networks, sizeof(networks));
	LOGI("  wpa_cli list_networks:\n%s", networks);
	if (strstr(networks, "TEMP-DISABLED"))
		bad_psk = 1;

	exec_capture("wpa_cli -i " STA_IFACE " scan_results 2>/dev/null",
	             scans, sizeof(scans));
	LOGI("  wpa_cli scan_results:\n%s", scans);

	if (bad_psk) {
		LOGE("  association failure reason: PSK rejected for '%s' "
		     "(wrong password or router rejected WPA handshake)",
		     ssid);
		return 1;
	}

	if (strcmp(wpa_state, "SCANNING") == 0) {
		unsigned int freq = 0;
		int signal = 0;

		if (scan_results_find_ssid(scans, ssid, &freq, &signal)) {
			LOGE("  association failure reason: '%s' was visible "
			     "at %u MHz (signal %d dBm), but wpa_supplicant "
			     "stayed in SCANNING; check band/freq_list, "
			     "regulatory domain, auth mode, or driver scan "
			     "filtering",
			     ssid, freq, signal);
		} else {
			LOGE("  association failure reason: '%s' was not found "
			     "in scan results while limited to the configured "
			     "band; check %s, router band/SSID broadcast, "
			     "signal strength, or DFS/regulatory channel",
			     ssid, BAND_CONF);
		}
	} else if (strcmp(wpa_state, "ASSOCIATING") == 0 ||
	           strcmp(wpa_state, "ASSOCIATED") == 0) {
		LOGE("  association failure reason: timed out in wpa_state=%s "
		     "before completing WPA handshake for '%s'",
		     wpa_state, ssid);
	} else if (strcmp(wpa_state, "DISCONNECTED") == 0) {
		LOGE("  association failure reason: wpa_supplicant is "
		     "DISCONNECTED from '%s'; check AP reachability, auth "
		     "mode, and driver messages",
		     ssid);
	} else {
		LOGE("  association failure reason: association timed out for "
		     "'%s' with wpa_state=%s",
		     ssid, wpa_state);
	}
	return 0;
}

static void connectivity_check(void)
{
	const char *targets[] = { "223.5.5.5", "114.114.114.114" };
	char buf[8192];
	size_t i;

	LOGI("[CHECK] Default route:");
	if (exec_capture("ip -4 route", buf, sizeof(buf)) == 0) {
		char *saveptr = NULL;
		char *line;
		for (line = strtok_r(buf, "\n", &saveptr); line;
		     line = strtok_r(NULL, "\n", &saveptr)) {
			if (strncmp(line, "default", 7) == 0)
				LOGI("  %s", line);
		}
	}

	LOGI("[CHECK] DNS config:");
	if (read_file_all("/etc/resolv.conf", buf, sizeof(buf)) >= 0) {
		char *saveptr = NULL;
		char *line;
		for (line = strtok_r(buf, "\n", &saveptr); line;
		     line = strtok_r(NULL, "\n", &saveptr)) {
			if (strstr(line, "nameserver"))
				LOGI("  %s", line);
		}
	} else {
		LOGW("  /etc/resolv.conf missing");
	}

	LOGI("[CHECK] IPv4 ping:");
	for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
		char cmd[128];
		snprintf(cmd, sizeof(cmd),
		         "ping -4 -c 2 -W 3 %s >/dev/null 2>&1", targets[i]);
		if (exec_cmd(cmd) == 0)
			LOGI("  %s reachable", targets[i]);
		else
			LOGW("  %s unreachable", targets[i]);
	}

	LOGI("[CHECK] DNS resolution:");
	if (exec_cmd("ping -4 -c 1 -W 3 www.baidu.com >/dev/null 2>&1") == 0)
		LOGI("  www.baidu.com resolves & reachable");
	else
		LOGW("  www.baidu.com failed");
}

/* ============================================================
 *   AP / STA-only primitives
 * ============================================================ */

/* hostapd alive? Cheap and sufficient — even if hostapd is up but AP is
 * mis-configured, we treat the daemon's presence as "AP intended to be
 * up" so we know to kill it later. */
static int is_ap_running(void)
{
	char out[64];
	if (exec_capture("pidof hostapd 2>/dev/null", out, sizeof(out)) != 0)
		return 0;
	return out[0] != '\0';
}

/* STA join used by both the boot path and BLE-triggered re-provisioning.
 * Auto-detects the band of the target SSID via a fresh scan, persists the
 * result to /etc/wifi_band.conf so freq_list (and any future AP fallback)
 * agrees with reality, then runs wpa_supplicant + udhcpc. */
static wifi_switch_status_t try_sta_only(const char *ssid,
                                          const char *password,
                                          char *out_ip, size_t out_ip_sz)
{
	int associated = 0;
	int i;
	char buf[8192];
	band_t target_band;
	int target_ch;

	out_ip[0] = '\0';

	LOGI("[STA-only 1/6] Killing any existing wpa_supplicant");
	exec_cmd("killall wpa_supplicant 2>/dev/null");
	exec_cmd("killall dnsmasq 2>/dev/null");
	exec_cmd("killall hostapd 2>/dev/null");
	exec_cmd("ip addr flush dev " STA_IFACE " 2>/dev/null");
	usleep(500 * 1000);

	LOGI("[STA-only 2/6] Bringing up " STA_IFACE);
	if (exec_cmd("ip link set " STA_IFACE " up") != 0)
		return WIFI_SWITCH_ERR_CONNECT_FAIL;

	/* Force regulatory domain to CN so 5G UNII-1/3 channels are
	 * unblocked for ACTIVE scanning and association. At cold boot
	 * cfg80211 often still has the world (00) regdom which marks
	 * UNII-1 (5170-5250 MHz, ch36-48) as passive-only + no-IR:
	 * wpa_supplicant can see the AP via passive beacons but can't
	 * TX auth frames, so it sits in SCANNING for the whole 30s
	 * timeout. Once any TX-capable client (e.g. our own hostapd
	 * AP fallback on ch36) has run since boot the regdom is CN
	 * already, which is why a manual restart of system_service a
	 * few minutes after boot connects fine even though the auto-
	 * boot run failed.
	 *
	 * NOTE: `iw reg set` is only a USER regulatory hint. If bcmdhd
	 * has already pushed its NVRAM ccode as a DRIVER hint, the user
	 * hint is silently ignored and we stay on `country 00`. We log
	 * iw reg get below so a failed STA join is diagnosable from the
	 * log alone — if you see `country: 00` here even after the set,
	 * the fix is bcmdhd-side (NVRAM ccode=CN, or `wl country CN`,
	 * or a module reload), not in this file. */
	{
		char out[256];
		int rc = exec_capture("iw reg set CN 2>&1", out, sizeof(out));
		if (rc != 0)
			LOGW("  iw reg set CN failed (rc=%d): %s",
			     rc, out[0] ? out : "(no output)");
	}
	usleep(300 * 1000);
	{
		char regdom[512];
		if (exec_capture("iw reg get 2>/dev/null",
		                 regdom, sizeof(regdom)) == 0) {
			char *p = strstr(regdom, "country ");
			if (p) {
				char *nl = strchr(p, '\n');
				if (nl) *nl = '\0';
				LOGI("  regdom now: %s", p);
			} else {
				LOGW("  iw reg get returned no country line");
			}
		}
	}

	/* Auto-band: scan first, decide whether the target is 2.4G or 5G,
	 * persist the choice to /etc/wifi_band.conf, THEN write the
	 * wpa_supplicant config. write_wpa_config() consults read_band()
	 * to populate freq_list, so it must run after persist_band(). Without
	 * this step, a stored 5G band but a 2.4G-only target SSID (or vice
	 * versa) leaves wpa_supplicant stuck in SCANNING. */
	LOGI("[STA-only 3/6] Detecting band of '%s' (scan)", ssid);
	if (scan_target_band(ssid, &target_band, &target_ch) != 0)
		return WIFI_SWITCH_ERR_NO_SSID;
	if (persist_band(target_band) != 0)
		LOGW("  could not persist band to %s — proceeding with "
		     "in-memory choice anyway", BAND_CONF);

	LOGI("[STA-only 4/6] Writing wpa_supplicant config to %s", WPA_CONF);
	if (write_wpa_config(ssid, password) != 0)
		return WIFI_SWITCH_ERR_UNKNOWN;

	LOGI("[STA-only 5/6] Starting wpa_supplicant");
	{
		/* `wpa_supplicant -B` daemonizes after parsing the config and
		 * opening the netlink/ctrl sockets, so any startup error (bad
		 * conf, missing binary, driver=nl80211 unsupported, ctrl_iface
		 * dir not writable, …) shows up on the parent's stderr before
		 * fork. Capture it via 2>&1 to surface the real reason. */
		char wpa_out[1024];
		char pid_out[64];
		int  rc, alive;

		rc = exec_capture("wpa_supplicant -B -i " STA_IFACE
		                  " -c " WPA_CONF " -D nl80211 2>&1",
		                  wpa_out, sizeof(wpa_out));

		/* Some wpa_supplicant builds (notably the Buildroot one shipped
		 * here) print "Successfully initialized wpa_supplicant" and then
		 * have the parent exit non-zero (typically 255) even though the
		 * detached daemon is up and running normally. Trust pidof over
		 * rc — if we see a live daemon, the start was effectively a
		 * success and we should proceed to wait for association. */
		usleep(200 * 1000);
		alive = (exec_capture("pidof wpa_supplicant 2>/dev/null",
		                      pid_out, sizeof(pid_out)) == 0
		         && pid_out[0] != '\0');
		if (!alive) {
			LOGE("  wpa_supplicant failed to start (rc=%d): %s",
			     rc, wpa_out[0] ? wpa_out : "(no output)");
			return WIFI_SWITCH_ERR_CONNECT_FAIL;
		}
		if (rc != 0) {
			LOGW("  wpa_supplicant -B exited rc=%d but daemon is "
			     "alive (pid=%s); proceeding. Output: %s",
			     rc, rstrip(pid_out),
			     wpa_out[0] ? wpa_out : "(none)");
		} else {
			LOGI("  wpa_supplicant daemon up (pid=%s)",
			     rstrip(pid_out));
		}
	}

	LOGI("  Waiting for association (up to 30s)...");
	for (i = 1; i <= 60; i++) {
		usleep(500 * 1000);
		if (is_associated(STA_IFACE)) {
			associated = 1;
			LOGI("  Associated after %dms", i * 500);
			break;
		}
		if (i % 10 == 0)
			LOGD("  still waiting (%ds)...", i / 2);
	}
	if (!associated) {
		int bad_psk = explain_sta_assoc_failure(ssid);
		LOGE("  STA-only association FAILED for '%s'%s", ssid,
		     bad_psk ? " (PSK rejected — wrong password?)" : "");
		return bad_psk ? WIFI_SWITCH_ERR_BAD_PASSWORD
		               : WIFI_SWITCH_ERR_CONNECT_FAIL;
	}
	exec_cmd("iw dev " STA_IFACE " link");

	LOGI("[STA-only 6/6] DHCP");
	if (exec_cmd("udhcpc -i " STA_IFACE " -q -n -t 10") != 0) {
		LOGE("  udhcpc failed");
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}

	if (exec_capture("ip -4 addr show " STA_IFACE, buf, sizeof(buf)) == 0)
		extract_inet4(buf, out_ip, out_ip_sz);
	if (out_ip[0] == '\0') {
		LOGE("  no IPv4 address on " STA_IFACE " after DHCP");
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}

	LOGI("[STA-only] DONE — IP=%s", out_ip);
	return WIFI_SWITCH_OK;
}

/* Bring up AP on wlan0 from scratch (or from an exit_ap_mode'd state).
 * Uses fallback channel for the configured band — at AP-fallback time we
 * have no STA target to align to, so this is just a "phone can find us"
 * default. */
static int enter_ap_mode(void)
{
	band_t band   = read_band();
	int    channel = (band == BAND_5) ? 36 : 6;
	char   out[64];

	LOGI("[AP-up] starting AP fallback, band=%s ch=%d",
	     band_str(band), channel);

	exec_cmd("killall wpa_supplicant 2>/dev/null");
	exec_cmd("killall hostapd 2>/dev/null");
	exec_cmd("killall dnsmasq 2>/dev/null");
	exec_cmd("ip addr flush dev " AP_IFACE " 2>/dev/null");
	usleep(500 * 1000);

	exec_cmd("ip link set " AP_IFACE " up");
	exec_cmd("ip addr flush dev " AP_IFACE);
	exec_cmd("ip addr add 192.168.50.1/24 dev " AP_IFACE);

	if (rewrite_hostapd_channel(channel, band) != 0) {
		LOGE("  hostapd.conf rewrite failed");
		return -1;
	}

	exec_cmd("killall hostapd 2>/dev/null");
	exec_cmd("killall dnsmasq 2>/dev/null");
	usleep(800 * 1000);

	if (exec_cmd("hostapd -B " HOSTAPD_CONF) != 0) {
		LOGE("  hostapd -B exec failed");
		return -1;
	}
	usleep(800 * 1000);
	if (exec_capture("pidof hostapd 2>/dev/null", out, sizeof(out)) != 0
	    || out[0] == '\0') {
		LOGE("  hostapd self-exited after start (likely hw_mode/"
		     "channel/regdom mismatch). Run 'hostapd -dd " HOSTAPD_CONF
		     "' on-target to see the cause.");
		return -1;
	}
	LOGI("  hostapd up on ch%d (%s mode), pid=%s",
	     channel, band_to_hw_mode(band), out);

	if (exec_cmd("dnsmasq -C " DNSMASQ_CONF) != 0)
		LOGW("  dnsmasq failed (clients can still associate but no DHCP)");

	LOGI("[AP-up] DONE");
	return 0;
}

/* Drop a marker file at /run/wifi_ready so the S99ovita_server init
 * script knows boot-time wifi setup has settled and it's safe to
 * spawn ovita-server. We touch this from BOTH success paths:
 *   - STA join succeeded (STA-only mode)
 *   - AP fallback up (provisioning available via BLE)
 * Path is on tmpfs so it disappears at reboot — every boot has to
 * earn its flag. Body has a one-line ASCII summary so the file is
 * also useful for `cat` debugging.
 *
 * Side effect: also disable WiFi power-save on wlan0. AP6256 / bcmdhd
 * defaults to power_save=on which adds 100-200ms beacon-sync stalls
 * mid-transfer; turning it off here gets us 10-30% more sustained
 * upload throughput. */
static void mark_network_ready(const char *mode, const char *detail)
{
	FILE *f;
	time_t now = time(NULL);
	struct tm lt;
	char ts[32];

	f = fopen("/run/wifi_ready", "w");
	if (!f) {
		LOGW("[ready] could not open /run/wifi_ready: %s", strerror(errno));
		return;
	}
	localtime_r(&now, &lt);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
	fprintf(f, "mode=%s detail=%s ts=%s\n",
	        mode, detail ? detail : "-", ts);
	fclose(f);
	LOGI("[ready] /run/wifi_ready: mode=%s detail=%s",
	     mode, detail ? detail : "-");

	/* TODO(power): leaving power_save off costs ~50-200 mW of extra
	 * idle current. Acceptable on wall-powered devices but bad for
	 * battery use-cases. When that becomes a constraint, switch to a
	 * dynamic toggle: turn off when an upload starts (signal from
	 * ovita-server), turn back on when N seconds idle after the last
	 * chunk arrives. The hooks for that signal aren't built yet —
	 * easiest path would be a new IPC cmd id + this helper. */
	if (exec_cmd("iw dev " STA_IFACE
	             " set power_save off 2>/dev/null") == 0) {
		LOGI("[ready] WiFi power_save disabled on %s "
		     "(throughput optimisation, costs ~50-200 mW idle)",
		     STA_IFACE);
	} else {
		LOGW("[ready] failed to disable WiFi power_save on %s "
		     "(throughput may be reduced)", STA_IFACE);
	}
}

/* Tear down AP cleanly so wlan0 can be reused as STA. */
static void exit_ap_mode(void)
{
	LOGI("[AP-down] tearing down AP (STA-only from now on)");

	exec_cmd("killall dnsmasq 2>/dev/null");
	exec_cmd("killall hostapd 2>/dev/null");

	exec_cmd("ip addr flush dev " AP_IFACE " 2>/dev/null");
	exec_cmd("ip link set "    AP_IFACE " down 2>/dev/null");

	LOGI("[AP-down] DONE");
}

/* Cold-boot priming for 5G STA on bcmdhd / AP6256.
 *
 * Empirically required: even with /lib/firmware/regulatory.db loaded
 * and NVRAM ccode=CN, a fresh boot cannot complete STA association on
 * 5G UNII-1/3 channels — wpa_supplicant sees the AP beacon at strong
 * signal but stays in SCANNING for the full 30 s timeout. The
 * underlying issue is bcmdhd-internal: the firmware's per-session 5G
 * TX-allowed list isn't initialised until the radio has operated as
 * AP master on a 5G channel at least once since module load. After
 * that priming, even on a different 5G channel, STA TX works.
 *
 * The cheapest userspace trigger is starting hostapd briefly on ch36.
 *
 * CRITICAL: do NOT call exit_ap_mode() here — that runs `ip link set
 * wlan0 down`, and the resulting link transition resets the very
 * firmware state we just primed. Instead, leave hostapd running. The
 * caller's next step (try_sta_only) opens with a `killall hostapd` +
 * `ip addr flush` that takes hostapd out cleanly WITHOUT touching the
 * administrative link state, so wlan0 stays continuously UP from
 * hostapd's tenure straight into wpa_supplicant's. This is exactly
 * the path that succeeds when `system_service` is restarted manually
 * after Phase B has left hostapd running. */
static void warm_regdom_via_hostapd(void)
{
	LOGI("[regdom-warm] starting hostapd to prime bcmdhd 5G TX "
	     "(left running; try_sta_only will reap it)");
	if (enter_ap_mode() != 0) {
		LOGW("[regdom-warm] enter_ap_mode failed; 5G STA may stall "
		     "in SCANNING");
		return;
	}
	/* Give NL80211_CMD_START_AP and bcmdhd's firmware-side init time
	 * to commit. On the verified-working path hostapd was up for ~3 s
	 * before STA took over; 2 s is comfortably inside that. */
	sleep(2);
	LOGI("[regdom-warm] DONE — hostapd left running for try_sta_only "
	     "to take over without bouncing wlan0");
}

static void recover_wifi_radio(void)
{
	LOGW("[RECOVER] resetting WiFi userspace and radio state");

	exec_cmd("killall wpa_supplicant 2>/dev/null");
	exec_cmd("killall hostapd 2>/dev/null");
	exec_cmd("killall dnsmasq 2>/dev/null");

	exec_cmd("ip addr flush dev " STA_IFACE " 2>/dev/null");
	exec_cmd("ip link set " STA_IFACE " down 2>/dev/null");
	usleep(500 * 1000);

	/* DHD HANG often powers the module down through rfkill. Toggle any
	 * rfkill nodes that exist; failures are harmless on images without
	 * rfkill sysfs entries. */
	exec_cmd("for f in /sys/class/rfkill/rfkill*/state; do "
	         "[ -e \"$f\" ] || continue; echo 0 > \"$f\" 2>/dev/null; "
	         "sleep 1; echo 1 > \"$f\" 2>/dev/null; done");
	sleep(2);

	exec_cmd("ip link set " STA_IFACE " up 2>/dev/null");
	sleep(1);
}

static void wifi_reconnect_monitor(void)
{
	int miss_count = 0;
	int retry_delay = 10;

	LOGI("[MONITOR] WiFi reconnect monitor started");

	while (1) {
		char ssid[128], password[128], ip[64];
		wifi_switch_status_t st;

		sleep(5);

		if (is_ap_running()) {
			miss_count = 0;
			continue;
		}

		if (wifi_switch_is_connected()) {
			if (miss_count != 0)
				LOGI("[MONITOR] WiFi connection is back");
			miss_count = 0;
			retry_delay = 10;
			continue;
		}

		miss_count++;
		LOGW("[MONITOR] WiFi disconnected or no IPv4 address (%d/2)",
		     miss_count);
		if (miss_count < 2)
			continue;

		if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
			LOGW("[MONITOR] switch already in progress, retry later");
			miss_count = 0;
			continue;
		}

		if (read_home_wifi(ssid, sizeof(ssid),
		                   password, sizeof(password)) != 0) {
			LOGW("[MONITOR] no stored STA credentials; keeping AP fallback");
			if (!is_ap_running())
				enter_ap_mode();
			pthread_mutex_unlock(&s_switch_mutex);
			miss_count = 0;
			sleep(30);
			continue;
		}

		LOGW("[MONITOR] attempting WiFi recovery and STA reconnect");
		recover_wifi_radio();
		st = try_sta_only(ssid, password, ip, sizeof(ip));
		if (st == WIFI_SWITCH_OK) {
			LOGI("[MONITOR] STA reconnect OK, IP=%s", ip);
			pthread_mutex_unlock(&s_switch_mutex);
			miss_count = 0;
			retry_delay = 10;
			continue;
		}

		LOGE("[MONITOR] STA reconnect failed, status=%d", (int)st);
		LOGW("[MONITOR] bringing AP fallback up for provisioning");
		enter_ap_mode();
		pthread_mutex_unlock(&s_switch_mutex);

		miss_count = 0;
		sleep(retry_delay);
		if (retry_delay < 60)
			retry_delay *= 2;
		if (retry_delay > 60)
			retry_delay = 60;
	}
}

/* ============================================================
 *   Thread entry
 * ============================================================ */

void *wifi_switch_thread(void *arg)
{
	char ssid[128], password[128], ip[64];
	wifi_switch_status_t st = WIFI_SWITCH_OK;
	int have_creds;
	int ap_up = 0;

	(void)arg;

	LOGI("=================================================");
	LOGI("  Taishan WiFi switch — STA-first, AP-fallback");
	LOGI("=================================================");

	have_creds = (read_home_wifi(ssid, sizeof(ssid),
	                             password, sizeof(password)) == 0);
	if (have_creds) {
		LOGI("  SSID    : %s", ssid);
		LOGI("  Password: %zu chars", strlen(password));
	} else {
		LOGW("  no credentials in %s — skipping STA, going straight to AP",
		     HOME_WIFI_CONF);
	}

	if (wait_for_system_ready() != 0) {
		LOGE("system not ready — thread exiting");
		return NULL;
	}
	disable_ipv6();
	print_environment();

	/* Blocking lock — boot path waits for any in-flight BLE-triggered
	 * switch (extremely unlikely this early, but cheap insurance). */
	pthread_mutex_lock(&s_switch_mutex);

	/* Cold-boot priming. Always required when target band is 5G on
	 * this bcmdhd build: a brief hostapd run is the only known trigger
	 * that initialises the firmware's 5G TX-allowed list for the
	 * current session. Without this Phase A reliably times out in
	 * SCANNING even though wpa_supplicant can see the AP beacon. 2.4G
	 * doesn't need it — ch1-11 are usable on world regdom directly. */
	if (have_creds && read_band() == BAND_5) {
		LOGI("[BOOT] target band is 5G — priming bcmdhd before Phase A");
		warm_regdom_via_hostapd();
	}

	/* ---- Phase A: try STA without bringing AP up ----
	 *
	 * Per spec: if STA succeeds at boot, we never start the AP at all
	 * (no SSID broadcast, no idle hostapd, no NAT chain). Only on
	 * failure do we fall through to Phase B and open the AP for BLE
	 * provisioning. */
	if (have_creds) {
		LOGI("[BOOT] Phase A — try STA-only with stored creds");
		st = try_sta_only(ssid, password, ip, sizeof(ip));
		if (st == WIFI_SWITCH_OK) {
			pthread_mutex_unlock(&s_switch_mutex);
			LOGI("[BOOT] SUCCESS — STA-only mode, IP=%s, AP NOT started", ip);
			mark_network_ready("STA", ip);
			LOGI("--- Running connectivity self-check ---");
			connectivity_check();
			LOGI("--- wifi_switch_thread monitoring STA connection ---");
			wifi_reconnect_monitor();
			return NULL;
		}
		LOGW("[BOOT] STA failed (status=%d) — kill STA and fall through to AP",
		     (int)st);
		exec_cmd("killall wpa_supplicant 2>/dev/null");
		usleep(500 * 1000);
	}

	/* ---- Phase B: bring up AP fallback ---- */
	LOGI("[BOOT] Phase B — bringing up AP fallback");
	ap_up = (enter_ap_mode() == 0);
	if (!ap_up) {
		LOGE("[BOOT] AP startup FAILED — device has no usable WiFi. "
		     "Investigate hostapd logs.");
	} else {
		LOGI("[BOOT] AP up. Awaiting BLE provisioning over NUS GATT to "
		     "switch back to STA.");
		mark_network_ready("AP", "192.168.50.1");
	}

	pthread_mutex_unlock(&s_switch_mutex);
	LOGI("--- wifi_switch_thread monitoring from AP active state ---");
	wifi_reconnect_monitor();
	return NULL;
}

/* Public on-demand entry. Callers (ble_gatt's CONNECT handler) MUST run
 * this on a worker thread — blocking the BLE D-Bus dispatcher here would
 * exceed BlueZ's 25 s WriteValue timeout.
 *
 * Per spec: on STA success we tear down the AP so the board ends up
 * STA-only. On STA failure the AP (if it was up) is left running so the
 * user can retry provisioning from the same phone session. */
wifi_switch_status_t wifi_switch_run_now(const char *ssid, const char *password)
{
	char file_ssid[128];
	char file_pwd[128];
	char ip[64];
	wifi_switch_status_t st;
	int  ap_was_up;

	/* Non-blocking: if another switch is already in flight (boot thread
	 * or a previous provisioning attempt) tell the caller so the phone
	 * sees a prompt "busy" response instead of a hung GATT write. */
	if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
		LOGW("wifi_switch_run_now: another switch is in progress — refusing");
		return WIFI_SWITCH_ERR_BUSY;
	}

	if (!ssid || !*ssid) {
		if (read_home_wifi(file_ssid, sizeof(file_ssid),
		                   file_pwd,  sizeof(file_pwd)) != 0) {
			pthread_mutex_unlock(&s_switch_mutex);
			return WIFI_SWITCH_ERR_UNKNOWN;
		}
		ssid = file_ssid;
		password = file_pwd;
	} else if (!password) {
		password = "";
	}

	LOGI("wifi_switch_run_now: ssid='%s' pwd_len=%zu",
	     ssid, strlen(password));

	/* Safety: the boot thread normally reaches readiness long before BLE
	 * provisioning arrives, but if someone hits CONNECT within the first
	 * seconds of boot we still need wlan0 + radio. wait_for_system_ready
	 * is idempotent. */
	if (wait_for_system_ready() != 0) {
		pthread_mutex_unlock(&s_switch_mutex);
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}
	disable_ipv6();

	ap_was_up = is_ap_running();

	if (ap_was_up) {
		/* AP is on the air on wlan0. We do NOT call exit_ap_mode
		 * here — that brings wlan0 administratively down, and the
		 * resulting link transition resets bcmdhd's per-session 5G
		 * priming. try_sta_only's step 1 already does
		 *   killall hostapd + killall dnsmasq + ip addr flush
		 * which takes the AP down cleanly WITHOUT touching link
		 * state, so wlan0 stays continuously UP from AP through STA.
		 * If the join fails we restore the AP below. */
		LOGI("AP currently running; try_sta_only will reap hostapd "
		     "without bouncing wlan0");
		st = try_sta_only(ssid, password, ip, sizeof(ip));
	} else {
		/* No AP up (boot Phase A succeeded earlier and we're just
		 * re-provisioning to a different SSID). If the new SSID is
		 * 5G but regdom is still world(00) — possible when the board
		 * has been STA-only on 2.4G since boot and never ran hostapd
		 * — proactively warm regdom so wpa_supplicant doesn't stall
		 * in SCANNING. The check is band-agnostic on purpose: scanning
		 * to learn the new SSID's band is what try_sta_only already
		 * does, but it's already too late by then. Cheap to just
		 * warm whenever regdom isn't CN. */
		LOGI("AP not running — using STA-only join");
		/* AP wasn't up, so we have no idea whether this session has
		 * already run hostapd on 5G. Prime defensively — cheap and
		 * always safe. (When the new SSID turns out to be 2.4G we
		 * waste ~3 s, but BLE provisioning is rare.) */
		warm_regdom_via_hostapd();
		st = try_sta_only(ssid, password, ip, sizeof(ip));
	}

	if (st == WIFI_SWITCH_OK) {
		LOGI("wifi_switch_run_now: STA up at %s", ip);
		mark_network_ready("STA", ip);
		/* Note: persisting creds to /etc/taishan_home_wifi.conf is
		 * the protocol layer's call (see wifi_switch.h). We do not
		 * write here so the API stays a pure operational primitive
		 * — protocol.c decides when a join is "trustworthy enough"
		 * to bake into the boot config. */
	} else {
		LOGE("wifi_switch_run_now: FAILED, status=%d",
		     (int)st);
		if (ap_was_up) {
			LOGW("  Restoring AP on wlan0 so provisioning can be retried");
			if (enter_ap_mode() != 0)
				LOGE("  AP restore failed after STA failure");
		}
	}

	pthread_mutex_unlock(&s_switch_mutex);
	return st;
}

wifi_switch_status_t wifi_switch_ensure_ap_mode(void)
{
	if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
		LOGW("wifi_switch_ensure_ap_mode: switch already in progress");
		return WIFI_SWITCH_ERR_BUSY;
	}

	if (is_ap_running()) {
		LOGI("wifi_switch_ensure_ap_mode: AP already running; no-op");
		pthread_mutex_unlock(&s_switch_mutex);
		return WIFI_SWITCH_OK;
	}

	LOGI("wifi_switch_ensure_ap_mode: forcing AP fallback mode");
	if (wait_for_system_ready() != 0) {
		pthread_mutex_unlock(&s_switch_mutex);
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}
	disable_ipv6();

	if (enter_ap_mode() != 0) {
		LOGE("wifi_switch_ensure_ap_mode: AP startup failed");
		pthread_mutex_unlock(&s_switch_mutex);
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}

	mark_network_ready("AP", "192.168.50.1");
	pthread_mutex_unlock(&s_switch_mutex);
	return WIFI_SWITCH_OK;
}

/* ============================================================
 *   WiFi scan (BLE cmd 0x08 backend)
 * ============================================================ */

/* Map a frequency in MHz to the band byte the BLE protocol expects. -1
 * for anything outside the 2.4 / 5 GHz ranges (we drop those entries). */
static int freq_to_scan_band(unsigned int freq)
{
	if (freq >= 2400 && freq < 2500) return WIFI_SCAN_BAND_24G;
	if (freq >= 5000 && freq < 6000) return WIFI_SCAN_BAND_5G;
	return -1;
}

/* Per-BSS parser state. Filled while we walk the iw-scan output for one
 * BSS block; flushed into a wifi_scan_entry_t at the next BSS header
 * (or end of buffer). */
typedef struct {
	uint8_t      ssid[WIFI_SCAN_SSID_MAX];
	uint8_t      ssid_len;
	int          rssi;          /* -127..0 typical                          */
	unsigned int freq;          /* MHz                                      */
	int          have_signal;
	int          have_freq;
	int          have_ssid;
	int          has_privacy;   /* Privacy bit in capability                */
	int          has_rsn;       /* "RSN:" section seen                      */
	int          has_wpa;       /* "WPA:" section seen                      */
	int          has_sae;       /* "SAE" anywhere in the BSS                */
	int          has_psk;       /* "PSK" auth suite anywhere                */
	int          has_eap;       /* EAP / 802.1X / Enterprise                */
} bss_parse_t;

static uint8_t derive_auth_type(const bss_parse_t *b)
{
	/* Order matters: SAE-only is WPA3, mixed PSK+SAE (transition mode)
	 * we report as WPA2-PSK so the phone picks the more-broadly-
	 * supported leg. EAP wins over PSK because Enterprise networks
	 * sometimes also list PSK suites alongside in confused vendor
	 * configs, and asking for PSK when the AP wants 802.1X just fails. */
	if (b->has_sae && !b->has_psk)            return WIFI_SCAN_AUTH_WPA3_SAE;
	if (b->has_eap)                           return WIFI_SCAN_AUTH_ENTERPRISE;
	if (b->has_rsn && (b->has_psk || b->has_sae))
	                                          return WIFI_SCAN_AUTH_WPA2_PSK;
	if (b->has_wpa && b->has_psk)             return WIFI_SCAN_AUTH_WPA_PSK;
	if (b->has_privacy)                       return WIFI_SCAN_AUTH_WEP;
	return WIFI_SCAN_AUTH_OPEN;
}

/* Decode iw's "SSID: <bytes-with-\xNN-escapes>" line into raw bytes. iw
 * prints control / non-ASCII octets as `\xNN` literal sequences. We need
 * the raw bytes back so the phone receives a UTF-8 SSID exactly as the
 * AP broadcasts it. Output truncates at 32 bytes (802.11 max). */
static uint8_t decode_iw_ssid(const char *src, uint8_t *dst, uint8_t dst_cap)
{
	uint8_t n = 0;
	while (*src && n < dst_cap) {
		if (src[0] == '\\' && src[1] == 'x'
		    && isxdigit((unsigned char)src[2])
		    && isxdigit((unsigned char)src[3])) {
			char hex[3] = { src[2], src[3], 0 };
			dst[n++] = (uint8_t)strtoul(hex, NULL, 16);
			src += 4;
		} else {
			dst[n++] = (uint8_t)*src++;
		}
	}
	return n;
}

/* Commit a finished BSS to *out_arr if we have a usable entry — must
 * have a non-empty SSID (hidden APs are dropped, the BLE protocol
 * requires ssid_len >= 1) and a freq we can map to a band. */
static void commit_bss(bss_parse_t *b,
                       wifi_scan_entry_t *out_arr,
                       size_t *out_count, size_t out_cap)
{
	int band;
	wifi_scan_entry_t *e;

	if (!b->have_ssid || b->ssid_len == 0)         goto reset;
	if (!b->have_freq)                              goto reset;
	band = freq_to_scan_band(b->freq);
	if (band < 0)                                   goto reset;
	if (*out_count >= out_cap)                      goto reset;

	e = &out_arr[(*out_count)++];
	memset(e, 0, sizeof(*e));
	memcpy(e->ssid, b->ssid, b->ssid_len);
	e->ssid_len  = b->ssid_len;
	e->rssi      = (int8_t)(b->have_signal ? b->rssi : -100);
	e->auth_type = derive_auth_type(b);
	e->band      = (uint8_t)band;

reset:
	memset(b, 0, sizeof(*b));
}

static int parse_iw_scan(const char *buf,
                         wifi_scan_entry_t *out_arr, size_t out_cap,
                         size_t *out_count)
{
	bss_parse_t cur;
	char *work;
	char *saveptr = NULL;
	char *line;

	*out_count = 0;
	memset(&cur, 0, sizeof(cur));

	work = strdup(buf);
	if (!work) return -1;

	for (line = strtok_r(work, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t;

		if (strncmp(line, "BSS ", 4) == 0) {
			commit_bss(&cur, out_arr, out_count, out_cap);
			continue;
		}

		t = lstrip(line);
		if (strncmp(t, "freq:", 5) == 0) {
			cur.freq = (unsigned int)strtoul(lstrip(t + 5), NULL, 10);
			cur.have_freq = 1;
		} else if (strncmp(t, "signal:", 7) == 0) {
			cur.rssi = (int)strtod(lstrip(t + 7), NULL);
			cur.have_signal = 1;
		} else if (strncmp(t, "SSID:", 5) == 0) {
			char *rest = lstrip(t + 5);
			rstrip(rest);
			cur.ssid_len = decode_iw_ssid(rest, cur.ssid,
			                              WIFI_SCAN_SSID_MAX);
			cur.have_ssid = 1;
		} else if (strncmp(t, "capability:", 11) == 0) {
			if (strstr(t + 11, "Privacy")) cur.has_privacy = 1;
		} else if (strncmp(t, "RSN:", 4) == 0) {
			cur.has_rsn = 1;
		} else if (strncmp(t, "WPA:", 4) == 0) {
			cur.has_wpa = 1;
		}

		/* Auth suite scanning is section-agnostic: we just look for
		 * the keywords anywhere inside the current BSS block, since
		 * the keyword choice (SAE / PSK / EAP) is unambiguous.
		 * "FT-PSK" / "PSK-SHA256" both contain "PSK" — fine. */
		if (strstr(t, "SAE"))     cur.has_sae = 1;
		if (strstr(t, "PSK"))     cur.has_psk = 1;
		if (strstr(t, "EAP")
		    || strstr(t, "802.1X")
		    || strstr(t, "Enterprise"))
			cur.has_eap = 1;
	}

	/* Flush the last BSS (no terminating "BSS " line). */
	commit_bss(&cur, out_arr, out_count, out_cap);

	free(work);
	return 0;
}

void wifi_switch_scan_free(wifi_scan_entry_t *entries)
{
	free(entries);
}

int wifi_switch_scan(wifi_scan_entry_t **out_entries, size_t *out_count)
{
	const size_t MAX_ENTRIES = 64;
	const size_t SCAN_BUF_SZ = 256 * 1024;
	wifi_scan_entry_t *arr = NULL;
	char *scan_buf = NULL;
	int rc = -1;
	int attempt;
	size_t count = 0;
	int ret = -1;

	if (!out_entries || !out_count) return -1;

	/* Don't fight an in-flight STA provisioning. trylock keeps the BLE
	 * dispatcher unblocked — the phone can retry. */
	if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
		LOGW("wifi_switch_scan: another switch in progress; refusing");
		return -1;
	}

	arr = calloc(MAX_ENTRIES, sizeof(*arr));
	scan_buf = malloc(SCAN_BUF_SZ);
	if (!arr || !scan_buf) {
		LOGE("wifi_switch_scan: OOM");
		goto out;
	}

	/* IMPORTANT: do NOT kill hostapd here. cfg80211 + bcmdhd handle
	 * concurrent (off-channel) scans while AP is active — the driver
	 * briefly leaves the home channel to dwell on each scan target,
	 * then comes back. The AP's beacon stream has small gaps but the
	 * phone's SSID list does NOT see the AP disappear (beacon period
	 * is ~100 ms; phones tolerate a missed beacon or two). Killing
	 * hostapd, in contrast, makes the SSID drop out of every nearby
	 * phone's WiFi list for the entire scan window — terrible UX
	 * during BLE provisioning since users assume the device crashed.
	 *
	 * Likewise we do NOT change the vif type or touch link state.
	 * The radio stays exactly as the caller left it (AP, STA, or
	 * idle managed) — `iw scan` works in all three. */
	LOGI("wifi_switch_scan: launching off-channel scan; AP keeps broadcasting");

	for (attempt = 1; attempt <= 3; attempt++) {
		rc = exec_capture("iw dev " STA_IFACE " scan 2>&1",
		                  scan_buf, SCAN_BUF_SZ);
		if (rc == 0 && strstr(scan_buf, "BSS "))
			break;
		LOGW("wifi_switch_scan: attempt %d/3 unusable (rc=%d): %.180s",
		     attempt, rc,
		     scan_buf[0] ? scan_buf : "(empty)");
		if (attempt < 3) sleep(2);
	}

	if (rc != 0 || !strstr(scan_buf, "BSS ")) {
		LOGE("wifi_switch_scan: no usable scan output after 3 tries");
		goto out;
	}

	if (parse_iw_scan(scan_buf, arr, MAX_ENTRIES, &count) != 0) {
		LOGE("wifi_switch_scan: parse_iw_scan failed");
		goto out;
	}

	LOGI("wifi_switch_scan: %zu visible network(s) (cap %zu)",
	     count, MAX_ENTRIES);
	ret = 0;

out:
	pthread_mutex_unlock(&s_switch_mutex);
	free(scan_buf);
	if (ret == 0) {
		*out_entries = arr;
		*out_count   = count;
	} else {
		free(arr);
	}
	return ret;
}
