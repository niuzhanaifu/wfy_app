/* proto_dispatch — transport-agnostic 0x55AA protocol handler.
 *
 * Built originally for the BLE NUS server in ble_gatt.c, then split out
 * here so a second transport (Unix-domain IPC for ovita-server) can
 * reuse the entire command set without duplicating any of the wifi /
 * scan / provisioning logic.
 *
 * One file, one responsibility: take bytes -> decode frames -> route
 * REQs to handlers -> emit RESP/PUSH back via a caller-supplied
 * proto_sink_t. Async workers (CONNECT, SCAN_WIFI) retain the sink so
 * their late PUSH frames land on the same transport that received the
 * original REQ.
 *
 * Pending SSID/PASSWD state is process-global (matching the original
 * BLE-only assumption) — if two transports race the SSID -> PASSWD ->
 * CONNECT sequence, the writes interleave and the loser reads back the
 * winner's value. wifi_switch.c's mutex still serialises the actual
 * join, so this manifests as "the wrong thing connects" rather than
 * corruption. Realistic deployments have at most one client at a time. */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define LOG_TAG "proto"
#include "log.h"

#include "protocol.h"
#include "proto_sink.h"
#include "proto_dispatch.h"
#include "wifi_switch.h"

/* ============================== Logging helpers ============================== */

static void log_hex_short(const char *prefix, const uint8_t *data, size_t len)
{
	char line[3 * 16 + 1];
	size_t i, j, n;

	if (len == 0) {
		LOGI("%s(empty)", prefix);
		return;
	}
	for (i = 0; i < len; i += 16) {
		n = (len - i < 16) ? (len - i) : 16;
		for (j = 0; j < n; j++)
			snprintf(line + j * 3, sizeof(line) - j * 3,
			         "%02x ", data[i + j]);
		line[n * 3] = '\0';
		LOGI("%s[%04zu] %s", prefix, i, line);
	}
}

static void payload_to_ascii(char *out, const uint8_t *data, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		uint8_t c = data[i];
		out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	out[len] = '\0';
}

/* ============================== Pending SSID/PASSWD ============================== */

#define WIFI_SSID_MAX     32        /* 802.11 SSID max */
#define WIFI_PASSWD_MAX   63        /* WPA2 PSK max */

static pthread_mutex_t s_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static char    s_pending_ssid[WIFI_SSID_MAX + 1];
static uint8_t s_pending_ssid_len   = 0;
static char    s_pending_passwd[WIFI_PASSWD_MAX + 1];
static uint8_t s_pending_passwd_len = 0;

/* ============================== Send helpers ============================== */

static void send_proto_frame(proto_sink_t *sink,
                             uint8_t type, uint8_t seq,
                             uint8_t command_set, uint8_t command_id,
                             const uint8_t *payload, uint16_t plen)
{
	uint8_t buf[PROTO_HDR_LEN + PROTO_PAYLOAD_MAX];
	int wrote;

	wrote = proto_encode(buf, sizeof(buf), type, seq,
	                     command_set, command_id, payload, plen);
	if (wrote < 0) {
		LOGE("proto_encode failed: %s",
		     proto_status_name((proto_status_t)wrote));
		return;
	}

	LOGI("-> %s seq=0x%02x cs=0x%02x(%s) cid=0x%02x(%s) plen=%u total=%d bytes",
	     proto_type_name(type), seq,
	     command_set, proto_command_set_name(command_set),
	     command_id, proto_command_id_name(command_id),
	     plen, wrote);
	log_hex_short("   tx ", buf, (size_t)wrote);

	proto_sink_send(sink, buf, (size_t)wrote);
}

static void send_proto_response(proto_sink_t *sink, uint8_t seq,
                                uint8_t command_set, uint8_t command_id,
                                uint8_t errcode)
{
	send_proto_frame(sink, PROTO_TYPE_RESP, seq, command_set, command_id,
	                 &errcode, 1);
}

static void send_proto_response_data(proto_sink_t *sink, uint8_t seq,
                                     uint8_t command_set, uint8_t command_id,
                                     uint8_t errcode,
                                     const uint8_t *data, uint16_t dlen)
{
	uint8_t payload[PROTO_PAYLOAD_MAX];
	if ((size_t)dlen + 1 > sizeof(payload)) {
		LOGE("send_proto_response_data: data too long (%u)", dlen);
		return;
	}
	payload[0] = errcode;
	if (dlen) memcpy(payload + 1, data, dlen);
	send_proto_frame(sink, PROTO_TYPE_RESP, seq, command_set, command_id,
	                 payload, (uint16_t)(dlen + 1));
}

static void send_proto_push(proto_sink_t *sink, uint8_t seq,
                            uint8_t command_set, uint8_t command_id,
                            const uint8_t *data, uint16_t dlen)
{
	send_proto_frame(sink, PROTO_TYPE_REQ, seq, command_set, command_id,
	                 data, dlen);
}

/* ============================== wifi_switch -> errcode ============================== */

static uint8_t wifi_status_to_errcode(wifi_switch_status_t st)
{
	switch (st) {
	case WIFI_SWITCH_OK:               return PROTO_ERRCODE_NONE;
	case WIFI_SWITCH_ERR_NO_SSID:      return PROTO_ERRCODE_NO_SSID;
	case WIFI_SWITCH_ERR_BAD_PASSWORD: return PROTO_ERRCODE_BAD_PASSWORD;
	case WIFI_SWITCH_ERR_CONNECT_FAIL: return PROTO_ERRCODE_CONNECT_FAIL;
	case WIFI_SWITCH_ERR_BUSY:
	case WIFI_SWITCH_ERR_UNKNOWN:
	default:                           return PROTO_ERRCODE_UNKNOWN;
	}
}

/* ============================== CONNECT worker ============================== */

typedef struct {
	uint8_t       seq;
	char          ssid[WIFI_SSID_MAX + 1];
	char          passwd[WIFI_PASSWD_MAX + 1];
	proto_sink_t *sink;        /* retained reference; released on exit */
} wifi_connect_job_t;

static void *wifi_connect_worker(void *arg)
{
	wifi_connect_job_t *job = arg;
	wifi_switch_status_t st;
	uint8_t errcode;

	LOGI("wifi_connect_worker: begin  seq=0x%02x  ssid='%s'  pwd_len=%zu",
	     job->seq, job->ssid, strlen(job->passwd));

	st = wifi_switch_run_now(job->ssid, job->passwd);
	errcode = wifi_status_to_errcode(st);

	if (st == WIFI_SWITCH_OK) {
		if (wifi_switch_persist_home_wifi(job->ssid, job->passwd) == 0)
			LOGI("wifi_connect_worker: credentials persisted to /etc/taishan_home_wifi.conf");
		else
			LOGW("wifi_connect_worker: connect succeeded but failed "
			     "to persist credentials; reboot will revert");
	}

	LOGI("wifi_connect_worker: done  seq=0x%02x  status=%d  errcode=0x%02x(%s)",
	     job->seq, (int)st, errcode, proto_errcode_name(errcode));

	send_proto_push(job->sink, job->seq, PROTO_CMDSET_NETWORK,
	                PROTO_CMDID_PUSH_RESULT, &errcode, 1);

	proto_sink_release(job->sink);
	free(job);
	return NULL;
}

static int trigger_wifi_connect(uint8_t seq, proto_sink_t *sink,
                                uint8_t *out_immediate_err)
{
	wifi_connect_job_t *job;
	pthread_t tid;
	int rc;
	uint8_t ssid_len, pwd_len;

	pthread_mutex_lock(&s_pending_lock);
	ssid_len = s_pending_ssid_len;
	pwd_len  = s_pending_passwd_len;
	if (ssid_len == 0) {
		pthread_mutex_unlock(&s_pending_lock);
		LOGE("  trigger_wifi_connect: no SSID stored — refusing");
		*out_immediate_err = PROTO_ERRCODE_NO_SSID;
		return -1;
	}

	job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_unlock(&s_pending_lock);
		LOGE("  trigger_wifi_connect: OOM allocating job");
		*out_immediate_err = PROTO_ERRCODE_UNKNOWN;
		return -1;
	}
	job->seq = seq;
	memcpy(job->ssid,   s_pending_ssid,   ssid_len);
	job->ssid[ssid_len] = '\0';
	memcpy(job->passwd, s_pending_passwd, pwd_len);
	job->passwd[pwd_len] = '\0';
	pthread_mutex_unlock(&s_pending_lock);

	job->sink = sink;
	proto_sink_retain(sink);

	rc = pthread_create(&tid, NULL, wifi_connect_worker, job);
	if (rc != 0) {
		LOGE("  trigger_wifi_connect: pthread_create failed: %s",
		     strerror(rc));
		proto_sink_release(sink);
		free(job);
		*out_immediate_err = PROTO_ERRCODE_UNKNOWN;
		return -1;
	}
	pthread_detach(tid);

	LOGI("  trigger_wifi_connect: worker spawned (seq=0x%02x, ssid='%s') — "
	     "PUSH_RESULT will follow when switch completes",
	     seq, job->ssid);
	return 0;
}

/* ============================== SCAN worker ============================== */

#define SCAN_PUSH_PAYLOAD_CAP       220
#define SCAN_PUSH_ENTRIES_PER_FRAME 5

typedef struct {
	uint8_t       seq;
	proto_sink_t *sink;
} wifi_scan_job_t;

static int encode_scan_entry(const wifi_scan_entry_t *e,
                             uint8_t *dst, size_t dst_cap)
{
	size_t need = (size_t)1 + e->ssid_len + 3;
	if (need > dst_cap) return -1;
	dst[0] = e->ssid_len;
	memcpy(dst + 1, e->ssid, e->ssid_len);
	dst[1 + e->ssid_len + 0] = (uint8_t)e->rssi;
	dst[1 + e->ssid_len + 1] = e->auth_type;
	dst[1 + e->ssid_len + 2] = e->band;
	return (int)need;
}

static void send_scan_results(proto_sink_t *sink, uint8_t seq,
                              const wifi_scan_entry_t *entries, size_t count)
{
	uint8_t  payload[PROTO_PAYLOAD_MAX];
	size_t   i = 0;
	uint8_t  frame_index = 0;

	do {
		size_t pos = 3;
		uint8_t entry_count = 0;

		while (i < count && entry_count < SCAN_PUSH_ENTRIES_PER_FRAME) {
			int n = encode_scan_entry(&entries[i],
			                          payload + pos,
			                          SCAN_PUSH_PAYLOAD_CAP - pos);
			if (n < 0) break;
			pos += (size_t)n;
			entry_count++;
			i++;
		}

		uint8_t flags = (i >= count) ? 0x01 : 0x00;
		payload[0] = frame_index;
		payload[1] = flags;
		payload[2] = entry_count;

		LOGI("  scan-push frame %u: entries=%u, flags=0x%02x, plen=%zu",
		     frame_index, entry_count, flags, pos);
		send_proto_push(sink, seq, PROTO_CMDSET_NETWORK,
		                PROTO_CMDID_PUSH_SCAN_RESULT,
		                payload, (uint16_t)pos);
		frame_index++;
	} while (i < count);
}

static void *wifi_scan_worker(void *arg)
{
	wifi_scan_job_t *job = arg;
	wifi_scan_entry_t *entries = NULL;
	size_t count = 0;

	LOGI("wifi_scan_worker: begin seq=0x%02x", job->seq);

	if (wifi_switch_scan(&entries, &count) != 0) {
		LOGW("wifi_scan_worker: scan failed; sending empty result list");
		count = 0;
		entries = NULL;
	} else {
		size_t k;
		LOGI("wifi_scan_worker: got %zu entries", count);
		for (k = 0; k < count; k++) {
			char ssid_str[WIFI_SCAN_SSID_MAX + 1];
			memcpy(ssid_str, entries[k].ssid, entries[k].ssid_len);
			ssid_str[entries[k].ssid_len] = '\0';
			LOGI("    [%zu] '%s' rssi=%d auth=%u band=%u",
			     k, ssid_str, entries[k].rssi,
			     entries[k].auth_type, entries[k].band);
		}
	}

	send_scan_results(job->sink, job->seq, entries, count);

	wifi_switch_scan_free(entries);
	proto_sink_release(job->sink);
	free(job);
	return NULL;
}

static int trigger_wifi_scan(uint8_t seq, proto_sink_t *sink,
                             uint8_t *out_immediate_err)
{
	wifi_scan_job_t *job;
	pthread_t tid;
	int rc;

	job = calloc(1, sizeof(*job));
	if (!job) {
		LOGE("  trigger_wifi_scan: OOM allocating job");
		*out_immediate_err = PROTO_ERRCODE_UNKNOWN;
		return -1;
	}
	job->seq  = seq;
	job->sink = sink;
	proto_sink_retain(sink);

	rc = pthread_create(&tid, NULL, wifi_scan_worker, job);
	if (rc != 0) {
		LOGE("  trigger_wifi_scan: pthread_create failed: %s",
		     strerror(rc));
		proto_sink_release(sink);
		free(job);
		*out_immediate_err = PROTO_ERRCODE_UNKNOWN;
		return -1;
	}
	pthread_detach(tid);

	LOGI("  trigger_wifi_scan: worker spawned (seq=0x%02x) — "
	     "PUSH_SCAN_RESULT frames will follow",
	     seq);
	return 0;
}

/* ============================== Frame dispatcher ============================== */

static void handle_proto_frame(const proto_frame_t *f, proto_sink_t *sink)
{
	uint8_t cs = f->command_set;
	uint8_t cid = f->command_id;

	LOGI("---- parsed frame (%zu bytes) ----", f->frame_size);
	LOGI("  crc         : 0x%04x  (not validated)", f->crc);
	LOGI("  type        : 0x%02x  (%s)", f->type, proto_type_name(f->type));
	LOGI("  seq         : 0x%02x", f->seq);
	LOGI("  command_set : 0x%02x  (%s)", cs, proto_command_set_name(cs));
	LOGI("  command_id  : 0x%02x  (%s)", cid, proto_command_id_name(cid));
	LOGI("  msg_len     : %u  (payload bytes)", f->msg_len);

	if (f->msg_len == 0) {
		LOGI("  payload     : (empty)");
	} else {
		char ascii[PROTO_PAYLOAD_MAX + 1];
		payload_to_ascii(ascii, f->payload, f->msg_len);
		LOGI("  payload     : '%s'  (%u bytes)", ascii, f->msg_len);
		log_hex_short("    hex ", f->payload, f->msg_len);
	}

	if (f->type != PROTO_TYPE_REQ) {
		LOGI("  not a REQ, no reply emitted");
		return;
	}

	if (cs != PROTO_CMDSET_NETWORK) {
		LOGE("  unsupported command_set 0x%02x → reply BAD_CMD", cs);
		send_proto_response(sink, f->seq, cs, cid, PROTO_ERRCODE_BAD_CMD);
		return;
	}

	switch (cid) {
	case PROTO_CMDID_GET_WIFI_STATE: {
		uint8_t state = wifi_switch_is_connected()
		                ? PROTO_WIFI_CONNECTED
		                : PROTO_WIFI_DISCONNECTED;
		LOGI("  >> handler: GET_WIFI_STATE -> 0x%02x (%s)",
		     state, state == PROTO_WIFI_CONNECTED
		            ? "connected" : "disconnected");
		send_proto_frame(sink, PROTO_TYPE_RESP, f->seq, cs, cid, &state, 1);
		break;
	}

	case PROTO_CMDID_STATUS:
		LOGI("  >> handler: device STATUS query → reply NONE");
		send_proto_response(sink, f->seq, cs, cid, PROTO_ERRCODE_NONE);
		break;

	case PROTO_CMDID_SSID:
		if (f->msg_len == 0 || f->msg_len > WIFI_SSID_MAX) {
			LOGE("  SSID length %u out of range (1..%d)",
			     f->msg_len, WIFI_SSID_MAX);
			send_proto_response(sink, f->seq, cs, cid,
			                    PROTO_ERRCODE_UNKNOWN);
			break;
		}
		pthread_mutex_lock(&s_pending_lock);
		memcpy(s_pending_ssid, f->payload, f->msg_len);
		s_pending_ssid[f->msg_len] = '\0';
		s_pending_ssid_len = (uint8_t)f->msg_len;
		pthread_mutex_unlock(&s_pending_lock);
		LOGI("  >> handler: SSID stored: '%s'", s_pending_ssid);
		send_proto_response(sink, f->seq, cs, cid, PROTO_ERRCODE_NONE);
		break;

	case PROTO_CMDID_PASSWD:
		if (f->msg_len == 0 || f->msg_len > WIFI_PASSWD_MAX) {
			LOGE("  PASSWD length %u out of range (1..%d)",
			     f->msg_len, WIFI_PASSWD_MAX);
			send_proto_response(sink, f->seq, cs, cid,
			                    PROTO_ERRCODE_UNKNOWN);
			break;
		}
		pthread_mutex_lock(&s_pending_lock);
		memcpy(s_pending_passwd, f->payload, f->msg_len);
		s_pending_passwd[f->msg_len] = '\0';
		s_pending_passwd_len = (uint8_t)f->msg_len;
		pthread_mutex_unlock(&s_pending_lock);
		LOGI("  >> handler: password stored, len=%u (value not logged)",
		     f->msg_len);
		send_proto_response(sink, f->seq, cs, cid, PROTO_ERRCODE_NONE);
		break;

	case PROTO_CMDID_CONNECT: {
		uint8_t immediate_err = 0;
		LOGI("  >> handler: CONNECT requested — replying IN_PROGRESS now, "
		     "PUSH_RESULT (cid=0x%02x) will follow when the switch finishes",
		     PROTO_CMDID_PUSH_RESULT);
		if (trigger_wifi_connect(f->seq, sink, &immediate_err) == 0) {
			send_proto_response(sink, f->seq, cs, cid,
			                    PROTO_ERRCODE_IN_PROGRESS);
		} else {
			send_proto_response(sink, f->seq, cs, cid, immediate_err);
		}
		break;
	}

	case PROTO_CMDID_GET_AP_SSID: {
		char ap_ssid[64], ap_pwd[128];
		LOGI("  >> handler: GET_AP_SSID");
		if (wifi_switch_get_ap_credentials(ap_ssid, sizeof(ap_ssid),
		                                   ap_pwd,  sizeof(ap_pwd)) == 0
		    && ap_ssid[0] != '\0') {
			size_t n = strlen(ap_ssid);
			if (n > PROTO_PAYLOAD_MAX - 1) n = PROTO_PAYLOAD_MAX - 1;
			LOGI("    AP SSID = '%s' (%zu bytes)", ap_ssid, n);
			send_proto_response_data(sink, f->seq, cs, cid,
			                         PROTO_ERRCODE_NONE,
			                         (const uint8_t *)ap_ssid,
			                         (uint16_t)n);
		} else {
			LOGE("    failed to read AP SSID from hostapd.conf");
			send_proto_response(sink, f->seq, cs, cid,
			                    PROTO_ERRCODE_UNKNOWN);
		}
		break;
	}

	case PROTO_CMDID_SCAN_WIFI: {
		uint8_t immediate_err = 0;
		LOGI("  >> handler: SCAN_WIFI requested — replying IN_PROGRESS now, "
		     "PUSH_SCAN_RESULT (cid=0x%02x) frames will follow",
		     PROTO_CMDID_PUSH_SCAN_RESULT);
		if (trigger_wifi_scan(f->seq, sink, &immediate_err) == 0) {
			send_proto_response(sink, f->seq, cs, cid,
			                    PROTO_ERRCODE_IN_PROGRESS);
		} else {
			send_proto_response(sink, f->seq, cs, cid, immediate_err);
		}
		break;
	}

	case PROTO_CMDID_GET_AP_PASSWD: {
		char ap_ssid[64], ap_pwd[128];
		LOGI("  >> handler: GET_AP_PASSWD");
		if (wifi_switch_get_ap_credentials(ap_ssid, sizeof(ap_ssid),
		                                   ap_pwd,  sizeof(ap_pwd)) == 0) {
			size_t n = strlen(ap_pwd);
			if (n > PROTO_PAYLOAD_MAX - 1) n = PROTO_PAYLOAD_MAX - 1;
			LOGI("    AP PSK = (%zu bytes, value not logged)", n);
			send_proto_response_data(sink, f->seq, cs, cid,
			                         PROTO_ERRCODE_NONE,
			                         (const uint8_t *)ap_pwd,
			                         (uint16_t)n);
		} else {
			LOGE("    failed to read AP PSK from hostapd.conf");
			send_proto_response(sink, f->seq, cs, cid,
			                    PROTO_ERRCODE_UNKNOWN);
		}
		break;
	}

	default:
		LOGE("  unknown command_id 0x%02x → reply BAD_CMD", cid);
		send_proto_response(sink, f->seq, cs, cid, PROTO_ERRCODE_BAD_CMD);
		break;
	}
}

void proto_dispatch_buffer(const uint8_t *data, size_t len, proto_sink_t *sink)
{
	size_t off = 0;

	while (off < len) {
		proto_frame_t f;
		proto_status_t st = proto_decode(data + off, len - off, &f);
		if (st != PROTO_OK) {
			LOGE("proto_decode at offset %zu (%zu bytes remaining): %s",
			     off, len - off, proto_status_name(st));
			return;
		}
		handle_proto_frame(&f, sink);
		off += f.frame_size;
	}
}
