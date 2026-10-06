#ifndef BLE_ENROLL_H
#define BLE_ENROLL_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

/*
 * Bluetooth LE enrollment service.
 *
 * The phone writes {"employee_id":5,"finger_index":1} to the
 * command characteristic and subscribes to the status
 * characteristic for progress updates. Both characteristics
 * require an authenticated (passkey) pairing; the passkey is
 * shown on the TFT, so only someone standing at the terminal
 * can pair a phone.
 */

#define BLE_DEVICE_NAME  "CBIO-ENTRY-001"

typedef struct {
    long employee_id;
    long finger_index;
} enroll_request_t;

bool ble_enroll_init(void);

/* True when the phone has queued an enrollment request */
bool ble_enroll_request_pending(void);

bool ble_enroll_take_request(enroll_request_t *request, TickType_t wait);

/*
 * True once after the BLE layer drew over the screen (passkey)
 * or the pairing finished, so the caller should redraw.
 */
bool ble_enroll_take_redraw(void);

/* Sends a JSON status update to the connected phone, if any */
void ble_enroll_notify(const char *json);

#endif /* BLE_ENROLL_H */
