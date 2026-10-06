
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include "fonts.h"

#define TFT_WIDTH       240
#define TFT_HEIGHT      320

#define TFT_SCLK        18
#define TFT_MOSI        23
#define TFT_MISO        19

#define TFT_CS           5
#define TFT_DC          21
#define TFT_RESET       22

#define SPI_HOST         SPI2_HOST

static const char *TAG = "TFT";

static spi_device_handle_t tft;

/* ----------------------------------------------------------
 * SPI write helper
 * ---------------------------------------------------------- */

static void tft_spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = data,
    };

    ESP_ERROR_CHECK(spi_device_transmit(tft, &trans));
}

/* ----------------------------------------------------------
 * Send command
 * ---------------------------------------------------------- */

static void tft_command(uint8_t command)
{
    gpio_set_level(TFT_DC, 0);
    tft_spi_write(&command, 1);
}

/* ----------------------------------------------------------
 * Send data
 * ---------------------------------------------------------- */

static void tft_data(const uint8_t *data, size_t len)
{
    gpio_set_level(TFT_DC, 1);
    tft_spi_write(data, len);
}

/* ----------------------------------------------------------
 * Send one command followed by data
 * ---------------------------------------------------------- */

static void tft_command_data(uint8_t command,
                             const uint8_t *data,
                             size_t len)
{
    tft_command(command);

    if (len > 0) {
        tft_data(data, len);
    }
}

/* ----------------------------------------------------------
 * Hardware reset
 * ---------------------------------------------------------- */

static void tft_reset(void)
{
    ESP_LOGI(TAG, "Resetting TFT...");

    gpio_set_level(TFT_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(TFT_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* ----------------------------------------------------------
 * ILI9341 initialization
 * ---------------------------------------------------------- */

static void ili9341_init(void)
{
    uint8_t data[16];

    ESP_LOGI(TAG, "Initializing ILI9341...");

    /* Software reset */
    tft_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Power control A */
    data[0] = 0x39;
    data[1] = 0x2C;
    data[2] = 0x00;
    data[3] = 0x34;
    data[4] = 0x02;
    tft_command_data(0xCB, data, 5);

    /* Power control B */
    data[0] = 0x00;
    data[1] = 0xC1;
    data[2] = 0x30;
    tft_command_data(0xCF, data, 3);

    /* Driver timing control A */
    data[0] = 0x85;
    data[1] = 0x00;
    data[2] = 0x78;
    tft_command_data(0xE8, data, 3);

    /* Driver timing control B */
    data[0] = 0x00;
    data[1] = 0x00;
    tft_command_data(0xEA, data, 2);

    /* Power on sequence */
    data[0] = 0x64;
    data[1] = 0x03;
    data[2] = 0x12;
    data[3] = 0x81;
    tft_command_data(0xED, data, 4);

    /* Pump ratio */
    data[0] = 0x20;
    tft_command_data(0xF7, data, 1);

    /* Power control 1 */
    data[0] = 0x23;
    tft_command_data(0xC0, data, 1);

    /* Power control 2 */
    data[0] = 0x10;
    tft_command_data(0xC1, data, 1);

    /* VCOM control 1 */
    data[0] = 0x3E;
    data[1] = 0x28;
    tft_command_data(0xC5, data, 2);

    /* VCOM control 2 */
    data[0] = 0x86;
    tft_command_data(0xC7, data, 1);

    /* Memory access control */
    data[0] = 0x48;
    tft_command_data(0x36, data, 1);

    /* Pixel format = 16-bit RGB565 */
    data[0] = 0x55;
    tft_command_data(0x3A, data, 1);

    /* Frame rate control */
    data[0] = 0x00;
    data[1] = 0x18;
    tft_command_data(0xB1, data, 2);

    /* Display function control */
    data[0] = 0x08;
    data[1] = 0x82;
    data[2] = 0x27;
    tft_command_data(0xB6, data, 3);

    /* 3Gamma function disable */
    data[0] = 0x00;
    tft_command_data(0xF2, data, 1);

    /* Gamma function */
    data[0] = 0x01;
    tft_command_data(0x26, data, 1);

    /* Positive gamma correction */
    uint8_t gamma_positive[] = {
        0x0F, 0x31, 0x2B, 0x0C,
        0x0E, 0x08, 0x4E, 0xF1,
        0x37, 0x07, 0x10, 0x03,
        0x0E, 0x09, 0x00
    };

    tft_command_data(0xE0,
                     gamma_positive,
                     sizeof(gamma_positive));

    /* Negative gamma correction */
    uint8_t gamma_negative[] = {
        0x00, 0x0E, 0x14, 0x03,
        0x11, 0x07, 0x31, 0xC1,
        0x48, 0x08, 0x0F, 0x0C,
        0x31, 0x36, 0x0F
    };

    tft_command_data(0xE1,
                     gamma_negative,
                     sizeof(gamma_negative));

    /* Exit sleep */
    tft_command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Display ON */
    tft_command(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "TFT initialization complete");
}

/* ----------------------------------------------------------
 * Set drawing window
 * ---------------------------------------------------------- */

static void tft_set_window(uint16_t x0,
                           uint16_t y0,
                           uint16_t x1,
                           uint16_t y1)
{
    uint8_t data[4];

    /* Column address */
    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;

    tft_command_data(0x2A, data, 4);

    /* Page address */
    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;

    tft_command_data(0x2B, data, 4);

    /* Memory write */
    tft_command(0x2C);
}

/* ----------------------------------------------------------
 * Strip renderer
 *
 * A full 240x320 framebuffer needs 150 KB, which does not fit
 * next to Wi-Fi. Instead every screen is drawn by a "scene"
 * function that is called once per 16-row strip; primitives
 * only touch pixels that fall inside the current strip, and
 * each finished strip is sent in a single SPI transfer.
 *
 * Shapes and text are anti-aliased by blending into the strip.
 * ---------------------------------------------------------- */

#define STRIP_ROWS      16

#define RGB(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* Palette */
#define C_BG_TOP        RGB(0x10, 0x1C, 0x40)
#define C_BG_BOTTOM     RGB(0x04, 0x08, 0x16)
#define C_CARD          RGB(0x15, 0x22, 0x48)
#define C_CARD_LINE     RGB(0x26, 0x36, 0x66)
#define C_FAINT         RGB(0x23, 0x32, 0x5E)
#define C_TEXT          RGB(0xF1, 0xF5, 0xF9)
#define C_MUTED         RGB(0x8F, 0x9F, 0xC4)
#define C_WHITE         0xFFFF
#define C_CYAN          RGB(0x22, 0xD3, 0xEE)
#define C_INDIGO        RGB(0x81, 0x8C, 0xF8)
#define C_BLUE          RGB(0x3B, 0x82, 0xF6)
#define C_GREEN         RGB(0x22, 0xC5, 0x5E)
#define C_RED           RGB(0xEF, 0x44, 0x44)
#define C_AMBER         RGB(0xF5, 0x9E, 0x0B)

#define HEADER_H        44
#define CX              (TFT_WIDTH / 2)

#define PI_F            3.14159265f

/* Stored big-endian, the byte order the ILI9341 expects */
static uint16_t strip[TFT_WIDTH * STRIP_ROWS];
static int strip_y0;
static int strip_y1;

static volatile bool wifi_connected;

/* render() and the scene data are shared by the main and BLE tasks */
static SemaphoreHandle_t draw_lock;

static void lock_display(void)
{
    if (draw_lock != NULL) {
        xSemaphoreTake(draw_lock, portMAX_DELAY);
    }
}

static void unlock_display(void)
{
    if (draw_lock != NULL) {
        xSemaphoreGive(draw_lock);
    }
}

static inline uint16_t swap16(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

/* fg over bg with alpha 0..255 */
static inline uint16_t mix(uint16_t fg, uint16_t bg, int a)
{
    int na = 255 - a;
    int r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * na) / 255;
    int g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * na) / 255;
    int b = ((fg & 0x1F) * a + (bg & 0x1F) * na) / 255;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Coverage 0..255 of a pixel whose centre is `d` px inside an edge */
static inline int coverage(float d)
{
    if (d >= 0.5f) {
        return 255;
    }

    if (d <= -0.5f) {
        return 0;
    }

    return (int)((d + 0.5f) * 255.0f);
}

/* Clip a row range to the current strip; false if nothing is left */
static inline bool clip_rows(int *y0, int *y1)
{
    if (*y0 < strip_y0) {
        *y0 = strip_y0;
    }

    if (*y1 > strip_y1) {
        *y1 = strip_y1;
    }

    return *y0 < *y1;
}

static inline void blend_pixel(int x, int y, uint16_t color, int alpha)
{
    if (alpha <= 0 || x < 0 || x >= TFT_WIDTH ||
        y < strip_y0 || y >= strip_y1) {
        return;
    }

    uint16_t *p = &strip[(y - strip_y0) * TFT_WIDTH + x];

    *p = alpha >= 255 ? swap16(color)
                      : swap16(mix(color, swap16(*p), alpha));
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int y0 = y;
    int y1 = y + h;

    if (!clip_rows(&y0, &y1)) {
        return;
    }

    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > TFT_WIDTH ? TFT_WIDTH : (x + w);
    uint16_t c = swap16(color);

    for (int py = y0; py < y1; py++) {
        uint16_t *row = &strip[(py - strip_y0) * TFT_WIDTH];

        for (int px = x0; px < x1; px++) {
            row[px] = c;
        }
    }
}

/* Rounded rectangle with smooth corners, optional alpha */
static void fill_round_rect_a(int x, int y, int w, int h, int r,
                              uint16_t color, int alpha)
{
    int y0 = y;
    int y1 = y + h;

    if (!clip_rows(&y0, &y1)) {
        return;
    }

    const float hx = w / 2.0f;
    const float hy = h / 2.0f;
    const float mx = x + hx;
    const float my = y + hy;

    for (int py = y0; py < y1; py++) {
        for (int px = x; px < x + w; px++) {

            /* Signed distance to the rounded box */
            float qx = fabsf(px + 0.5f - mx) - (hx - r);
            float qy = fabsf(py + 0.5f - my) - (hy - r);
            float d;

            if (qx > 0.0f && qy > 0.0f) {
                d = sqrtf(qx * qx + qy * qy) - r;
            } else {
                d = (qx > qy ? qx : qy) - r;
            }

            blend_pixel(px, py, color, coverage(-d) * alpha / 255);
        }
    }
}

static void fill_round_rect(int x, int y, int w, int h, int r,
                            uint16_t color)
{
    fill_round_rect_a(x, y, w, h, r, color, 255);
}

/* Card: filled rounded box with a 1 px outline */
static void draw_card(int x, int y, int w, int h, int r)
{
    fill_round_rect(x, y, w, h, r, C_CARD_LINE);
    fill_round_rect(x + 1, y + 1, w - 2, h - 2, r - 1, C_CARD);
}

/*
 * Angle of (dx, dy) around a centre: 0 points straight down,
 * +/-PI points straight up (screen y grows downwards).
 */
static inline float angle_of(float dx, float dy)
{
    return atan2f(dx, dy);
}

static inline float angle_diff(float a, float b)
{
    float d = a - b;

    while (d > PI_F) {
        d -= 2.0f * PI_F;
    }

    while (d < -PI_F) {
        d += 2.0f * PI_F;
    }

    return fabsf(d);
}

/*
 * Smooth ring between radius (r - thickness) and r. When half_span
 * is below PI only the arc within half_span of `center_angle` is
 * drawn.
 */
static void draw_arc(float cx, float cy, float r, float thickness,
                     float center_angle, float half_span,
                     uint16_t color)
{
    int y0 = (int)(cy - r - 1);
    int y1 = (int)(cy + r + 2);

    if (!clip_rows(&y0, &y1)) {
        return;
    }

    const float r_in = r - thickness;

    for (int py = y0; py < y1; py++) {
        float dy = py + 0.5f - cy;

        for (int px = (int)(cx - r - 1); px <= (int)(cx + r + 1); px++) {
            float dx = px + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);

            int a_out = coverage(r - d);
            int a_in = r_in > 0.0f ? coverage(d - r_in) : 255;
            int a = a_out < a_in ? a_out : a_in;

            if (a == 0) {
                continue;
            }

            if (half_span < PI_F &&
                angle_diff(angle_of(dx, dy), center_angle) > half_span) {
                continue;
            }

            blend_pixel(px, py, color, a);
        }
    }
}

static void draw_ring(float cx, float cy, float r, float thickness,
                      uint16_t color)
{
    draw_arc(cx, cy, r, thickness, 0.0f, PI_F, color);
}

static void fill_circle(float cx, float cy, float r, uint16_t color)
{
    draw_arc(cx, cy, r, r + 1.0f, 0.0f, PI_F, color);
}

/* Soft halo fading from `alpha` at r_in to nothing at r_out */
static void draw_glow(float cx, float cy, float r_in, float r_out,
                      uint16_t color, int alpha)
{
    int y0 = (int)(cy - r_out);
    int y1 = (int)(cy + r_out + 1);

    if (!clip_rows(&y0, &y1)) {
        return;
    }

    for (int py = y0; py < y1; py++) {
        float dy = py + 0.5f - cy;

        for (int px = (int)(cx - r_out); px <= (int)(cx + r_out); px++) {
            float dx = px + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);

            if (d >= r_out) {
                continue;
            }

            float t = d <= r_in ? 1.0f : 1.0f - (d - r_in) / (r_out - r_in);

            blend_pixel(px, py, color, (int)(alpha * t * t));
        }
    }
}

/* Smooth line with round caps, `width` pixels thick */
static void draw_thick_line(float x0, float y0, float x1, float y1,
                            float width, uint16_t color)
{
    float half = width / 2.0f;

    int top = (int)((y0 < y1 ? y0 : y1) - half - 1);
    int bottom = (int)((y0 > y1 ? y0 : y1) + half + 2);
    int left = (int)((x0 < x1 ? x0 : x1) - half - 1);
    int right = (int)((x0 > x1 ? x0 : x1) + half + 1);

    if (!clip_rows(&top, &bottom)) {
        return;
    }

    float vx = x1 - x0;
    float vy = y1 - y0;
    float len2 = vx * vx + vy * vy;

    for (int py = top; py < bottom; py++) {
        for (int px = left; px <= right; px++) {

            float wx = px + 0.5f - x0;
            float wy = py + 0.5f - y0;

            float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;

            if (t < 0.0f) {
                t = 0.0f;
            } else if (t > 1.0f) {
                t = 1.0f;
            }

            float ex = wx - t * vx;
            float ey = wy - t * vy;

            blend_pixel(px, py, color,
                        coverage(half - sqrtf(ex * ex + ey * ey)));
        }
    }
}

/*
 * Stylised fingerprint: concentric, slightly tall elliptical
 * ridges that open at the bottom, with a few breaks so it does
 * not look like a plain target.
 */
static void draw_fingerprint(float cx, float cy, float radius,
                             uint16_t color)
{
    enum { RIDGES = 6 };

    const float stretch = 1.25f;
    const float spacing = radius / RIDGES;
    const float ridge_half = spacing * 0.26f;

    /* Bottom opening (radians either side of straight down) */
    static const float bottom_gap[RIDGES] = {
        0.0f, 0.55f, 0.45f, 0.60f, 0.75f, 0.95f
    };

    int y0 = (int)(cy - radius * stretch - 2);
    int y1 = (int)(cy + radius * stretch + 3);

    if (!clip_rows(&y0, &y1)) {
        return;
    }

    for (int py = y0; py < y1; py++) {
        for (int px = (int)(cx - radius - 2); px <= (int)(cx + radius + 2); px++) {

            float dx = px + 0.5f - cx;
            float dy = (py + 0.5f - cy) / stretch;
            float r = sqrtf(dx * dx + dy * dy);

            int k = (int)(r / spacing);

            if (k >= RIDGES) {
                continue;
            }

            float centre = spacing * (k + 0.5f);
            int a = coverage(ridge_half - fabsf(r - centre));

            if (k == 0) {
                /* Core: a small solid dot */
                a = coverage(ridge_half * 1.4f - r);
            }

            if (a == 0) {
                continue;
            }

            float ang = angle_of(dx, dy);

            if (fabsf(ang) < bottom_gap[k]) {
                continue;
            }

            /* Small breaks that give the ridges a natural look */
            if ((k == 2 && ang > 1.9f && ang < 2.3f) ||
                (k == 3 && ang < -1.6f && ang > -2.0f) ||
                (k == 4 && ang > 2.6f && ang < 2.85f) ||
                (k == 5 && ang < -2.5f && ang > -2.75f)) {
                continue;
            }

            blend_pixel(px, py, color, a);
        }
    }
}

/* ----------------------------------------------------------
 * Text (anti-aliased fonts from fonts.h)
 * ---------------------------------------------------------- */

static const glyph_t *glyph_of(const font_t *font, char c)
{
    if (c < font->first || c > font->last) {
        return NULL;
    }

    return &font->glyphs[c - font->first];
}

static int text_width(const char *text, const font_t *font)
{
    int w = 0;

    for (; *text; text++) {
        const glyph_t *g = glyph_of(font, *text);

        if (g != NULL) {
            w += g->advance;
        }
    }

    return w;
}

/* `y` is the top of the line box */
static void draw_text(int x, int y, const char *text,
                      const font_t *font, uint16_t color)
{
    if (y + font->height <= strip_y0 || y >= strip_y1) {
        return;
    }

    for (; *text; text++) {
        const glyph_t *g = glyph_of(font, *text);

        if (g == NULL) {
            continue;
        }

        const uint8_t *bits = font->bitmap + g->offset;
        const int stride = (g->width + 1) / 2;
        const int gx = x + g->x_offset;
        const int gy = y + g->y_offset;

        for (int row = 0; row < g->height; row++) {

            int py = gy + row;

            if (py < strip_y0 || py >= strip_y1) {
                continue;
            }

            for (int col = 0; col < g->width; col++) {
                uint8_t byte = bits[row * stride + col / 2];
                int a4 = (col & 1) ? (byte & 0x0F) : (byte >> 4);

                blend_pixel(gx + col, py, color, a4 * 17);
            }
        }

        x += g->advance;
    }
}

static void draw_text_centered(int y, const char *text,
                               const font_t *font, uint16_t color)
{
    draw_text(CX - text_width(text, font) / 2, y, text, font, color);
}

/* Copies `src`, cutting it with "..." so it fits `max_w` pixels */
static void fit_text(char *dst, size_t dst_len, const char *src,
                     const font_t *font, int max_w)
{
    snprintf(dst, dst_len, "%s", src);

    size_t n = strlen(dst);

    if (text_width(dst, font) <= max_w) {
        return;
    }

    while (n > 0) {
        n--;

        /* Do not leave a trailing space before the dots */
        while (n > 0 && dst[n - 1] == ' ') {
            n--;
        }

        if (n + 4 > dst_len) {
            continue;
        }

        memcpy(dst + n, "...", 4);

        if (text_width(dst, font) <= max_w) {
            return;
        }
    }
}

/*
 * Word-wraps `text` onto up to `max_lines` centred lines starting
 * at `y`; the last line is cut with "..." if the text is longer.
 * With draw == false it only counts the lines.
 */
static int wrap_centered(int y, const char *text, const font_t *font,
                         uint16_t color, int max_w, int max_lines,
                         bool draw)
{
    char line[64];
    int l = 0;

    for (; l < max_lines && *text != '\0'; l++) {

        int len = (int)strlen(text);
        int take = len < (int)sizeof(line) - 1 ? len : (int)sizeof(line) - 1;

        /* Longest prefix that fits, broken at a space when possible */
        for (;;) {
            memcpy(line, text, take);
            line[take] = '\0';

            if (text_width(line, font) <= max_w || take <= 1) {
                break;
            }

            int space = take - 1;

            while (space > 0 && text[space] != ' ') {
                space--;
            }

            take = space > 0 ? space : take - 1;
        }

        if (l == max_lines - 1 && take < len) {
            fit_text(line, sizeof(line), text, font, max_w);
        }

        if (draw) {
            draw_text_centered(y + l * font->height, line, font, color);
        }

        text += take;

        while (*text == ' ') {
            text++;
        }
    }

    return l;
}

/* Card holding a wrapped message, plus an optional muted subtitle */
static void draw_message_card(int y, int h, const char *message,
                              const char *subtitle)
{
    const int max_w = TFT_WIDTH - 56;
    const int max_lines = 3;

    draw_card(14, y, TFT_WIDTH - 28, h, 14);

    int lines = wrap_centered(0, message, &font_body, 0, max_w,
                              max_lines, false);

    /* The subtitle only fits under a single-line message */
    bool show_sub = subtitle != NULL && lines == 1;
    int block = lines * font_body.height +
                (show_sub ? font_small.height + 2 : 0);
    int top = y + (h - block) / 2;

    wrap_centered(top, message, &font_body, C_TEXT, max_w, max_lines, true);

    if (show_sub) {
        draw_text_centered(top + font_body.height + 2, subtitle,
                           &font_small, C_MUTED);
    }
}

/* ----------------------------------------------------------
 * Clock (time comes from SNTP once Wi-Fi is up)
 * ---------------------------------------------------------- */

static int shown_minute = -1;

static bool local_time(struct tm *tm)
{
    time_t now = time(NULL);

    localtime_r(&now, tm);

    /* Before the first SNTP sync the clock starts in 1970 */
    return tm->tm_year + 1900 >= 2024;
}

static int minute_of_day(void)
{
    struct tm tm;

    return local_time(&tm) ? tm.tm_hour * 60 + tm.tm_min : -1;
}

/* ----------------------------------------------------------
 * Shared screen furniture
 * ---------------------------------------------------------- */

static void draw_wifi_icon(float cx, float base_y, bool connected)
{
    uint16_t color = connected ? C_CYAN : C_MUTED;

    for (int i = 1; i <= 3; i++) {
        draw_arc(cx, base_y, i * 5.0f + 1.0f, 2.0f, PI_F, 0.78f, color);
    }

    fill_circle(cx, base_y - 1.0f, 2.0f, color);

    if (!connected) {
        draw_thick_line(cx - 8, base_y - 15, cx + 8, base_y + 1, 2.5f, C_RED);
    }
}

static void draw_header(void)
{
    /* App mark: fingerprint in a rounded tile */
    fill_round_rect(12, 9, 26, 26, 8, C_CARD_LINE);
    draw_fingerprint(25, 22, 9, C_CYAN);

    draw_text(46, 12, "Employees Access", &font_body, C_TEXT);

    draw_wifi_icon(TFT_WIDTH - 22, 29, wifi_connected);

    fill_rect(0, HEADER_H - 1, TFT_WIDTH, 1, C_FAINT);
}

/* Fingerprint inside soft halo rings, used by the scan screens */
static void draw_scanner(int cy, int r, uint16_t accent)
{
    draw_glow(CX, cy, r * 0.6f, r * 1.35f, accent, 70);

    fill_circle(CX, cy, r, C_CARD);
    draw_ring(CX, cy, r, 1.5f, C_CARD_LINE);
    draw_ring(CX, cy, r + 10, 1.0f, C_FAINT);

    /* Bright segments on the outer ring, like a scanning sweep */
    draw_arc(CX, cy, r + 10, 3.0f, -2.4f, 0.55f, accent);
    draw_arc(CX, cy, r + 10, 3.0f, 0.75f, 0.55f, accent);

    draw_fingerprint(CX, cy, r * 0.62f, accent);
}

/* Round badge with a tick or a cross */
static void draw_badge(int cy, bool ok)
{
    uint16_t color = ok ? C_GREEN : C_RED;

    draw_glow(CX, cy, 40, 92, color, 90);
    fill_circle(CX, cy, 48, color);
    draw_ring(CX, cy, 58, 2.0f, color);

    if (ok) {
        draw_thick_line(CX - 21, cy + 1, CX - 6, cy + 16, 9, C_WHITE);
        draw_thick_line(CX - 6, cy + 16, CX + 22, cy - 14, 9, C_WHITE);
    } else {
        draw_thick_line(CX - 17, cy - 17, CX + 17, cy + 17, 9, C_WHITE);
        draw_thick_line(CX + 17, cy - 17, CX - 17, cy + 17, 9, C_WHITE);
    }
}

/* Circle with up to two initials */
static void draw_avatar(int cx, int cy, const char *name, uint16_t color)
{
    char initials[3] = "";
    int n = 0;
    bool word_start = true;

    for (const char *p = name; *p && n < 2; p++) {
        if (*p == ' ') {
            word_start = true;
        } else if (word_start) {
            initials[n++] = (char)toupper((unsigned char)*p);
            word_start = false;
        }
    }

    initials[n] = '\0';

    fill_circle(cx, cy, 22, mix(color, C_CARD, 60));
    draw_ring(cx, cy, 22, 1.5f, color);
    draw_text(cx - text_width(initials, &font_body) / 2,
              cy - font_body.height / 2, initials, &font_body, color);
}

/* Pill-shaped label at the bottom of the scan screens */
static void draw_pill(int y, const char *text, uint16_t dot)
{
    int w = text_width(text, &font_body) + 46;

    fill_round_rect(CX - w / 2, y, w, 34, 17, C_CARD_LINE);
    fill_round_rect(CX - w / 2 + 1, y + 1, w - 2, 32, 16, C_CARD);
    fill_circle(CX - w / 2 + 20, y + 17, 4, dot);
    draw_text(CX - w / 2 + 32, y + 6, text, &font_body, C_TEXT);
}

static void render(void (*scene)(void))
{
    for (int y = 0; y < TFT_HEIGHT; y += STRIP_ROWS) {

        strip_y0 = y;
        strip_y1 = y + STRIP_ROWS;

        if (strip_y1 > TFT_HEIGHT) {
            strip_y1 = TFT_HEIGHT;
        }

        /* Vertical gradient background */
        for (int py = strip_y0; py < strip_y1; py++) {
            uint16_t c = swap16(mix(C_BG_BOTTOM, C_BG_TOP,
                                    py * 255 / (TFT_HEIGHT - 1)));
            uint16_t *row = &strip[(py - strip_y0) * TFT_WIDTH];

            for (int px = 0; px < TFT_WIDTH; px++) {
                row[px] = c;
            }
        }

        scene();

        tft_set_window(0, strip_y0, TFT_WIDTH - 1, strip_y1 - 1);
        tft_data((const uint8_t *)strip,
                 TFT_WIDTH * (strip_y1 - strip_y0) * sizeof(uint16_t));
    }
}

/* ----------------------------------------------------------
 * Initialize SPI
 * ---------------------------------------------------------- */

static void tft_spi_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI...");

    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << TFT_DC) |
            (1ULL << TFT_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(TFT_DC, 1);
    gpio_set_level(TFT_RESET, 1);

    spi_bus_config_t bus_config = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(strip),
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            SPI_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO
        )
    );

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_CS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(
        spi_bus_add_device(
            SPI_HOST,
            &dev_config,
            &tft
        )
    );

    ESP_LOGI(TAG, "SPI initialized");
}

/* ----------------------------------------------------------
 * Screens
 * ---------------------------------------------------------- */

static void scene_startup(void)
{
    draw_glow(CX, 128, 30, 100, C_CYAN, 80);
    fill_circle(CX, 128, 56, C_CARD);
    draw_ring(CX, 128, 56, 1.5f, C_CARD_LINE);
    draw_fingerprint(CX, 128, 36, C_CYAN);

    draw_text_centered(208, "Employees Access", &font_title, C_TEXT);
    draw_text_centered(242, "Starting up...", &font_small, C_MUTED);

    fill_round_rect(60, 276, 120, 6, 3, C_FAINT);
    fill_round_rect(60, 276, 44, 6, 3, C_CYAN);
}

/* Idle screen: clock, date and the scanner */
static void scene_idle(void)
{
    static const char *const days[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    static const char *const months[] = {
        "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December"
    };

    struct tm tm;

    draw_header();

    if (local_time(&tm)) {
        char clock[8];
        char date[40];

        snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(date, sizeof(date), "%s, %d %s",
                 days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);

        draw_text_centered(46, clock, &font_huge, C_TEXT);
        draw_text_centered(118, date, &font_small, C_MUTED);
    } else {
        draw_text_centered(70, "Welcome", &font_title, C_TEXT);
        draw_text_centered(104, "Scan your finger to check in",
                           &font_small, C_MUTED);
    }

    draw_scanner(204, 44, C_CYAN);
    draw_pill(272, "Place your finger", C_GREEN);
}

static void scene_processing(void)
{
    draw_header();

    draw_scanner(150, 58, C_AMBER);

    draw_text_centered(236, "Verifying", &font_title, C_TEXT);
    draw_text_centered(268, "Hold your finger still", &font_small, C_MUTED);

    for (int i = 0; i < 3; i++) {
        fill_circle(CX - 16 + i * 16, 298, 3.5f,
                    i == 1 ? C_AMBER : C_FAINT);
    }
}

/* Filled in by tft_show_granted() before rendering */
static char granted_name[48];
static char granted_details[40];

static void scene_granted(void)
{
    char name[48];

    draw_header();
    draw_badge(116, true);

    draw_text_centered(184, "Access granted", &font_title, C_GREEN);

    draw_card(14, 226, TFT_WIDTH - 28, 74, 14);
    draw_avatar(52, 263, granted_name, C_GREEN);

    fit_text(name, sizeof(name), granted_name, &font_body,
             TFT_WIDTH - 28 - 74 - 12);
    draw_text(88, 240, name, &font_body, C_TEXT);
    draw_text(88, 264, granted_details, &font_small, C_MUTED);
}

/* Filled in by tft_show_punch() */
static bool punch_in;
static bool punch_duplicate;
static char punch_name[48];
static char punch_detail[48];
static char punch_worked[32];

/* Arrow going into (in) or out of (out) a door frame */
static void draw_punch_badge(int cy, bool in, uint16_t color)
{
    draw_glow(CX, cy, 40, 92, color, 90);
    fill_circle(CX, cy, 48, color);
    draw_ring(CX, cy, 58, 2.0f, color);

    /* Door frame on the right, open at the left */
    draw_thick_line(CX + 8, cy - 22, CX + 22, cy - 22, 6, C_WHITE);
    draw_thick_line(CX + 22, cy - 22, CX + 22, cy + 22, 6, C_WHITE);
    draw_thick_line(CX + 22, cy + 22, CX + 8, cy + 22, 6, C_WHITE);

    /* Arrow: towards the door for IN, away from it for OUT */
    float tail = in ? CX - 26 : CX + 6;
    float head = in ? CX + 6 : CX - 26;
    float dir = in ? 1.0f : -1.0f;

    draw_thick_line(tail, cy, head, cy, 6, C_WHITE);
    draw_thick_line(head, cy, head - dir * 12, cy - 12, 6, C_WHITE);
    draw_thick_line(head, cy, head - dir * 12, cy + 12, 6, C_WHITE);
}

static void scene_punch(void)
{
    uint16_t color = punch_duplicate ? C_AMBER : punch_in ? C_GREEN : C_INDIGO;
    const char *title = punch_duplicate
        ? (punch_in ? "Already in" : "Already out")
        : (punch_in ? "Punched in" : "Punched out");
    char name[48];

    draw_header();
    draw_punch_badge(116, punch_in, color);

    draw_text_centered(184, title, &font_title, color);

    draw_card(14, 226, TFT_WIDTH - 28, 74, 14);
    draw_avatar(52, 263, punch_name, color);

    const int text_w = TFT_WIDTH - 28 - 74 - 12;
    bool two_lines = punch_worked[0] != '\0';
    int y = two_lines ? 234 : 240;
    char line[48];

    fit_text(name, sizeof(name), punch_name, &font_body, text_w);
    draw_text(88, y, name, &font_body, C_TEXT);

    fit_text(line, sizeof(line), punch_detail, &font_small, text_w);
    draw_text(88, y + 22, line, &font_small, C_MUTED);

    if (two_lines) {
        fit_text(line, sizeof(line), punch_worked, &font_small, text_w);
        draw_text(88, y + 40, line, &font_small, color);
    }
}

static char denied_reason[64] = "Please try again";

static void scene_denied(void)
{
    draw_header();
    draw_badge(116, false);

    draw_text_centered(184, "Access denied", &font_title, C_RED);

    draw_message_card(222, 84, denied_reason, "Please try again");
}

static char passkey_text[8];

static void scene_passkey(void)
{
    draw_header();

    draw_glow(CX, 104, 26, 76, C_BLUE, 90);
    fill_circle(CX, 104, 36, C_BLUE);

    /* Bluetooth rune */
    const float h = 40.0f;
    const float a = h / 4.0f;
    const float w = 4.0f;

    draw_thick_line(CX, 104 - h / 2, CX, 104 + h / 2, w, C_WHITE);
    draw_thick_line(CX - a, 104 - a, CX + a, 104 + a, w, C_WHITE);
    draw_thick_line(CX + a, 104 + a, CX, 104 + h / 2, w, C_WHITE);
    draw_thick_line(CX - a, 104 + a, CX + a, 104 - a, w, C_WHITE);
    draw_thick_line(CX + a, 104 - a, CX, 104 - h / 2, w, C_WHITE);

    draw_text_centered(156, "Pairing code", &font_title, C_TEXT);

    draw_card(14, 196, TFT_WIDTH - 28, 76, 14);
    draw_text_centered(202, passkey_text, &font_huge, C_WHITE);

    draw_text_centered(286, "Enter this code on your phone",
                       &font_small, C_MUTED);
}

static char enroll_name[48];
static char enroll_instruction[32];
static char enroll_hint[48];
static int enroll_step;

static void scene_enroll(void)
{
    static const char *const labels[] = { "Scan 1", "Scan 2", "Save" };

    uint16_t accent = enroll_step >= 3 ? C_AMBER : C_INDIGO;
    char name[48];

    draw_header();

    /* Who is being enrolled */
    draw_text_centered(52, "ENROLLING", &font_small, C_INDIGO);
    fit_text(name, sizeof(name),
             enroll_name[0] != '\0' ? enroll_name : "New fingerprint",
             &font_body, TFT_WIDTH - 32);
    draw_text_centered(68, name, &font_body, C_TEXT);

    draw_scanner(146, 42, accent);

    draw_text_centered(202, enroll_instruction, &font_title, C_TEXT);
    draw_text_centered(234, enroll_hint, &font_small, C_MUTED);

    /* Stepper: scan 1, scan 2, save */
    for (int i = 0; i < 3; i++) {
        int x = 40 + i * 80;
        bool done = (i + 1) < enroll_step;
        bool current = (i + 1) == enroll_step;
        uint16_t c = done ? C_GREEN : current ? accent : C_FAINT;

        if (i < 2) {
            fill_rect(x + 12, 271, 56, 2, done ? C_GREEN : C_FAINT);
        }

        fill_circle(x, 272, current ? 9.0f : 7.0f, c);

        if (done) {
            draw_thick_line(x - 3.5f, 272, x - 1, 275, 2, C_WHITE);
            draw_thick_line(x - 1, 275, x + 4, 269, 2, C_WHITE);
        }

        draw_text(x - text_width(labels[i], &font_small) / 2, 286,
                  labels[i], &font_small, current ? C_TEXT : C_MUTED);
    }
}

static bool enroll_ok;
static char enroll_message[96];

static void scene_enroll_result(void)
{
    draw_header();
    draw_badge(116, enroll_ok);

    draw_text_centered(184, enroll_ok ? "Enrolled" : "Enrollment failed",
                       &font_title, enroll_ok ? C_GREEN : C_RED);

    if (enroll_ok) {
        draw_card(14, 226, TFT_WIDTH - 28, 74, 14);

        char name[48];

        draw_avatar(52, 263, enroll_name, C_GREEN);
        fit_text(name, sizeof(name), enroll_name, &font_body,
                 TFT_WIDTH - 28 - 74 - 12);
        draw_text(88, 240, name, &font_body, C_TEXT);
        draw_text(88, 264, enroll_message, &font_small, C_MUTED);
    } else {
        draw_message_card(222, 84, enroll_message, NULL);
    }
}

/* ----------------------------------------------------------
 * Public TFT API
 * ---------------------------------------------------------- */

bool tft_init(void)
{
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "      EMPLOYEES ACCESS TFT");
    ESP_LOGI(TAG, "================================");

    draw_lock = xSemaphoreCreateMutex();

    if (draw_lock == NULL) {
        return false;
    }

    tft_spi_init();
    tft_reset();
    ili9341_init();

    return true;
}

void tft_set_wifi_status(bool connected)
{
    wifi_connected = connected;
}

bool tft_clock_changed(void)
{
    return minute_of_day() != shown_minute;
}

void tft_show_startup(void)
{
    lock_display();
    render(scene_startup);
    unlock_display();
}

static void show_idle(void)
{
    lock_display();
    shown_minute = minute_of_day();
    render(scene_idle);
    unlock_display();
}

void tft_show_ready(void)
{
    show_idle();
}

void tft_show_place_finger(void)
{
    show_idle();
}

void tft_show_processing(void)
{
    lock_display();
    render(scene_processing);
    unlock_display();
}

void tft_show_granted(const char *name, uint16_t id, uint16_t score)
{
    struct tm tm;

    lock_display();

    if (name != NULL && name[0] != '\0') {
        snprintf(granted_name, sizeof(granted_name), "%s", name);
    } else {
        snprintf(granted_name, sizeof(granted_name), "Employee %u", id);
    }

    if (local_time(&tm)) {
        snprintf(granted_details, sizeof(granted_details),
                 "Checked in at %02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(granted_details, sizeof(granted_details),
                 "Slot %u  -  Match %u", id, score);
    }

    render(scene_granted);

    unlock_display();
}

void tft_show_punch(const char *name, bool in, bool duplicate,
                    const char *detail, const char *worked)
{
    lock_display();

    punch_in = in;
    punch_duplicate = duplicate;
    snprintf(punch_name, sizeof(punch_name), "%s",
             name != NULL && name[0] != '\0' ? name : "Employee");
    snprintf(punch_detail, sizeof(punch_detail), "%s", detail != NULL ? detail : "");
    snprintf(punch_worked, sizeof(punch_worked), "%s",
             worked != NULL && !duplicate ? worked : "");

    render(scene_punch);

    unlock_display();
}

void tft_show_denied(const char *reason)
{
    lock_display();

    snprintf(denied_reason, sizeof(denied_reason), "%s",
             reason != NULL ? reason : "Not recognised");

    render(scene_denied);

    unlock_display();
}

void tft_show_passkey(uint32_t passkey)
{
    lock_display();

    snprintf(passkey_text, sizeof(passkey_text), "%06u",
             (unsigned)(passkey % 1000000));

    render(scene_passkey);

    unlock_display();
}

void tft_show_enroll_step(const char *name, int step,
                          const char *instruction, const char *hint)
{
    lock_display();

    snprintf(enroll_name, sizeof(enroll_name), "%s",
             name != NULL ? name : "");
    snprintf(enroll_instruction, sizeof(enroll_instruction), "%s",
             instruction != NULL ? instruction : "");
    snprintf(enroll_hint, sizeof(enroll_hint), "%s",
             hint != NULL ? hint : "");

    enroll_step = step;

    render(scene_enroll);

    unlock_display();
}

void tft_show_enroll_result(bool ok, const char *name, const char *message)
{
    lock_display();

    enroll_ok = ok;

    snprintf(enroll_name, sizeof(enroll_name), "%s",
             name != NULL ? name : "");
    snprintf(enroll_message, sizeof(enroll_message), "%s",
             message != NULL ? message : "");

    render(scene_enroll_result);

    unlock_display();
}
