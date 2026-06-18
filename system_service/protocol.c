/* protocol — pure codec for the 55AA framing defined in protocol.h.
 *
 * This module has no knowledge of the transport it runs over. It converts
 * between raw byte frames and proto_frame_t structs and nothing else.
 * Logging and dispatch live in the caller (ble_gatt.c today, potentially
 * a UART handler tomorrow — the same codec works for either). */

#include <string.h>

#include "protocol.h"

/* ============================== Decoder ============================== */

proto_status_t proto_decode(const uint8_t *data, size_t len,
                            proto_frame_t *out)
{
	uint16_t plen;
	size_t   frame_size;

	if (!data || !out)
		return PROTO_ERR_ARG;
	if (len < PROTO_HDR_LEN)
		return PROTO_ERR_SHORT;
	if (data[0] != PROTO_MAGIC_0 || data[1] != PROTO_MAGIC_1)
		return PROTO_ERR_MAGIC;

	/* Big-endian uint16 — keeps the wire layout consistent with the
	 * existing CRC field, which the original codec reads as BE too. */
	plen       = (uint16_t)(((uint16_t)data[8] << 8) | data[9]);
	frame_size = (size_t)PROTO_HDR_LEN + plen;
	if (frame_size > len)
		return PROTO_ERR_TRUNC;
	if (plen > PROTO_PAYLOAD_MAX)
		return PROTO_ERR_TRUNC;

	out->crc         = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
	out->type        = data[4];
	out->seq         = data[5];
	out->command_set = data[6];
	out->command_id  = data[7];
	out->msg_len     = plen;
	out->payload     = (plen > 0) ? data + PROTO_HDR_LEN : NULL;
	out->frame_size  = frame_size;

	return PROTO_OK;
}

/* ============================== Encoder ============================== */

int proto_encode(uint8_t *out, size_t out_cap,
                 uint8_t type, uint8_t seq,
                 uint8_t command_set, uint8_t command_id,
                 const uint8_t *payload, uint16_t plen)
{
	size_t total;

	if (!out)
		return PROTO_ERR_ARG;
	if (plen > 0 && !payload)
		return PROTO_ERR_ARG;

	total = (size_t)PROTO_HDR_LEN + plen;
	if (total > out_cap)
		return PROTO_ERR_NOSPACE;

	out[0] = PROTO_MAGIC_0;
	out[1] = PROTO_MAGIC_1;
	out[2] = 0x00;                                /* CRC hi (placeholder)        */
	out[3] = 0x00;                                /* CRC lo (placeholder)        */
	out[4] = type;
	out[5] = seq;
	out[6] = command_set;
	out[7] = command_id;
	out[8] = (uint8_t)((plen >> 8) & 0xFF);       /* msg_len hi (big-endian)     */
	out[9] = (uint8_t)(plen & 0xFF);              /* msg_len lo                  */
	if (plen > 0)
		memcpy(out + PROTO_HDR_LEN, payload, plen);

	return (int)total;
}

/* ============================== Names ============================== */

const char *proto_type_name(uint8_t type)
{
	switch (type) {
	case PROTO_TYPE_REQ:  return "REQ";
	case PROTO_TYPE_RESP: return "RESP";
	default:              return "?";
	}
}

const char *proto_command_set_name(uint8_t cs)
{
	switch (cs) {
	case PROTO_CMDSET_NETWORK: return "NETWORK";
	default:                   return "?";
	}
}

const char *proto_command_id_name(uint8_t id)
{
	switch (id) {
	case PROTO_CMDID_GET_WIFI_STATE: return "GET_WIFI_STATE";
	case PROTO_CMDID_STATUS:        return "STATUS";
	case PROTO_CMDID_SSID:          return "SSID";
	case PROTO_CMDID_PASSWD:        return "PASSWD";
	case PROTO_CMDID_CONNECT:       return "CONNECT";
	case PROTO_CMDID_PUSH_RESULT:   return "PUSH_RESULT";
	case PROTO_CMDID_GET_AP_SSID:   return "GET_AP_SSID";
	case PROTO_CMDID_GET_AP_PASSWD: return "GET_AP_PASSWD";
	case PROTO_CMDID_SCAN_WIFI:     return "SCAN_WIFI";
	case PROTO_CMDID_PUSH_SCAN_RESULT: return "PUSH_SCAN_RESULT";
	case PROTO_CMDID_SWITCH_TO_AP:  return "SWITCH_TO_AP";
	default:                        return "UNKNOWN";
	}
}

const char *proto_errcode_name(uint8_t errcode)
{
	switch (errcode) {
	case PROTO_ERRCODE_NONE:         return "NONE";
	case PROTO_ERRCODE_BAD_CMD:      return "BAD_CMD";
	case PROTO_ERRCODE_NO_SSID:      return "NO_SSID";
	case PROTO_ERRCODE_BAD_PASSWORD: return "BAD_PASSWORD";
	case PROTO_ERRCODE_CONNECT_FAIL: return "CONNECT_FAIL";
	case PROTO_ERRCODE_IN_PROGRESS:  return "IN_PROGRESS";
	case PROTO_ERRCODE_UNKNOWN:      return "UNKNOWN";
	default:                         return "?";
	}
}

const char *proto_status_name(proto_status_t st)
{
	switch (st) {
	case PROTO_OK:          return "OK";
	case PROTO_ERR_SHORT:   return "SHORT (buffer < header)";
	case PROTO_ERR_MAGIC:   return "MAGIC mismatch";
	case PROTO_ERR_TRUNC:   return "TRUNC (msg_len > remaining or > PAYLOAD_MAX)";
	case PROTO_ERR_NOSPACE: return "NOSPACE (output too small)";
	case PROTO_ERR_ARG:     return "ARG (bad argument)";
	default:                return "?";
	}
}
