#ifndef SYSTEM_SERVICE_PROTO_SINK_H
#define SYSTEM_SERVICE_PROTO_SINK_H

#include <stddef.h>
#include <stdint.h>

/* A "place to write protocol frames to". Both BLE GATT (TX char notify)
 * and the Unix-domain IPC socket (write to a connected client fd) wear
 * this so the protocol dispatcher and async workers can route their
 * responses / pushes without caring which transport delivered the
 * originating REQ.
 *
 * retain / release form a refcount because async workers
 * (wifi_connect_worker, wifi_scan_worker) outlive the request that
 * spawned them — when the worker finally has a result to push, the
 * caller's connection may have already gone away. The dispatcher
 * retains on worker spawn and the worker releases on exit; for
 * singleton transports (BLE) retain/release are no-ops. send() is
 * always safe to call on a half-shut-down sink: it should silently
 * drop when the underlying transport is gone. */
typedef struct proto_sink proto_sink_t;
struct proto_sink {
	void (*send)   (proto_sink_t *self, const uint8_t *data, size_t len);
	void (*retain) (proto_sink_t *self);
	void (*release)(proto_sink_t *self);
};

static inline void proto_sink_send(proto_sink_t *s,
                                   const uint8_t *d, size_t n)
{
	if (s && s->send) s->send(s, d, n);
}

static inline void proto_sink_retain(proto_sink_t *s)
{
	if (s && s->retain) s->retain(s);
}

static inline void proto_sink_release(proto_sink_t *s)
{
	if (s && s->release) s->release(s);
}

#endif
