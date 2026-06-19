/* wifi_switch_a733 - Debian/NetworkManager backend for A733.
 *
 * The public API matches wifi_switch.c so protocol/BLE code stays unchanged.
 * RK3566 keeps the original hostapd + wpa_supplicant implementation; this
 * file is selected only when Makefile is invoked with TARGET_PLATFORM=a733.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "wifi_switch"
#include "log.h"
#include "platform.h"
#include "wifi_switch.h"

#if !SYSTEM_SERVICE_IS_A733
#error "wifi_switch_a733.c must be compiled with SYSTEM_SERVICE_PLATFORM_A733=1"
#endif

#define HOME_WIFI_CONF SYSTEM_SERVICE_HOME_WIFI_CONF
#define HOSTAPD_CONF   SYSTEM_SERVICE_HOSTAPD_CONF
#define STA_IFACE      SYSTEM_SERVICE_WIFI_IFACE
#define NM_STA_CONN    SYSTEM_SERVICE_NM_STA_CONN
#define NM_AP_CONN     SYSTEM_SERVICE_NM_AP_CONN
#define AP_SSID        SYSTEM_SERVICE_AP_SSID
#define AP_PASSWD      SYSTEM_SERVICE_AP_PASSWD
#define AP_ADDR        "192.168.50.1"
#define AP_CIDR        "192.168.50.1/24"

#define WIFI_SSID_MAX      32
#define WIFI_PASSWD_MAX    63
#define SCAN_BUF_SZ        (256 * 1024)
#define MAX_SCAN_ENTRIES   64

static pthread_mutex_t s_switch_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *lstrip(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	return s;
}

static void rstrip(char *s)
{
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

static int exec_cmd(const char *cmdline)
{
	int rc, status;

	LOGD("EXEC: %s", cmdline);
	rc = system(cmdline);
	if (rc < 0) {
		LOGE("system('%s'): %s", cmdline, strerror(errno));
		return -1;
	}
	if (WIFEXITED(rc)) {
		status = WEXITSTATUS(rc);
		if (status != 0)
			LOGD("'%s' exited with %d", cmdline, status);
		return status;
	}
	LOGD("'%s' did not exit normally (rc=0x%x)", cmdline, rc);
	return -1;
}

static int exec_capture(const char *cmdline, char *buf, size_t buf_sz)
{
	FILE *fp;
	size_t off = 0;
	int rc;

	if (!buf || buf_sz == 0)
		return -1;
	buf[0] = '\0';

	LOGD("CAPTURE: %s", cmdline);
	fp = popen(cmdline, "r");
	if (!fp) {
		LOGE("popen('%s'): %s", cmdline, strerror(errno));
		return -1;
	}

	while (off + 1 < buf_sz) {
		size_t n = fread(buf + off, 1, buf_sz - off - 1, fp);
		off += n;
		if (n == 0)
			break;
	}
	buf[off] = '\0';

	rc = pclose(fp);
	if (rc < 0)
		return -1;
	if (WIFEXITED(rc))
		return WEXITSTATUS(rc);
	return -1;
}

static int shell_quote(const char *in, char *out, size_t cap)
{
	size_t pos = 0;

	if (!in || !out || cap < 3)
		return -1;
	out[pos++] = '\'';
	while (*in) {
		if (*in == '\'') {
			if (pos + 4 >= cap)
				return -1;
			memcpy(out + pos, "'\\''", 4);
			pos += 4;
			in++;
		} else {
			if (pos + 1 >= cap)
				return -1;
			out[pos++] = *in++;
		}
	}
	if (pos + 2 > cap)
		return -1;
	out[pos++] = '\'';
	out[pos] = '\0';
	return 0;
}

static int contains_ci(const char *haystack, const char *needle)
{
	size_t nlen, i;

	if (!haystack || !needle)
		return 0;
	nlen = strlen(needle);
	if (nlen == 0)
		return 1;

	for (i = 0; haystack[i]; i++) {
		size_t j;
		for (j = 0; j < nlen; j++) {
			unsigned char a = (unsigned char)haystack[i + j];
			unsigned char b = (unsigned char)needle[j];
			if (!a)
				return 0;
			if (tolower(a) != tolower(b))
				break;
		}
		if (j == nlen)
			return 1;
	}
	return 0;
}

static int read_file_all(const char *path, char *buf, size_t cap)
{
	FILE *f;
	size_t n;

	if (!buf || cap == 0)
		return -1;
	buf[0] = '\0';

	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, cap - 1, f);
	buf[n] = '\0';
	fclose(f);
	return 0;
}

static int atomic_write_file(const char *path, const char *body)
{
	char tmp[256];
	FILE *f;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		LOGE("atomic_write: path too long: %s", path);
		return -1;
	}

	f = fopen(tmp, "w");
	if (!f) {
		LOGE("open %s: %s", tmp, strerror(errno));
		return -1;
	}
	if (fputs(body, f) < 0 || fclose(f) != 0) {
		LOGE("write %s failed: %s", tmp, strerror(errno));
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		LOGE("rename %s -> %s: %s", tmp, path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void strip_quotes(char *s)
{
	size_t n;

	s = lstrip(s);
	rstrip(s);
	n = strlen(s);
	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
	               (s[0] == '\'' && s[n - 1] == '\''))) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

static int read_home_wifi(char *ssid, size_t ssid_sz,
                          char *password, size_t password_sz)
{
	char buf[2048];
	char *line, *saveptr = NULL;
	int have_ssid = 0;

	if (!ssid || !password || ssid_sz == 0 || password_sz == 0)
		return -1;
	ssid[0] = '\0';
	password[0] = '\0';

	if (read_file_all(HOME_WIFI_CONF, buf, sizeof(buf)) < 0) {
		LOGW("read %s: %s", HOME_WIFI_CONF, strerror(errno));
		return -1;
	}

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *eq, *key, *val;

		key = lstrip(line);
		if (*key == '#' || *key == '\0')
			continue;
		eq = strchr(key, '=');
		if (!eq)
			continue;
		*eq = '\0';
		val = lstrip(eq + 1);
		rstrip(key);
		strip_quotes(val);

		if (strcmp(key, "HOME_SSID") == 0) {
			snprintf(ssid, ssid_sz, "%s", val);
			have_ssid = (ssid[0] != '\0');
		} else if (strcmp(key, "HOME_PASSWORD") == 0) {
			snprintf(password, password_sz, "%s", val);
		}
	}

	return have_ssid ? 0 : -1;
}

int wifi_switch_persist_home_wifi(const char *ssid, const char *password)
{
	char body[512];

	if (!ssid || !ssid[0]) {
		LOGE("persist_home_wifi: empty SSID, refusing");
		return -1;
	}
	if (strchr(ssid, '"') || (password && strchr(password, '"'))) {
		LOGE("persist_home_wifi: double quote is not supported");
		return -1;
	}

	if (snprintf(body, sizeof(body),
	             "# Managed by system_service; rewritten when BLE provisioning succeeds.\n"
	             "HOME_SSID=\"%s\"\n"
	             "HOME_PASSWORD=\"%s\"\n",
	             ssid, password ? password : "") >= (int)sizeof(body)) {
		LOGE("persist_home_wifi: rendered config exceeds buffer");
		return -1;
	}

	if (atomic_write_file(HOME_WIFI_CONF, body) != 0)
		return -1;
	LOGI("persist_home_wifi: %s updated (ssid='%s', pwd_len=%zu)",
	     HOME_WIFI_CONF, ssid, password ? strlen(password) : 0);
	return 0;
}

static int command_exists(const char *tool)
{
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", tool);
	return exec_cmd(cmd) == 0;
}

static int wait_for_system_ready(void)
{
	char ifq[128], cmd[256];
	int i;

	if (!command_exists("nmcli")) {
		LOGE("A733 backend requires NetworkManager/nmcli");
		return -1;
	}
	if (!command_exists("iw")) {
		LOGE("A733 backend requires iw");
		return -1;
	}
	if (shell_quote(STA_IFACE, ifq, sizeof(ifq)) != 0)
		return -1;

	for (i = 0; i < 60; i++) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/class/net/%s", STA_IFACE);
		if (access(path, F_OK) == 0)
			break;
		LOGI("waiting for %s ... (%d/60)", STA_IFACE, i + 1);
		sleep(1);
	}
	if (i == 60) {
		LOGE("%s did not appear", STA_IFACE);
		return -1;
	}

	exec_cmd("nmcli radio wifi on >/dev/null 2>&1");
	snprintf(cmd, sizeof(cmd),
	         "nmcli device set %s managed yes >/dev/null 2>&1", ifq);
	exec_cmd(cmd);
	return 0;
}

static void mark_network_ready(const char *mode, const char *detail)
{
	FILE *f = fopen("/run/wifi_ready", "w");
	if (!f) {
		LOGW("could not write /run/wifi_ready: %s", strerror(errno));
		return;
	}
	fprintf(f, "mode=%s\n", mode ? mode : "?");
	fprintf(f, "detail=%s\n", detail ? detail : "");
	fclose(f);
}

static int parse_hostapd_credentials(char *ssid_out, size_t ssid_sz,
                                     char *pwd_out, size_t pwd_sz)
{
	char buf[2048];
	char *line, *saveptr = NULL;
	int have_ssid = 0;

	if (read_file_all(HOSTAPD_CONF, buf, sizeof(buf)) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = lstrip(line);
		rstrip(t);
		if (strncmp(t, "ssid=", 5) == 0) {
			snprintf(ssid_out, ssid_sz, "%s", t + 5);
			have_ssid = (ssid_out[0] != '\0');
		} else if (strncmp(t, "wpa_passphrase=", 15) == 0) {
			snprintf(pwd_out, pwd_sz, "%s", t + 15);
		}
	}

	return have_ssid ? 0 : -1;
}

int wifi_switch_get_ap_credentials(char *ssid_out, size_t ssid_sz,
                                   char *pwd_out,  size_t pwd_sz)
{
	if (!ssid_out || !pwd_out || ssid_sz == 0 || pwd_sz == 0)
		return -1;

	ssid_out[0] = '\0';
	pwd_out[0] = '\0';

	if (parse_hostapd_credentials(ssid_out, ssid_sz, pwd_out, pwd_sz) == 0)
		return 0;

	snprintf(ssid_out, ssid_sz, "%s", AP_SSID);
	snprintf(pwd_out, pwd_sz, "%s", AP_PASSWD);
	return ssid_out[0] ? 0 : -1;
}

static int nm_connection_active(const char *name)
{
	char out[4096];
	char *line, *saveptr = NULL;

	if (exec_capture("nmcli -t -f NAME connection show --active 2>/dev/null",
	                 out, sizeof(out)) != 0)
		return 0;

	for (line = strtok_r(out, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		if (strcmp(line, name) == 0)
			return 1;
	}
	return 0;
}

static void nm_down_connection(const char *name)
{
	char nq[256], cmd[512];
	if (shell_quote(name, nq, sizeof(nq)) != 0)
		return;
	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 10 connection down %s >/dev/null 2>&1", nq);
	exec_cmd(cmd);
}

static void nm_delete_connection(const char *name)
{
	char nq[256], cmd[512];
	if (shell_quote(name, nq, sizeof(nq)) != 0)
		return;
	snprintf(cmd, sizeof(cmd),
	         "nmcli connection delete %s >/dev/null 2>&1", nq);
	exec_cmd(cmd);
}

static int get_iface_ipv4(char *ip_out, size_t ip_sz)
{
	char ifq[128], cmd[256], out[1024];
	char *p, *slash;
	size_t n;

	if (!ip_out || ip_sz == 0)
		return -1;
	ip_out[0] = '\0';
	if (shell_quote(STA_IFACE, ifq, sizeof(ifq)) != 0)
		return -1;

	snprintf(cmd, sizeof(cmd),
	         "ip -4 -o addr show dev %s scope global 2>/dev/null", ifq);
	if (exec_capture(cmd, out, sizeof(out)) != 0 || !out[0])
		return -1;

	p = strstr(out, " inet ");
	if (!p)
		return -1;
	p += 6;
	slash = strchr(p, '/');
	if (!slash)
		return -1;
	n = (size_t)(slash - p);
	if (n >= ip_sz)
		n = ip_sz - 1;
	memcpy(ip_out, p, n);
	ip_out[n] = '\0';
	return ip_out[0] ? 0 : -1;
}

int wifi_switch_is_connected(void)
{
	char ip[64];

	if (nm_connection_active(NM_AP_CONN))
		return 0;
	if (get_iface_ipv4(ip, sizeof(ip)) != 0)
		return 0;
	return 1;
}

static uint8_t decode_iw_ssid(const char *src, uint8_t *dst, uint8_t dst_cap)
{
	uint8_t n = 0;
	while (*src && n < dst_cap) {
		if (src[0] == '\\' && src[1] == 'x' &&
		    isxdigit((unsigned char)src[2]) &&
		    isxdigit((unsigned char)src[3])) {
			char hex[3] = { src[2], src[3], 0 };
			dst[n++] = (uint8_t)strtoul(hex, NULL, 16);
			src += 4;
		} else {
			dst[n++] = (uint8_t)*src++;
		}
	}
	return n;
}

static int target_visible_in_scan(const char *ssid)
{
	char ifq[128], cmd[256];
	char *buf, *line, *saveptr = NULL;
	size_t ssid_len;
	int visible = 0;

	if (!ssid || !ssid[0])
		return 0;
	ssid_len = strlen(ssid);
	if (ssid_len > WIFI_SCAN_SSID_MAX)
		return 0;

	if (shell_quote(STA_IFACE, ifq, sizeof(ifq)) != 0)
		return 0;
	snprintf(cmd, sizeof(cmd), "iw dev %s scan 2>&1", ifq);

	buf = malloc(SCAN_BUF_SZ);
	if (!buf)
		return 0;
	if (exec_capture(cmd, buf, SCAN_BUF_SZ) != 0 || !strstr(buf, "BSS ")) {
		free(buf);
		return 0;
	}

	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {
		char *t = lstrip(line);
		if (strncmp(t, "SSID:", 5) == 0) {
			uint8_t raw[WIFI_SCAN_SSID_MAX];
			char *rest = lstrip(t + 5);
			uint8_t n;
			rstrip(rest);
			n = decode_iw_ssid(rest, raw, sizeof(raw));
			if ((size_t)n == ssid_len &&
			    memcmp(raw, ssid, ssid_len) == 0) {
				visible = 1;
				break;
			}
		}
	}

	free(buf);
	return visible;
}

static wifi_switch_status_t ensure_ap_mode_locked(void);

wifi_switch_status_t wifi_switch_run_now(const char *ssid, const char *password)
{
	char file_ssid[WIFI_SSID_MAX + 1];
	char file_pwd[WIFI_PASSWD_MAX + 1];
	char ssidq[256], pwdq[512], ifq[128], connq[256], cmd[1400], out[4096];
	char ip[64];
	const char *use_ssid = ssid;
	const char *use_pwd = password ? password : "";
	int rc, i;
	wifi_switch_status_t st = WIFI_SWITCH_ERR_UNKNOWN;

	if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
		LOGW("wifi_switch_run_now: another switch is in progress");
		return WIFI_SWITCH_ERR_BUSY;
	}

	if (!use_ssid || !use_ssid[0]) {
		if (read_home_wifi(file_ssid, sizeof(file_ssid),
		                   file_pwd, sizeof(file_pwd)) != 0) {
			st = WIFI_SWITCH_ERR_NO_SSID;
			goto out_unlock;
		}
		use_ssid = file_ssid;
		use_pwd = file_pwd;
	}

	LOGI("A733 WiFi connect: ssid='%s' pwd_len=%zu",
	     use_ssid, strlen(use_pwd));

	if (wait_for_system_ready() != 0)
		goto out_unlock;

	if (shell_quote(use_ssid, ssidq, sizeof(ssidq)) != 0 ||
	    shell_quote(use_pwd, pwdq, sizeof(pwdq)) != 0 ||
	    shell_quote(STA_IFACE, ifq, sizeof(ifq)) != 0 ||
	    shell_quote(NM_STA_CONN, connq, sizeof(connq)) != 0)
		goto out_unlock;

	nm_down_connection(NM_AP_CONN);
	nm_delete_connection(NM_STA_CONN);

	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 10 device wifi rescan ifname %s >/dev/null 2>&1", ifq);
	exec_cmd(cmd);
	sleep(1);

	if (!target_visible_in_scan(use_ssid)) {
		LOGE("A733 WiFi connect: target SSID '%s' not visible", use_ssid);
		st = WIFI_SWITCH_ERR_NO_SSID;
		ensure_ap_mode_locked();
		goto out_unlock;
	}

	if (use_pwd[0]) {
		snprintf(cmd, sizeof(cmd),
		         "nmcli -w 70 device wifi connect %s password %s ifname %s name %s 2>&1",
		         ssidq, pwdq, ifq, connq);
	} else {
		snprintf(cmd, sizeof(cmd),
		         "nmcli -w 70 device wifi connect %s ifname %s name %s 2>&1",
		         ssidq, ifq, connq);
	}

	rc = exec_capture(cmd, out, sizeof(out));
	if (rc != 0) {
		LOGE("A733 nmcli connect failed rc=%d: %.300s", rc, out);
		if (contains_ci(out, "password") ||
		    contains_ci(out, "secret") ||
		    contains_ci(out, "secrets"))
			st = WIFI_SWITCH_ERR_BAD_PASSWORD;
		else
			st = WIFI_SWITCH_ERR_CONNECT_FAIL;
		ensure_ap_mode_locked();
		goto out_unlock;
	}

	snprintf(cmd, sizeof(cmd),
	         "nmcli connection modify %s connection.autoconnect yes >/dev/null 2>&1",
	         connq);
	exec_cmd(cmd);

	for (i = 0; i < 30; i++) {
		if (wifi_switch_is_connected() &&
		    get_iface_ipv4(ip, sizeof(ip)) == 0) {
			LOGI("A733 WiFi connect OK: %s", ip);
			mark_network_ready("STA", ip);
			st = WIFI_SWITCH_OK;
			goto out_unlock;
		}
		sleep(1);
	}

	LOGE("A733 WiFi connect: no IPv4 address after nmcli success");
	st = WIFI_SWITCH_ERR_CONNECT_FAIL;
	ensure_ap_mode_locked();

out_unlock:
	pthread_mutex_unlock(&s_switch_mutex);
	return st;
}

static wifi_switch_status_t ensure_ap_mode_locked(void)
{
	char ssid[128], pwd[128];
	char ssidq[256], pwdq[256], ifq[128], apq[256], staq[256];
	char cmd[1400], out[4096];
	int rc;

	if (wait_for_system_ready() != 0)
		return WIFI_SWITCH_ERR_UNKNOWN;

	if (wifi_switch_get_ap_credentials(ssid, sizeof(ssid), pwd, sizeof(pwd)) != 0) {
		snprintf(ssid, sizeof(ssid), "%s", AP_SSID);
		snprintf(pwd, sizeof(pwd), "%s", AP_PASSWD);
	}
	if (strlen(pwd) < 8) {
		LOGW("A733 AP password too short; using built-in fallback");
		snprintf(pwd, sizeof(pwd), "%s", AP_PASSWD);
	}

	if (shell_quote(ssid, ssidq, sizeof(ssidq)) != 0 ||
	    shell_quote(pwd, pwdq, sizeof(pwdq)) != 0 ||
	    shell_quote(STA_IFACE, ifq, sizeof(ifq)) != 0 ||
	    shell_quote(NM_AP_CONN, apq, sizeof(apq)) != 0 ||
	    shell_quote(NM_STA_CONN, staq, sizeof(staq)) != 0)
		return WIFI_SWITCH_ERR_UNKNOWN;

	LOGI("A733 AP fallback: ssid='%s'", ssid);

	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 10 connection down %s >/dev/null 2>&1", staq);
	exec_cmd(cmd);
	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 10 connection down %s >/dev/null 2>&1", apq);
	exec_cmd(cmd);
	snprintf(cmd, sizeof(cmd),
	         "nmcli connection delete %s >/dev/null 2>&1", apq);
	exec_cmd(cmd);
	exec_cmd("nmcli radio wifi on >/dev/null 2>&1");
	snprintf(cmd, sizeof(cmd),
	         "nmcli device set %s managed yes >/dev/null 2>&1", ifq);
	exec_cmd(cmd);

	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 40 device wifi hotspot ifname %s con-name %s ssid %s password %s 2>&1",
	         ifq, apq, ssidq, pwdq);
	rc = exec_capture(cmd, out, sizeof(out));
	if (rc != 0) {
		LOGE("A733 hotspot failed rc=%d: %.300s", rc, out);
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}

	snprintf(cmd, sizeof(cmd),
	         "nmcli connection modify %s ipv4.method shared ipv4.addresses %s >/dev/null 2>&1",
	         apq, AP_CIDR);
	if (exec_cmd(cmd) != 0)
		LOGW("A733 hotspot: failed to force AP address %s", AP_CIDR);

	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 10 connection down %s >/dev/null 2>&1", apq);
	exec_cmd(cmd);
	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 40 connection up %s 2>&1", apq);
	rc = exec_capture(cmd, out, sizeof(out));
	if (rc != 0) {
		LOGE("A733 hotspot re-up failed rc=%d: %.300s", rc, out);
		return WIFI_SWITCH_ERR_CONNECT_FAIL;
	}

	mark_network_ready("AP", AP_ADDR);
	return WIFI_SWITCH_OK;
}

wifi_switch_status_t wifi_switch_ensure_ap_mode(void)
{
	wifi_switch_status_t st;

	if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
		LOGW("wifi_switch_ensure_ap_mode: switch already in progress");
		return WIFI_SWITCH_ERR_BUSY;
	}

	if (nm_connection_active(NM_AP_CONN)) {
		LOGI("wifi_switch_ensure_ap_mode: AP already active");
		pthread_mutex_unlock(&s_switch_mutex);
		return WIFI_SWITCH_OK;
	}

	st = ensure_ap_mode_locked();
	pthread_mutex_unlock(&s_switch_mutex);
	return st;
}

static void wifi_reconnect_monitor(void)
{
	int miss = 0;

	LOGI("A733 reconnect monitor started");
	for (;;) {
		sleep(30);
		if (wifi_switch_is_connected()) {
			miss = 0;
			continue;
		}

		miss++;
		LOGW("A733 WiFi not connected (%d/2)", miss);
		if (miss < 2)
			continue;
		miss = 0;

		{
			char ssid[WIFI_SSID_MAX + 1];
			char pwd[WIFI_PASSWD_MAX + 1];
			if (read_home_wifi(ssid, sizeof(ssid), pwd, sizeof(pwd)) == 0) {
				LOGI("A733 reconnecting stored SSID '%s'", ssid);
				wifi_switch_run_now(ssid, pwd);
			} else if (!nm_connection_active(NM_AP_CONN)) {
				LOGW("A733 no stored WiFi; restoring AP fallback");
				wifi_switch_ensure_ap_mode();
			}
		}
	}
}

void *wifi_switch_thread(void *arg)
{
	char ssid[WIFI_SSID_MAX + 1];
	char pwd[WIFI_PASSWD_MAX + 1];
	wifi_switch_status_t st;

	(void)arg;

	LOGI("=================================================");
	LOGI("  A733 WiFi switch - Debian/NetworkManager backend");
	LOGI("=================================================");

	if (read_home_wifi(ssid, sizeof(ssid), pwd, sizeof(pwd)) == 0) {
		LOGI("[BOOT] stored SSID found: %s", ssid);
		st = wifi_switch_run_now(ssid, pwd);
		if (st != WIFI_SWITCH_OK)
			LOGW("[BOOT] STA failed (%d); AP fallback should be active", (int)st);
	} else {
		LOGW("[BOOT] no credentials in %s; starting AP fallback", HOME_WIFI_CONF);
		st = wifi_switch_ensure_ap_mode();
		if (st != WIFI_SWITCH_OK)
			LOGE("[BOOT] AP fallback failed (%d)", (int)st);
	}

	wifi_reconnect_monitor();
	return NULL;
}

static int freq_to_scan_band(unsigned int freq)
{
	if (freq >= 2400 && freq < 2500)
		return WIFI_SCAN_BAND_24G;
	if (freq >= 5000 && freq < 6000)
		return WIFI_SCAN_BAND_5G;
	return -1;
}

typedef struct {
	uint8_t      ssid[WIFI_SCAN_SSID_MAX];
	uint8_t      ssid_len;
	int          rssi;
	unsigned int freq;
	int          have_signal;
	int          have_freq;
	int          have_ssid;
	int          has_privacy;
	int          has_rsn;
	int          has_wpa;
	int          has_sae;
	int          has_psk;
	int          has_eap;
} bss_parse_t;

static uint8_t derive_auth_type(const bss_parse_t *b)
{
	if (b->has_sae && !b->has_psk) return WIFI_SCAN_AUTH_WPA3_SAE;
	if (b->has_eap) return WIFI_SCAN_AUTH_ENTERPRISE;
	if (b->has_rsn && (b->has_psk || b->has_sae))
		return WIFI_SCAN_AUTH_WPA2_PSK;
	if (b->has_wpa && b->has_psk) return WIFI_SCAN_AUTH_WPA_PSK;
	if (b->has_privacy) return WIFI_SCAN_AUTH_WEP;
	return WIFI_SCAN_AUTH_OPEN;
}

static void commit_bss(bss_parse_t *b,
                       wifi_scan_entry_t *out_arr,
                       size_t *out_count, size_t out_cap)
{
	int band;
	wifi_scan_entry_t *e;

	if (!b->have_ssid || b->ssid_len == 0) goto reset;
	if (!b->have_freq) goto reset;
	band = freq_to_scan_band(b->freq);
	if (band < 0) goto reset;
	if (*out_count >= out_cap) goto reset;

	e = &out_arr[(*out_count)++];
	memset(e, 0, sizeof(*e));
	memcpy(e->ssid, b->ssid, b->ssid_len);
	e->ssid_len = b->ssid_len;
	e->rssi = (int8_t)(b->have_signal ? b->rssi : -100);
	e->auth_type = derive_auth_type(b);
	e->band = (uint8_t)band;

reset:
	memset(b, 0, sizeof(*b));
}

static int parse_iw_scan(const char *buf,
                         wifi_scan_entry_t *out_arr, size_t out_cap,
                         size_t *out_count)
{
	bss_parse_t cur;
	char *work, *line, *saveptr = NULL;

	*out_count = 0;
	memset(&cur, 0, sizeof(cur));

	work = strdup(buf);
	if (!work)
		return -1;

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
			if (strstr(t + 11, "Privacy"))
				cur.has_privacy = 1;
		} else if (strncmp(t, "RSN:", 4) == 0) {
			cur.has_rsn = 1;
		} else if (strncmp(t, "WPA:", 4) == 0) {
			cur.has_wpa = 1;
		}

		if (strstr(t, "SAE")) cur.has_sae = 1;
		if (strstr(t, "PSK")) cur.has_psk = 1;
		if (strstr(t, "EAP") ||
		    strstr(t, "802.1X") ||
		    strstr(t, "Enterprise"))
			cur.has_eap = 1;
	}

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
	wifi_scan_entry_t *arr = NULL;
	char *scan_buf = NULL;
	char ifq[128], cmd[256];
	int attempt, rc = -1, ret = -1;
	size_t count = 0;

	if (!out_entries || !out_count)
		return -1;

	if (pthread_mutex_trylock(&s_switch_mutex) != 0) {
		LOGW("wifi_switch_scan: another switch in progress");
		return -1;
	}

	if (wait_for_system_ready() != 0)
		goto out;
	if (shell_quote(STA_IFACE, ifq, sizeof(ifq)) != 0)
		goto out;

	arr = calloc(MAX_SCAN_ENTRIES, sizeof(*arr));
	scan_buf = malloc(SCAN_BUF_SZ);
	if (!arr || !scan_buf) {
		LOGE("wifi_switch_scan: OOM");
		goto out;
	}

	snprintf(cmd, sizeof(cmd),
	         "nmcli -w 10 device wifi rescan ifname %s >/dev/null 2>&1", ifq);
	exec_cmd(cmd);
	sleep(1);

	snprintf(cmd, sizeof(cmd), "iw dev %s scan 2>&1", ifq);
	for (attempt = 1; attempt <= 3; attempt++) {
		rc = exec_capture(cmd, scan_buf, SCAN_BUF_SZ);
		if (rc == 0 && strstr(scan_buf, "BSS "))
			break;
		LOGW("wifi_switch_scan: attempt %d/3 unusable (rc=%d): %.180s",
		     attempt, rc, scan_buf[0] ? scan_buf : "(empty)");
		if (attempt < 3)
			sleep(2);
	}

	if (rc != 0 || !strstr(scan_buf, "BSS ")) {
		LOGE("wifi_switch_scan: no usable scan output");
		goto out;
	}

	if (parse_iw_scan(scan_buf, arr, MAX_SCAN_ENTRIES, &count) != 0) {
		LOGE("wifi_switch_scan: parse_iw_scan failed");
		goto out;
	}

	LOGI("wifi_switch_scan: %zu visible network(s)", count);
	*out_entries = arr;
	*out_count = count;
	arr = NULL;
	ret = 0;

out:
	free(scan_buf);
	free(arr);
	pthread_mutex_unlock(&s_switch_mutex);
	return ret;
}
