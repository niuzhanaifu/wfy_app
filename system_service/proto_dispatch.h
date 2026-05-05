#ifndef SYSTEM_SERVICE_PROTO_DISPATCH_H
#define SYSTEM_SERVICE_PROTO_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include "proto_sink.h"

/* Walk a transport's read buffer as a stream of concatenated 0x55AA
 * frames. For each REQ frame found:
 *   - the matching command handler runs;
 *   - any synchronous RESP is written back via 'sink';
 *   - if the handler spawns an async worker (CONNECT, SCAN_WIFI), the
 *     worker captures a retained reference to 'sink' and uses it to
 *     emit PUSH_RESULT / PUSH_SCAN_RESULT later.
 *
 * Caller does NOT need to keep 'sink' alive past return — the worker
 * keeps its own ref via proto_sink_retain. */
void proto_dispatch_buffer(const uint8_t *data, size_t len,
                           proto_sink_t *sink);

#endif
