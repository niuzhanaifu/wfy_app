#ifndef SYSTEM_SERVICE_PROTOCOL_H
#define SYSTEM_SERVICE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* On-wire frame layout (1-based offsets to mirror the spec table):
 *
 *   bytes  size       field        notes
 *   1..2   uint16_t   magic        = 0x55 0xAA
 *   3..4   uint16_t   crc          big-endian; reserved, currently NOT validated
 *   5      uint8_t    type         0=request, 1=response
 *   6      uint8_t    seq          sequence id
 *   7      uint8_t    command_set  command family. 0x01 = network
 *   8      uint8_t    command_id   command number within the family
 *   9..10  uint16_t   msg_len      payload byte count, big-endian
 *   11..N  uint8_t[]  msg          payload (msg_len bytes; may be 0)
 *
 * This module is transport-agnostic: it converts between raw byte frames
 * and proto_frame_t structs. No I/O, no logging, no knowledge of BLE. */

#define PROTO_MAGIC_0     0x55
#define PROTO_MAGIC_1     0xAA
#define PROTO_HDR_LEN     10

/* Practical cap on payload size accepted/produced by this module. The
 * wire field msg_len is a 16-bit value so the protocol allows up to 65535
 * bytes per frame, but the BLE NUS transport in practice carries far less
 * (MTU ~244 bytes per write). 256 covers every defined message with room
 * to spare and keeps stack-allocated frame buffers small. */
#define PROTO_PAYLOAD_MAX 256

#define PROTO_TYPE_REQ    0x00   /* type=0  client->server request, or server->client push */
#define PROTO_TYPE_RESP   0x01   /* type=1  reply, seq echoes the originating REQ */

/* Command set (byte 7 of the header). Only one family is defined today;
 * the byte is reserved so future feature groups can be added without
 * stomping on existing command_id ranges. */
#define PROTO_CMDSET_NETWORK   0x01

/* Command IDs (byte 8 of the header). Valid only within their command_set;
 * names assume PROTO_CMDSET_NETWORK. */
#define PROTO_CMDID_GET_WIFI_STATE 0x00   /* phone -> device: query wifi connection state.
                                             RESP payload is exactly one byte (NOT an errcode):
                                               0x00 = connected (PROTO_WIFI_CONNECTED)
                                               0x01 = not connected (PROTO_WIFI_DISCONNECTED) */
#define PROTO_CMDID_STATUS         0x01   /* phone -> device: device status query */
#define PROTO_CMDID_SSID           0x02   /* phone -> device: stash target SSID */
#define PROTO_CMDID_PASSWD         0x03   /* phone -> device: stash target PSK */
#define PROTO_CMDID_CONNECT        0x04   /* phone -> device: kick off the join. Reply is
                                             IN_PROGRESS, real outcome arrives as a server-
                                             initiated PUSH_RESULT frame keyed by the same seq. */
#define PROTO_CMDID_PUSH_RESULT    0x05   /* device -> phone: async final result of an earlier
                                             CONNECT. Type=REQ, seq matches the originating
                                             CONNECT REQ, payload = single errcode byte. */
#define PROTO_CMDID_GET_AP_SSID    0x06   /* phone -> device: read SSID our hostapd advertises.
                                             RESP payload = errcode byte + SSID bytes. */
#define PROTO_CMDID_GET_AP_PASSWD  0x07   /* phone -> device: read our hostapd PSK.
                                             RESP payload = errcode byte + PSK bytes. */
#define PROTO_CMDID_SCAN_WIFI      0x08   /* phone -> device: kick off a WiFi scan. Reply
                                             is a single errcode byte (typically IN_PROGRESS).
                                             Real results are pushed asynchronously as 0x09
                                             frames; the phone waits up to 10 s after the
                                             ack for the first 0x09. */
#define PROTO_CMDID_PUSH_SCAN_RESULT 0x09 /* device -> phone: PUSH a chunk of scan results.
                                             Type=REQ, seq matches the originating SCAN REQ.
                                             Payload (may span multiple frames):
                                               [0]  u8  frame_index  (0..N-1)
                                               [1]  u8  flags         bit0=end_of_list
                                               [2]  u8  entry_count   K
                                               [3..] K entries, each:
                                                   u8     ssid_len     (1..32)
                                                   bytes  ssid (UTF-8)
                                                   i8     rssi (dBm)
                                                   u8     auth_type    (see below)
                                                   u8     band         (0=2.4G, 1=5G)
                                             auth_type: 0=open 1=WEP 2=WPA-PSK 3=WPA2-PSK
                                                        4=WPA3-SAE 5=Enterprise 255=unknown */

/* Errcodes. For STATUS / SSID / PASSWD / CONNECT replies the RESP payload
 * is exactly one of these bytes. For GET_AP_* the RESP payload is the
 * errcode followed by the requested data when errcode == NONE. */
#define PROTO_ERRCODE_NONE          0x00   /* success / no error */
#define PROTO_ERRCODE_BAD_CMD       0x01   /* unknown / unsupported command_id */
#define PROTO_ERRCODE_NO_SSID       0x02   /* no such SSID in scan results */
#define PROTO_ERRCODE_BAD_PASSWORD  0x03   /* auth / 4-way handshake failed */
#define PROTO_ERRCODE_CONNECT_FAIL  0x04   /* assoc / dhcp / other step failed */
#define PROTO_ERRCODE_IN_PROGRESS   0x05   /* request accepted, work running asynchronously;
                                              a PUSH_RESULT frame will deliver the real outcome */
#define PROTO_ERRCODE_UNKNOWN       0xFF   /* anything else */

/* Payload bytes for the GET_WIFI_STATE reply. The RESP carries exactly
 * one of these as its single payload byte — this is NOT an errcode and
 * MUST NOT be interpreted via PROTO_ERRCODE_*. */
#define PROTO_WIFI_CONNECTED        0x00
#define PROTO_WIFI_DISCONNECTED     0x01

typedef enum {
	PROTO_OK          = 0,
	PROTO_ERR_SHORT   = -1,   /* buffer smaller than header */
	PROTO_ERR_MAGIC   = -2,   /* leading magic mismatched */
	PROTO_ERR_TRUNC   = -3,   /* declared msg_len > remaining buffer or > PROTO_PAYLOAD_MAX */
	PROTO_ERR_NOSPACE = -4,   /* encode output buffer too small */
	PROTO_ERR_ARG     = -5,   /* NULL / bad argument */
} proto_status_t;

/* Parsed frame view. `payload` points INTO the source buffer supplied to
 * proto_decode — it is NOT copied. The source buffer must outlive any use
 * of this struct. `frame_size` = PROTO_HDR_LEN + msg_len; handy for
 * walking concatenated frames. */
typedef struct {
	uint16_t       crc;          /* (data[2] << 8) | data[3], as received  */
	uint8_t        type;
	uint8_t        seq;
	uint8_t        command_set;
	uint8_t        command_id;
	uint16_t       msg_len;      /* payload byte count                     */
	const uint8_t *payload;      /* NULL iff msg_len == 0                  */
	size_t         frame_size;   /* PROTO_HDR_LEN + msg_len                */
} proto_frame_t;

/* Decode a single frame at the start of data[0..len-1]. On success returns
 * PROTO_OK and fills *out (frame_size tells the caller how much to skip). */
proto_status_t proto_decode(const uint8_t *data, size_t len,
                            proto_frame_t *out);

/* Build a frame into out[0..out_cap-1]. CRC is zero-filled for now.
 * Returns bytes written on success, or negative proto_status_t on error.
 * payload may be NULL when plen == 0. */
int proto_encode(uint8_t *out, size_t out_cap,
                 uint8_t type, uint8_t seq,
                 uint8_t command_set, uint8_t command_id,
                 const uint8_t *payload, uint16_t plen);

/* Name lookups for logging / diagnostics. Always return a valid static
 * string (never NULL) — unknown codes map to "UNKNOWN"/"?". */
const char *proto_type_name(uint8_t type);
const char *proto_command_set_name(uint8_t cs);
const char *proto_command_id_name(uint8_t id);
const char *proto_status_name(proto_status_t st);
const char *proto_errcode_name(uint8_t errcode);

#endif
