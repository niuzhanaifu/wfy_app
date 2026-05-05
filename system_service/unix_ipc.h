#ifndef SYSTEM_SERVICE_UNIX_IPC_H
#define SYSTEM_SERVICE_UNIX_IPC_H

/* IPC server thread: listens on /run/system_service.sock and accepts
 * connections from local processes (notably ovita-server). Each
 * connection speaks the same 0x55AA protocol used over BLE NUS — the
 * codec, command handlers, and async workers are all shared via
 * proto_dispatch_buffer().
 *
 * The socket is mode 0666 so a non-root daemon (ovita-server) can
 * connect; tighten via SO_PEERCRED if you ever care. Listen backlog 4
 * is plenty (we expect at most one or two clients at a time). */
void *unix_ipc_thread(void *arg);

#endif
