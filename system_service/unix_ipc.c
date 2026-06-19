/* unix_ipc — Unix-domain server for the 0x55AA protocol.
 *
 * Spawns one listener thread on /run/system_service.sock; each accepted
 * connection runs in its own thread and feeds bytes into
 * proto_dispatch_buffer with a per-connection sink that wraps write(fd).
 *
 * Lifetime: the per-connection state (unix_client_t) is reference-counted
 * because async workers (CONNECT, SCAN_WIFI) outlive the request that
 * spawned them. The reader thread holds one ref; each spawned worker
 * retains an extra ref (via proto_sink_retain). When everyone has
 * released, the struct is freed and the fd closed. If the client
 * disconnects mid-flight, send() turns into a no-op (fd was set to -1)
 * but the struct stays alive until the worker finishes its push attempt
 * — no use-after-free, no leaks beyond worker lifetime. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_TAG "unix_ipc"
#include "log.h"

#include "platform.h"
#include "proto_sink.h"
#include "proto_dispatch.h"
#include "unix_ipc.h"

#define SOCK_PATH          SYSTEM_SERVICE_SOCK_PATH
#define SOCK_FALLBACK_PATH SYSTEM_SERVICE_SOCK_FALLBACK_PATH

typedef struct {
	proto_sink_t    base;       /* MUST be first — we cast self -> unix_client_t */
	int             fd;         /* -1 once closed */
	int             refcount;
	pthread_mutex_t lock;       /* serialises send + state changes */
} unix_client_t;

static void unix_sink_send(proto_sink_t *self, const uint8_t *data, size_t len)
{
	unix_client_t *c = (unix_client_t *)self;
	size_t total = 0;

	pthread_mutex_lock(&c->lock);
	if (c->fd < 0) {
		LOGD("send to closed client, dropping %zu bytes", len);
		pthread_mutex_unlock(&c->lock);
		return;
	}
	while (total < len) {
		ssize_t w = write(c->fd, data + total, len - total);
		if (w < 0) {
			if (errno == EINTR) continue;
			LOGW("write to client fd=%d failed: %s",
			     c->fd, strerror(errno));
			break;
		}
		if (w == 0) break;
		total += (size_t)w;
	}
	pthread_mutex_unlock(&c->lock);
}

static void unix_sink_retain(proto_sink_t *self)
{
	unix_client_t *c = (unix_client_t *)self;
	pthread_mutex_lock(&c->lock);
	c->refcount++;
	pthread_mutex_unlock(&c->lock);
}

static void unix_sink_release(proto_sink_t *self)
{
	unix_client_t *c = (unix_client_t *)self;
	int free_now = 0;

	pthread_mutex_lock(&c->lock);
	if (--c->refcount == 0) {
		if (c->fd >= 0) {
			close(c->fd);
			c->fd = -1;
		}
		free_now = 1;
	}
	pthread_mutex_unlock(&c->lock);

	if (free_now) {
		pthread_mutex_destroy(&c->lock);
		free(c);
	}
}

static void *client_thread(void *arg)
{
	unix_client_t *c = arg;
	uint8_t buf[2048];

	LOGI("client connected (fd=%d)", c->fd);

	for (;;) {
		ssize_t n = read(c->fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR) continue;
			LOGW("client read fd=%d: %s", c->fd, strerror(errno));
			break;
		}
		if (n == 0) {
			LOGI("client fd=%d closed by peer", c->fd);
			break;
		}
		proto_dispatch_buffer(buf, (size_t)n, &c->base);
	}

	/* Mark the fd dead so any in-flight worker's push gets dropped
	 * instead of writing to a stale fd (see unix_sink_send). The
	 * struct itself stays alive as long as workers hold refs. */
	pthread_mutex_lock(&c->lock);
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	pthread_mutex_unlock(&c->lock);

	proto_sink_release(&c->base);   /* drop reader's own ref */
	return NULL;
}

void *unix_ipc_thread(void *arg)
{
	struct sockaddr_un addr;
	int srv;
	const char *sock_path = SOCK_PATH;

	(void)arg;

	srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) {
		LOGE("socket(AF_UNIX): %s", strerror(errno));
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	unlink(sock_path);   /* stale socket from previous run is harmless to remove */

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#if SYSTEM_SERVICE_IS_A733
		int saved_errno = errno;
		if ((saved_errno == EACCES || saved_errno == EPERM) &&
		    strcmp(SOCK_FALLBACK_PATH, SOCK_PATH) != 0) {
			LOGW("bind(%s): %s; falling back to %s",
			     SOCK_PATH, strerror(saved_errno), SOCK_FALLBACK_PATH);
			sock_path = SOCK_FALLBACK_PATH;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
			unlink(sock_path);
			if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0)
				goto bound;
		}
#endif
		LOGE("bind(%s): %s", sock_path, strerror(errno));
		close(srv);
		return NULL;
	}
bound:
	if (chmod(sock_path, 0666) < 0) {
		LOGW("chmod %s 0666: %s — non-root clients may fail to connect",
		     sock_path, strerror(errno));
	}
	if (listen(srv, 4) < 0) {
		LOGE("listen: %s", strerror(errno));
		close(srv);
		return NULL;
	}

	LOGI("listening on %s (mode 0666)", sock_path);

	for (;;) {
		int cli_fd = accept(srv, NULL, NULL);
		if (cli_fd < 0) {
			if (errno == EINTR) continue;
			LOGE("accept: %s", strerror(errno));
			break;
		}

		unix_client_t *c = calloc(1, sizeof(*c));
		if (!c) {
			LOGE("OOM accepting client; closing");
			close(cli_fd);
			continue;
		}
		c->base.send    = unix_sink_send;
		c->base.retain  = unix_sink_retain;
		c->base.release = unix_sink_release;
		c->fd           = cli_fd;
		c->refcount     = 1;       /* reader thread's ref */
		pthread_mutex_init(&c->lock, NULL);

		pthread_t tid;
		int rc = pthread_create(&tid, NULL, client_thread, c);
		if (rc != 0) {
			LOGE("pthread_create for client thread: %s",
			     strerror(rc));
			close(cli_fd);
			pthread_mutex_destroy(&c->lock);
			free(c);
			continue;
		}
		pthread_detach(tid);
	}

	close(srv);
	return NULL;
}
