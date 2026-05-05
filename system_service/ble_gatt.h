#ifndef SYSTEM_SERVICE_BLE_GATT_H
#define SYSTEM_SERVICE_BLE_GATT_H

#include <stddef.h>
#include <stdint.h>

/* GATT server thread: registers a Nordic-UART-Service-style GATT app with
 * BlueZ over D-Bus, advertises as "TAISHAN-PAI", accepts Just-Works
 * pairing, and echoes whatever the phone writes on RX back via TX notify. */
void *ble_gatt_thread(void *arg);

/* Push bytes to connected phones that have subscribed to TX notifications.
 * Safe to call from any thread once ble_gatt_thread has initialized.
 * Silently drops if no subscriber or the stack isn't up yet (logged). */
void ble_gatt_send(const uint8_t *data, size_t len);

#endif
