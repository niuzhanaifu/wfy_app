/* system_service — single boot-time daemon for TAISHAN PAI, fanning
 * out into six independent pthreads:
 *
 *   time_sync_thread    time zone + NTP + RTC write-back    (time_sync.c)
 *   uart_log_thread     /dev/kmsg -> /blackbox/uart_log/    (uart_log.c)
 *   sys_stat_thread     load/mem  -> /blackbox/sys_stat/    (sys_stat.c)
 *   ble_gatt_thread     BlueZ GATT server (NUS over BLE)    (ble_gatt.c)
 *   wifi_switch_thread  boot-time STA+AP switch workflow    (wifi_switch.c)
 *   unix_ipc_thread     IPC server on /run/system_service.sock,
 *                       same 0x55AA protocol as BLE — used by
 *                       ovita-server (Rust) to delegate wifi
 *                       scan / connect / AP-config without
 *                       duplicating the implementation        (unix_ipc.c)
 *
 * Each feature is self-contained: its own state, its own loop, its own
 * resources. main() just spawns and waits — SIGTERM from start-stop-daemon
 * kills the whole process and all threads. */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "main"
#include "log.h"

#include "time_sync.h"
#include "uart_log.h"
#include "sys_stat.h"
#include "ble_gatt.h"
#include "wifi_switch.h"
#include "unix_ipc.h"

int main(int argc, char **argv)
{
	pthread_t th_time, th_uart, th_stat, th_ble, th_wifi, th_ipc;
	int i, debug = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-debug") == 0) {
			debug = 1;
		} else {
			fprintf(stderr,
			        "system_service: unknown argument '%s'\n"
			        "usage: %s [-debug]\n",
			        argv[i], argv[0]);
			return 1;
		}
	}

	/* IMPORTANT: initialise the log backend BEFORE spawning any
	 * worker thread. Otherwise early LOGI/LOGE calls from those
	 * threads would fall through to stderr (= /dev/console when
	 * launched by S99system_service), bypassing the rotated log
	 * file and being effectively lost. */
	if (log_init(debug) != 0) {
		/* Non-fatal — log_init already warned on stderr and set
		 * up a stderr fallback. Keep going. */
	}

	LOGI("system_service starting (%s mode) — spawning six worker threads",
	     debug ? "debug/stderr" : "file/rotated");

	pthread_create(&th_time, NULL, time_sync_thread,   NULL);
	pthread_create(&th_uart, NULL, uart_log_thread,    NULL);
	pthread_create(&th_stat, NULL, sys_stat_thread,    NULL);
	pthread_create(&th_ble,  NULL, ble_gatt_thread,    NULL);
	pthread_create(&th_wifi, NULL, wifi_switch_thread, NULL);
	pthread_create(&th_ipc,  NULL, unix_ipc_thread,    NULL);

	/* Threads run forever. Join so main() never returns and the process
	 * stays resident until start-stop-daemon sends SIGTERM. */
	pthread_join(th_time, NULL);
	pthread_join(th_uart, NULL);
	pthread_join(th_stat, NULL);
	pthread_join(th_ble,  NULL);
	pthread_join(th_wifi, NULL);
	pthread_join(th_ipc,  NULL);

	while (1) {
		sleep(5);
	}

	return 0;
}
