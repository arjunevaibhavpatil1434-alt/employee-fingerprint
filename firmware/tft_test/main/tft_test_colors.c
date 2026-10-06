#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

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
 * Fill entire screen with RGB565 color
 * ---------------------------------------------------------- */

static void tft_fill_screen(uint16_t color)
{
    static uint8_t buffer[320 * 2];

    for (int i = 0; i < 320; i++) {
        buffer[i * 2]     = color >> 8;
        buffer[i * 2 + 1] = color & 0xFF;
    }

    tft_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);

    gpio_set_level(TFT_DC, 1);

    /* 240 pixels per row */
    for (int y = 0; y < TFT_HEIGHT; y++) {
        tft_spi_write(buffer, sizeof(buffer));
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
        .max_transfer_sz = 320 * 2,
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
 * Main
 * ---------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "      ESP32 TFT TEST");
    ESP_LOGI(TAG, "      240 x 320 SPI");
    ESP_LOGI(TAG, "================================");

    tft_spi_init();

    tft_reset();

    ili9341_init();

    while (1) {

        ESP_LOGI(TAG, "Displaying RED");
        tft_fill_screen(0xF800);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "Displaying GREEN");
        tft_fill_screen(0x07E0);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "Displaying BLUE");
        tft_fill_screen(0x001F);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "Displaying WHITE");
        tft_fill_screen(0xFFFF);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "Displaying BLACK");
        tft_fill_screen(0x0000);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
