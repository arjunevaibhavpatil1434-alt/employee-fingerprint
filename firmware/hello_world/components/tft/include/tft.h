#ifndef TFT_H
#define TFT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool tft_init(void);

/* Only stores the flag; the header icon updates on the next redraw */
void tft_set_wifi_status(bool connected);

/* True when the minute shown on the idle screen's clock is out of date */
bool tft_clock_changed(void);

void tft_show_startup(void);
void tft_show_ready(void);
void tft_show_place_finger(void);
void tft_show_processing(void);
/* name may be NULL or empty (server unreachable): the ID is shown instead */
void tft_show_granted(const char *name, uint16_t id, uint16_t score);
/*
 * Attendance punch. in: punched in (else out); duplicate: a repeat
 * scan within a minute; detail: e.g. "Out at 18:01"; worked: e.g.
 * "8h 16m today" or empty.
 */
void tft_show_punch(const char *name, bool in, bool duplicate,
                    const char *detail, const char *worked);

/* reason is shown in the card; NULL shows "Not recognised" */
void tft_show_denied(const char *reason);

/* Bluetooth pairing code the phone user must type in */
void tft_show_passkey(uint32_t passkey);

/* step: 1 = first scan, 2 = second scan, 3 = saving */
void tft_show_enroll_step(const char *name, int step,
                          const char *instruction, const char *hint);

void tft_show_enroll_result(bool ok, const char *name, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* TFT_H */
