#ifndef SYSTEM_SERVICE_WIFI_SWITCH_H
#define SYSTEM_SERVICE_WIFI_SWITCH_H

#include <stddef.h>
#include <stdint.h>

/* Outcome of a single STA/AP switch attempt. Chosen so callers (e.g.
 * ble_gatt) can map cleanly onto the on-wire protocol error codes while
 * keeping wifi_switch.c free of any protocol/transport knowledge. */
typedef enum {
	WIFI_SWITCH_OK               = 0,  /* associated + DHCP succeeded */
	WIFI_SWITCH_ERR_NO_SSID      = 1,  /* scan never saw the target SSID */
	WIFI_SWITCH_ERR_BAD_PASSWORD = 2,  /* 4-way handshake rejected the PSK */
	WIFI_SWITCH_ERR_CONNECT_FAIL = 3,  /* assoc / DHCP / other step failed */
	WIFI_SWITCH_ERR_BUSY         = 4,  /* another switch is already running */
	WIFI_SWITCH_ERR_UNKNOWN      = 5,  /* internal error (OOM, bad args, ...) */
} wifi_switch_status_t;

/* Boot-time pthread entry: load creds from /etc/taishan_home_wifi.conf and
 * run the switch; on failure retry every 60 s; stays resident on success. */
void *wifi_switch_thread(void *arg);

/* On-demand one-shot switch, safe to call from any thread. Serializes
 * against wifi_switch_thread via an internal mutex so the boot workflow
 * and BLE-triggered provisioning cannot step on each other.
 *
 *   ssid     — target SSID. NULL or empty means "reload from
 *              /etc/taishan_home_wifi.conf" (same as boot-time behaviour).
 *   password — WPA PSK; NULL is treated as empty (open network).
 *
 * Blocks for up to ~60 s (scan + assoc + DHCP). Do NOT call from a D-Bus
 * dispatcher thread — run it on a worker and deliver the result async. */
wifi_switch_status_t wifi_switch_run_now(const char *ssid, const char *password);

/* Force the board into AP fallback mode on the platform WiFi interface.
 * RK3566 uses hostapd + dnsmasq; A733/Debian uses NetworkManager hotspot.
 * Safe to call from any thread; returns WIFI_SWITCH_ERR_BUSY if a STA
 * switch/scan/recovery is active. */
wifi_switch_status_t wifi_switch_ensure_ap_mode(void);

/* Returns 1 if wlan0 currently holds a real (non-link-local) IPv4
 * address — i.e. it associated and finished DHCP — and 0 otherwise.
 * Reads /proc-style state via `ip -4 addr show`, so it always reflects
 * the *current* situation, not whatever wifi_switch_run_now last
 * returned. Cheap (~10ms via popen) and safe to call from the BLE
 * dispatcher thread. */
int wifi_switch_is_connected(void);

/* Read the SSID and PSK that the provisioning AP advertises. RK3566 parses
 * /etc/hostapd.conf; A733/Debian uses hostapd.conf when present and falls
 * back to compile-time AP defaults otherwise.
 *
 * Returns 0 on success, -1 if hostapd.conf is unreadable or has no
 * `ssid=` line. Both buffers are NUL-terminated even on failure (set to
 * empty strings) so callers don't need to pre-clear them. */
int wifi_switch_get_ap_credentials(char *ssid_out, size_t ssid_sz,
                                   char *pwd_out,  size_t pwd_sz);

/* Persist credentials to /etc/taishan_home_wifi.conf so the boot thread
 * (and S48ap, which reads the same file) picks them up on next reboot.
 * Atomic via write-then-rename so a crash mid-write cannot leave an empty
 * file. Returns 0 on success, -1 on validation or I/O failure. Callers
 * should ONLY persist after a confirmed successful join — otherwise bad
 * credentials get baked in and the boot thread loops on them forever.
 *
 * Values containing a literal double-quote are rejected because the
 * reader does not understand backslash escapes; refuse rather than
 * silently corrupt the file. */
int wifi_switch_persist_home_wifi(const char *ssid, const char *password);

/* ============================================================
 *   WiFi scan (BLE cmd 0x08 backend)
 * ============================================================ */

#define WIFI_SCAN_AUTH_OPEN          0
#define WIFI_SCAN_AUTH_WEP           1
#define WIFI_SCAN_AUTH_WPA_PSK       2
#define WIFI_SCAN_AUTH_WPA2_PSK      3
#define WIFI_SCAN_AUTH_WPA3_SAE      4
#define WIFI_SCAN_AUTH_ENTERPRISE    5
#define WIFI_SCAN_AUTH_UNKNOWN     255

#define WIFI_SCAN_BAND_24G  0
#define WIFI_SCAN_BAND_5G   1

#define WIFI_SCAN_SSID_MAX  32

typedef struct {
	uint8_t ssid_len;                       /* 1..32 (hidden SSIDs are dropped) */
	uint8_t ssid[WIFI_SCAN_SSID_MAX];       /* raw bytes, NOT NUL-terminated   */
	int8_t  rssi;                           /* dBm, signed                     */
	uint8_t auth_type;                      /* WIFI_SCAN_AUTH_*                */
	uint8_t band;                           /* WIFI_SCAN_BAND_*                */
} wifi_scan_entry_t;

/* Run a single WiFi scan and return parsed entries.
 *
 * Behaviour: kills hostapd / dnsmasq if running, sets wlan0 vif type to
 * managed, runs `iw dev wlan0 scan`, parses results, then restarts AP if
 * it was up before. wlan0 is NOT brought down — the link stays UP from
 * AP through scan to AP again, so bcmdhd's 5G priming is preserved.
 *
 * On success: returns 0 with *out_entries pointing to a malloc'd array of
 * *out_count entries (caller frees with wifi_switch_scan_free).
 * On failure: returns -1; *out_entries / *out_count untouched.
 *
 * Hidden / 0-length SSIDs are filtered out (the BLE protocol requires
 * 1..32 byte SSIDs). May block ~3-5 seconds. Safe to call from any
 * thread; serializes against try_sta_only via the same internal mutex
 * — when STA provisioning is in flight this returns -1 immediately. */
int wifi_switch_scan(wifi_scan_entry_t **out_entries, size_t *out_count);
void wifi_switch_scan_free(wifi_scan_entry_t *entries);

#endif
