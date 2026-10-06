#include "esp_http_client.h"
#include "tft.h"
#include "ble_enroll.h"
#include "json_util.h"
#include "esp_err.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/uart.h"

#include "esp_log.h"
#include "esp_err.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include "nvs_flash.h"
#include "esp_http_client.h"



/* =========================================================
 * R307S CONFIGURATION
 * ========================================================= */

#define FP_UART         UART_NUM_2
#define FP_TX           17
#define FP_RX           16
#define FP_BAUD_RATE    57600

#define FP_CAPACITY     1000    /* R307S template slots */

/* =========================================================
 * WIFI CONFIGURATION
 * ========================================================= */

/* WIFI_SSID and WIFI_PASSWORD live in secrets.h (not in git);
 * copy secrets.h.example to secrets.h and fill them in */
#include "secrets.h"

static const char *WIFI_TAG = "WIFI";

/* =========================================================
 * SERVER CONFIGURATION
 * ========================================================= */

#define SERVER_BASE_URL     "http://192.168.88.84:8100"
#define HEARTBEAT_URL       SERVER_BASE_URL "/api/v1/devices/heartbeat"
#define ACCESS_EVENT_URL    SERVER_BASE_URL "/api/v1/access-events"
#define ENROLL_START_URL    SERVER_BASE_URL "/api/v1/enrollment/start"
#define ENROLL_COMPLETE_URL SERVER_BASE_URL "/api/v1/enrollment/complete"

/* POSIX TZ string for the clock on the idle screen (India: UTC+5:30) */
#define TIMEZONE            "IST-5:30"
#define NTP_SERVER          "pool.ntp.org"

#define DEVICE_ID           "ESP32-ENTRY-001"
#define FIRMWARE_VERSION    "0.1.0"

static const char *SERVER_TAG = "SERVER";

/* =========================================================
 * WIFI EVENT GROUP
 * ========================================================= */

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t wifi_event_group;

/* =========================================================
 * WIFI EVENT HANDLER
 * ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(WIFI_TAG, "Wi-Fi started");

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(WIFI_TAG, "esp_wifi_connect failed: %s",
                     esp_err_to_name(err));
        }

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        xEventGroupClearBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        tft_set_wifi_status(false);

        wifi_event_sta_disconnected_t *disconn =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(
            WIFI_TAG,
            "Wi-Fi disconnected (reason %d) - reconnecting...",
            disconn->reason
        );

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(WIFI_TAG, "Reconnect failed: %s",
                     esp_err_to_name(err));
        }

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            WIFI_TAG,
            "Got IP: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        ESP_LOGI(
            WIFI_TAG,
            "Gateway: " IPSTR,
            IP2STR(&event->ip_info.gw)
        );

        ESP_LOGI(
            WIFI_TAG,
            "Netmask: " IPSTR,
            IP2STR(&event->ip_info.netmask)
        );

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        tft_set_wifi_status(true);
    }
}

/* =========================================================
 * WIFI INITIALIZATION
 * ========================================================= */

static void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_LOGW(WIFI_TAG, "NVS needs to be erased");

        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());

    } else {
        ESP_ERROR_CHECK(ret);
    }

    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(WIFI_TAG, "Failed to create Wi-Fi event group");
        abort();
    }

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        WIFI_SSID,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        WIFI_PASSWORD,
        sizeof(wifi_config.sta.password) - 1
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(
        WIFI_TAG,
        "Connecting to Wi-Fi: %s",
        WIFI_SSID
    );
}

/* =========================================================
 * WAIT FOR WIFI
 * ========================================================= */

static bool wait_for_wifi(void)
{
    ESP_LOGI(
        WIFI_TAG,
        "Waiting for Wi-Fi connection..."
    );

    EventBits_t bits =
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)
        );

    if (bits & WIFI_CONNECTED_BIT) {

        ESP_LOGI(
            WIFI_TAG,
            "Wi-Fi connection successful"
        );

        return true;
    }

    ESP_LOGE(
        WIFI_TAG,
        "Wi-Fi connection timeout"
    );

    return false;
}

/* =========================================================
 * SERVER HEARTBEAT
 * ========================================================= */

static bool send_heartbeat(void)
{
    const char *json =
        "{"
        "\"device_id\":\"" DEVICE_ID "\","
        "\"firmware_version\":\"" FIRMWARE_VERSION "\","
        "\"status\":\"online\""
        "}";

    ESP_LOGI(
        SERVER_TAG,
        "Sending heartbeat to %s",
        HEARTBEAT_URL
    );

    esp_http_client_config_t config = {
        .url = HEARTBEAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(
            SERVER_TAG,
            "Failed to create HTTP client"
        );
        return false;
    }

    esp_err_t err =
        esp_http_client_set_header(
            client,
            "Content-Type",
            "application/json"
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            SERVER_TAG,
            "Failed to set HTTP header: %s",
            esp_err_to_name(err)
        );
        esp_http_client_cleanup(client);
        return false;
    }

    err =
        esp_http_client_set_post_field(
            client,
            json,
            strlen(json)
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            SERVER_TAG,
            "Failed to set POST body: %s",
            esp_err_to_name(err)
        );
        esp_http_client_cleanup(client);
        return false;
    }

    err =
        esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(
            SERVER_TAG,
            "Heartbeat request failed: %s",
            esp_err_to_name(err)
        );
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code =
        esp_http_client_get_status_code(client);

    int content_length =
        esp_http_client_get_content_length(client);

    ESP_LOGI(
        SERVER_TAG,
        "HTTP status = %d",
        status_code
    );

    ESP_LOGI(
        SERVER_TAG,
        "Response length = %d",
        content_length
    );

    esp_http_client_cleanup(client);

    if (status_code >= 200 &&
        status_code < 300) {

        ESP_LOGI(
            SERVER_TAG,
            "Heartbeat SUCCESS"
        );

        return true;
    }

    ESP_LOGE(
        SERVER_TAG,
        "Heartbeat FAILED: HTTP %d",
        status_code
    );

    return false;
}

/* =========================================================
 * R307S UART COMMAND
 * ========================================================= */

static int send_command_raw(
    const uint8_t *cmd,
    int cmd_len,
    uint8_t *rx,
    int rx_size)
{
    uart_flush_input(FP_UART);

    printf("\nSending command:\n");

    for (int i = 0; i < cmd_len; i++) {
        printf("%02X ", cmd[i]);
    }

    printf("\n");

    int written =
        uart_write_bytes(
            FP_UART,
            (const char *)cmd,
            cmd_len
        );

    if (written != cmd_len) {
        printf(
            "UART write failed: wrote %d of %d bytes\n",
            written,
            cmd_len
        );
        return -1;
    }

    /*
     * Read the 9-byte header first, then exactly the number of
     * bytes announced in its length field. Asking for rx_size bytes
     * up front would always block for the full timeout, because
     * acknowledgement packets are only 12-16 bytes long.
     */
    int len =
        uart_read_bytes(
            FP_UART,
            rx,
            9,
            pdMS_TO_TICKS(2000)
        );

    if (len == 9 &&
        rx[0] == 0xEF &&
        rx[1] == 0x01) {

        int payload_len =
            ((int)rx[7] << 8) | rx[8];

        if (payload_len > rx_size - 9) {
            payload_len = rx_size - 9;
        }

        int more =
            uart_read_bytes(
                FP_UART,
                rx + 9,
                payload_len,
                pdMS_TO_TICKS(500)
            );

        if (more > 0) {
            len += more;
        }
    }

    printf("Received %d bytes:\n", len);

    for (int i = 0; i < len; i++) {
        printf("%02X ", rx[i]);
    }

    printf("\n");

    return len;
}

static uint8_t send_command(
    const uint8_t *cmd,
    int cmd_len)
{
    uint8_t rx[64] = {0};

    int len =
        send_command_raw(
            cmd,
            cmd_len,
            rx,
            sizeof(rx)
        );

    if (len < 12) {
        printf(
            "No valid response. Received %d bytes\n",
            len < 0 ? 0 : len
        );
        return 0xFF;
    }

    if (rx[0] != 0xEF ||
        rx[1] != 0x01 ||
        rx[6] != 0x07) {
        printf("Invalid acknowledgement packet\n");
        return 0xFF;
    }

    return rx[9];
}

/* =========================================================
 * VERIFY PASSWORD
 * ========================================================= */

static bool verify_password(void)
{
    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x07,
        0x13,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x1B
    };

    printf("\n==============================\n");
    printf(" Verifying R307S password\n");
    printf("==============================\n");

    uint8_t result =
        send_command(cmd, sizeof(cmd));

    if (result == 0x00) {
        printf("Password verification SUCCESS\n");
        return true;
    }

    printf("Password verification FAILED\n");
    printf("Error code = 0x%02X\n", result);

    return false;
}

/* =========================================================
 * GENIMG
 * ========================================================= */

static uint8_t gen_img(void)
{
    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x03,
        0x01,
        0x00, 0x05
    };

    return send_command(
        cmd,
        sizeof(cmd)
    );
}

/* =========================================================
 * IMG2TZ
 * ========================================================= */

static uint8_t img2tz(uint8_t buffer)
{
    uint8_t checksum =
        0x01 +
        0x04 +
        0x02 +
        buffer;

    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x04,
        0x02,
        buffer,
        0x00,
        checksum
    };

    return send_command(
        cmd,
        sizeof(cmd)
    );
}

/* =========================================================
 * REGMODEL
 * ========================================================= */

static uint8_t reg_model(void)
{
    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x03,
        0x05,
        0x00,
        0x09
    };

    return send_command(
        cmd,
        sizeof(cmd)
    );
}

/* =========================================================
 * STORE MODEL
 * ========================================================= */

static uint8_t store_model(
    uint8_t buffer,
    uint16_t id)
{
    uint16_t checksum =
        0x01 +
        0x06 +
        0x06 +
        buffer +
        ((id >> 8) & 0xFF) +
        (id & 0xFF);

    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x06,
        0x06,
        buffer,
        (uint8_t)(id >> 8),
        (uint8_t)(id & 0xFF),
        (uint8_t)(checksum >> 8),
        (uint8_t)(checksum & 0xFF)
    };

    printf("StoreModel target ID: %u\n", id);

    return send_command(
        cmd,
        sizeof(cmd)
    );
}

/* =========================================================
 * TEMPLATE COUNT
 * ========================================================= */

static bool get_template_count(uint16_t *count)
{
    if (count == NULL) {
        return false;
    }

    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x03,
        0x1D,
        0x00,
        0x21
    };

    uint8_t response[64] = {0};

    printf("\n============================================\n");
    printf("        CHECKING R307S TEMPLATE COUNT\n");
    printf("============================================\n");

    int len =
        send_command_raw(
            cmd,
            sizeof(cmd),
            response,
            sizeof(response)
        );

    if (len < 12) {
        printf(
            "TemplateNum failed: received only %d bytes\n",
            len < 0 ? 0 : len
        );
        return false;
    }

    if (response[6] != 0x07 ||
        response[7] != 0x00 ||
        response[8] != 0x05) {

        printf(
            "Unexpected TemplateNum response packet.\n"
        );
        return false;
    }

    uint8_t confirmation_code =
        response[9];

    printf(
        "Confirmation code: 0x%02X\n",
        confirmation_code
    );

    if (confirmation_code != 0x00) {
        printf(
            "TemplateNum FAILED: 0x%02X\n",
            confirmation_code
        );
        return false;
    }

    *count =
        ((uint16_t)response[10] << 8) |
        response[11];

    printf(
        "Stored template count: %u\n",
        *count
    );

    return true;
}

/* =========================================================
 * DELETE MODEL
 * ========================================================= */

static uint8_t delete_model(uint16_t id)
{
    uint16_t checksum =
        0x01 + 0x00 + 0x07 +
        0x0C +
        ((id >> 8) & 0xFF) +
        (id & 0xFF) +
        0x00 + 0x01;

    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x07,
        0x0C,
        (uint8_t)(id >> 8),
        (uint8_t)(id & 0xFF),
        0x00, 0x01,             /* delete one template */
        (uint8_t)(checksum >> 8),
        (uint8_t)(checksum & 0xFF)
    };

    printf("DeleteChar target ID: %u\n", id);

    return send_command(
        cmd,
        sizeof(cmd)
    );
}

/* =========================================================
 * WAIT FOR FINGER
 * ========================================================= */

typedef enum {
    WAIT_OK,
    WAIT_TIMEOUT,
    WAIT_ABORTED,           /* the phone queued an enrollment */
    WAIT_SENSOR_ERROR,
} wait_result_t;

/*
 * timeout_ms = 0 waits forever. With abort_on_request the wait
 * ends as soon as the phone queues an enrollment. `redraw` is
 * called when the BLE layer has drawn over the screen.
 */
static wait_result_t wait_for_finger(
    int timeout_ms,
    bool abort_on_request,
    void (*redraw)(void))
{
    printf("\n");
    printf("====================================\n");
    printf(" PLACE YOUR FINGER ON THE SENSOR\n");
    printf("====================================\n");

    TickType_t start = xTaskGetTickCount();

    while (1) {

        if (abort_on_request &&
            ble_enroll_request_pending()) {
            return WAIT_ABORTED;
        }

        /* Also redraw when the idle clock's minute rolls over */
        if (redraw != NULL &&
            (ble_enroll_take_redraw() || tft_clock_changed())) {
            redraw();
        }

        uint8_t result =
            gen_img();

        if (result == 0x00) {
            printf("\nFinger detected!\n");
            return WAIT_OK;
        }

        if (result != 0x02) {
            printf(
                "GenImg error: 0x%02X\n",
                result
            );
            return WAIT_SENSOR_ERROR;
        }

        if (timeout_ms > 0 &&
            xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            return WAIT_TIMEOUT;
        }

        vTaskDelay(
            pdMS_TO_TICKS(300)
        );
    }
}

/* =========================================================
 * WAIT FOR FINGER REMOVED
 * ========================================================= */

static wait_result_t wait_for_finger_removed(int timeout_ms)
{
    printf("\n");
    printf("====================================\n");
    printf(" REMOVE YOUR FINGER\n");
    printf("====================================\n");

    TickType_t start = xTaskGetTickCount();

    while (1) {

        uint8_t result =
            gen_img();

        if (result == 0x02) {
            printf("Finger removed.\n");
            return WAIT_OK;
        }

        if (result != 0x00) {
            printf(
                "GenImg error while waiting for removal: 0x%02X\n",
                result
            );
            return WAIT_SENSOR_ERROR;
        }

        if (timeout_ms > 0 &&
            xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            return WAIT_TIMEOUT;
        }

        vTaskDelay(
            pdMS_TO_TICKS(300)
        );
    }
}

/* =========================================================
 * SEARCH FINGERPRINT
 * ========================================================= */

static bool search_fingerprint(
    uint16_t *matched_id,
    uint16_t *matching_score)
{
    if (matched_id == NULL ||
        matching_score == NULL) {
        return false;
    }

    uint8_t cmd[] = {
        0xEF, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x01,
        0x00, 0x08,
        0x04,
        0x01,
        0x00, 0x00,     /* start page 0         */
        0x03, 0xE8,     /* 1000 pages (capacity) */
        0x00, 0xF9
    };

    uint8_t response[64] = {0};

    printf("\n============================================\n");
    printf("          SEARCHING FINGERPRINT\n");
    printf("============================================\n");

    int len =
        send_command_raw(
            cmd,
            sizeof(cmd),
            response,
            sizeof(response)
        );

    /*
     * Successful Search response:
     * Header(6) + ID(1) + Length(2) + content(7) = 16 bytes.
     * Content:
     *   Confirmation  ID_H ID_L  Score_H Score_L  Checksum_H Checksum_L
     */

    if (len < 16) {
        printf(
            "Search response too short: %d bytes\n",
            len < 0 ? 0 : len
        );
        return false;
    }

    if (response[6] != 0x07 ||
        response[7] != 0x00 ||
        response[8] != 0x07) {

        printf(
            "Unexpected Search response packet.\n"
        );
        return false;
    }

    uint8_t confirmation_code =
        response[9];

    printf(
        "Search confirmation: 0x%02X\n",
        confirmation_code
    );

    if (confirmation_code != 0x00) {
        printf(
            "Fingerprint NOT matched. "
            "Sensor error = 0x%02X\n",
            confirmation_code
        );
        return false;
    }

    *matched_id =
        ((uint16_t)response[10] << 8) |
        response[11];

    *matching_score =
        ((uint16_t)response[12] << 8) |
        response[13];

    printf(
        "Matched fingerprint ID : %u\n",
        *matched_id
    );

    printf(
        "Matching score         : %u\n",
        *matching_score
    );

    return true;
}

/* =========================================================
 * CAPTURE A TEMPLATE
 *
 * wait_for_finger() returns on the first touch, when often only
 * the edge of the finger is on the glass. Let the finger settle,
 * take a fresh image and retry a few times when the sensor
 * reports an unclear image (0x06 messy, 0x07 too few features).
 * ========================================================= */

static bool capture_template(uint8_t buffer)
{
    for (int attempt = 1; attempt <= 3; attempt++) {

        vTaskDelay(pdMS_TO_TICKS(attempt == 1 ? 300 : 250));

        if (gen_img() != 0x00) {
            printf("Finger lifted before a clear image was taken\n");
            return false;
        }

        uint8_t result = img2tz(buffer);

        if (result == 0x00) {
            return true;
        }

        printf("Img2Tz attempt %d: 0x%02X\n", attempt, result);

        if (result != 0x06 && result != 0x07) {
            return false;
        }
    }

    return false;
}

/* =========================================================
 * HTTP JSON POST
 *
 * Returns the HTTP status code, or -1 if the server could not
 * be reached. The response body is always NUL-terminated.
 * ========================================================= */

static int http_post_json(
    const char *url,
    const char *body,
    char *response,
    size_t response_len)
{
    response[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(SERVER_TAG, "Failed to initialize HTTP client");
        return -1;
    }

    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json"
    );

    int body_len = strlen(body);

    ESP_LOGI(SERVER_TAG, "POST %s", url);
    ESP_LOGI(SERVER_TAG, "%s", body);

    /*
     * open/write/read instead of perform(), so the response
     * body can be read back.
     */
    esp_err_t err = esp_http_client_open(client, body_len);

    if (err != ESP_OK) {
        ESP_LOGE(
            SERVER_TAG,
            "HTTP request failed: %s",
            esp_err_to_name(err)
        );

        esp_http_client_cleanup(client);
        return -1;
    }

    if (esp_http_client_write(client, body, body_len) != body_len) {
        ESP_LOGE(SERVER_TAG, "Failed to send request body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    esp_http_client_fetch_headers(client);

    int status_code =
        esp_http_client_get_status_code(client);

    int read_len =
        esp_http_client_read_response(
            client,
            response,
            response_len - 1
        );

    response[read_len > 0 ? read_len : 0] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(SERVER_TAG, "HTTP %d: %s", status_code, response);

    return status_code;
}

static bool http_ok(int status_code)
{
    return status_code >= 200 &&
           status_code < 300;
}

static bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(wifi_event_group) &
            WIFI_CONNECTED_BIT) != 0;
}

/* =========================================================
 * SERVER ACCESS EVENT ("login validation")
 *
 * The server maps the sensor slot to an employee and decides
 * whether access is allowed.
 * ========================================================= */

typedef enum {
    ACCESS_ALLOWED,
    ACCESS_DENIED,
    ACCESS_UNVERIFIED,      /* server unreachable or returned an error */
} access_result_t;

/* Attendance result of an allowed scan, filled by send_access_event() */
typedef struct {
    char punch[8];          /* "in", "out" or "" (server sent none) */
    bool duplicate;         /* second scan within a minute: nothing new recorded */
    char detail[32];        /* e.g. "Out at 18:01" */
    char worked[32];        /* e.g. "8h 16m today" (punch out only) */
} punch_info_t;

static access_result_t send_access_event(
    uint16_t sensor_slot,
    uint16_t match_score,
    char *employee_name,
    size_t employee_name_len,
    char *reason,
    size_t reason_len,
    punch_info_t *punch)
{
    char body[160];
    char response[768];
    char status[16];

    employee_name[0] = '\0';
    reason[0] = '\0';
    memset(punch, 0, sizeof(*punch));

    if (!wifi_is_connected()) {
        ESP_LOGW(SERVER_TAG, "Wi-Fi down - access event not sent");
        return ACCESS_UNVERIFIED;
    }

    snprintf(
        body,
        sizeof(body),
        "{"
        "\"device_id\":\"%s\","
        "\"sensor_slot\":%u,"
        "\"match_score\":%u"
        "}",
        DEVICE_ID,
        sensor_slot,
        match_score
    );

    int status_code =
        http_post_json(ACCESS_EVENT_URL, body,
                       response, sizeof(response));

    if (!http_ok(status_code)) {
        return ACCESS_UNVERIFIED;
    }

    json_get_string(response, "full_name",
                    employee_name, employee_name_len);

    json_get_string(response, "reason", reason, reason_len);

    json_get_string(response, "punch", punch->punch, sizeof(punch->punch));
    json_get_string(response, "detail", punch->detail, sizeof(punch->detail));
    json_get_string(response, "worked", punch->worked, sizeof(punch->worked));
    punch->duplicate = strstr(response, "\"duplicate\": true") != NULL ||
                       strstr(response, "\"duplicate\":true") != NULL;

    if (json_get_string(response, "status", status, sizeof(status)) &&
        strcmp(status, "allowed") == 0) {
        return ACCESS_ALLOWED;
    }

    return ACCESS_DENIED;
}

/* Turns a server reason code into text for the denied screen */
static const char *describe_denial(const char *reason)
{
    if (strcmp(reason, "unknown_sensor_slot") == 0) {
        return "Finger not registered";
    }

    if (strcmp(reason, "employee_inactive") == 0) {
        return "Employee inactive";
    }

    if (strcmp(reason, "biometric_record_inactive") == 0) {
        return "Fingerprint disabled";
    }

    return "Access not allowed";
}

/* =========================================================
 * ENROLLMENT (requested from the phone over Bluetooth)
 * ========================================================= */

static void enroll_failed(const char *message)
{
    char escaped[96];
    char json[160];

    printf("Enrollment FAILED: %s\n", message);

    json_escape(message, escaped, sizeof(escaped));
    snprintf(json, sizeof(json),
             "{\"state\":\"error\",\"message\":\"%s\"}", escaped);

    ble_enroll_notify(json);
    tft_show_enroll_result(false, NULL, message);

    vTaskDelay(pdMS_TO_TICKS(3000));
}

static const char *wait_error_text(wait_result_t result)
{
    return result == WAIT_TIMEOUT
           ? "Timed out waiting for the finger"
           : "Fingerprint sensor error";
}

/*
 * Two scans of the same finger merged into a model in buffer 1.
 * Returns NULL on success, otherwise a message for the user.
 */
static const char *capture_enrollment_model(const char *name)
{
    wait_result_t w;

    /* Scan 1 */
    ble_enroll_notify("{\"state\":\"place_finger\",\"step\":1}");
    tft_show_enroll_step(name, 1, "Place finger", "Scan 1 of 2");

    w = wait_for_finger(30000, false, NULL);

    if (w != WAIT_OK) {
        return wait_error_text(w);
    }

    if (!capture_template(1)) {
        return "Scan was unclear, please try again";
    }

    /* Lift */
    ble_enroll_notify("{\"state\":\"remove_finger\"}");
    tft_show_enroll_step(name, 2, "Lift your finger", "Then place it again");

    w = wait_for_finger_removed(15000);

    if (w != WAIT_OK) {
        return w == WAIT_TIMEOUT
               ? "Finger was not removed"
               : "Fingerprint sensor error";
    }

    /* Scan 2 */
    ble_enroll_notify("{\"state\":\"place_finger\",\"step\":2}");
    tft_show_enroll_step(name, 2, "Place again", "Scan 2 of 2");

    w = wait_for_finger(30000, false, NULL);

    if (w != WAIT_OK) {
        return wait_error_text(w);
    }

    if (!capture_template(2)) {
        return "Scan was unclear, please try again";
    }

    if (reg_model() != 0x00) {
        return "The two scans did not match, please try again";
    }

    return NULL;
}

static void handle_enroll_request(const enroll_request_t *request)
{
    char body[160];
    char response[768];
    char name[48] = "";
    char detail[96];
    long slot = 0;

    printf("\n");
    printf("============================================\n");
    printf("   ENROLLMENT: employee %ld, finger %ld\n",
           request->employee_id, request->finger_index);
    printf("============================================\n");

    ble_enroll_notify("{\"state\":\"contacting_server\"}");
    tft_show_enroll_step("", 0, "Please wait", "Contacting the server");

    if (!wifi_is_connected()) {
        enroll_failed("Device is offline (no Wi-Fi)");
        return;
    }

    /* 1. Server validates the employee and assigns a sensor slot */
    snprintf(body, sizeof(body),
             "{\"employee_id\":%ld,\"device_id\":\"%s\",\"finger_index\":%ld}",
             request->employee_id, DEVICE_ID, request->finger_index);

    int status_code =
        http_post_json(ENROLL_START_URL, body,
                       response, sizeof(response));

    if (status_code < 0) {
        enroll_failed("Server unreachable");
        return;
    }

    if (!http_ok(status_code)) {
        if (!json_get_string(response, "detail", detail, sizeof(detail))) {
            snprintf(detail, sizeof(detail),
                     "Server error %d", status_code);
        }
        enroll_failed(detail);
        return;
    }

    if (!json_get_int(response, "sensor_slot", &slot) ||
        slot < 1 || slot >= FP_CAPACITY) {
        enroll_failed("Server sent an invalid sensor slot");
        return;
    }

    json_get_string(response, "full_name", name, sizeof(name));

    /* 2. Capture the finger */
    const char *error = capture_enrollment_model(name);

    if (error != NULL) {
        enroll_failed(error);
        return;
    }

    /* 3. Store it in the sensor */
    ble_enroll_notify("{\"state\":\"saving\"}");
    tft_show_enroll_step(name, 3, "Saving", "Please wait");

    if (store_model(1, (uint16_t)slot) != 0x00) {
        enroll_failed("Sensor could not store the fingerprint");
        return;
    }

    /* 4. Tell the server the slot now belongs to this employee */
    snprintf(body, sizeof(body),
             "{\"employee_id\":%ld,\"device_id\":\"%s\","
             "\"finger_index\":%ld,\"sensor_slot\":%ld}",
             request->employee_id, DEVICE_ID,
             request->finger_index, slot);

    status_code =
        http_post_json(ENROLL_COMPLETE_URL, body,
                       response, sizeof(response));

    if (!http_ok(status_code)) {

        /* Keep sensor and server consistent */
        delete_model((uint16_t)slot);

        if (status_code < 0) {
            snprintf(detail, sizeof(detail), "Server unreachable");
        } else if (!json_get_string(response, "detail",
                                    detail, sizeof(detail))) {
            snprintf(detail, sizeof(detail),
                     "Server error %d", status_code);
        }

        enroll_failed(detail);
        return;
    }

    /* Done */
    char escaped[64];
    char json[160];
    char slot_text[16];

    if (name[0] == '\0') {
        snprintf(name, sizeof(name), "Employee %ld",
                 request->employee_id);
    }

    json_escape(name, escaped, sizeof(escaped));
    snprintf(json, sizeof(json),
             "{\"state\":\"done\",\"slot\":%ld,\"name\":\"%s\"}",
             slot, escaped);

    ble_enroll_notify(json);

    snprintf(slot_text, sizeof(slot_text), "Saved to slot %ld", slot);
    tft_show_enroll_result(true, name, slot_text);

    printf("Enrollment SUCCESS: %s -> slot %ld\n", name, slot);

    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* =========================================================
 * VERIFY A FINGER
 * ========================================================= */

static void finish_attempt(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    wait_for_finger_removed(10000);
}

static void run_verification(void)
{
    tft_show_place_finger();

    wait_result_t w =
        wait_for_finger(0, true, tft_show_place_finger);

    if (w == WAIT_ABORTED) {
        return;                 /* enrollment takes over */
    }

    if (w != WAIT_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }

    tft_show_processing();

    if (!capture_template(1)) {
        tft_show_denied("Unclear scan");
        finish_attempt();
        return;
    }

    uint16_t slot = 0;
    uint16_t score = 0;

    if (!search_fingerprint(&slot, &score)) {
        tft_show_denied("Fingerprint not recognised");
        finish_attempt();
        return;
    }

    char name[48];
    char reason[48];
    punch_info_t punch;

    access_result_t access =
        send_access_event(slot, score,
                          name, sizeof(name),
                          reason, sizeof(reason),
                          &punch);

    switch (access) {

    case ACCESS_ALLOWED:
        printf("ACCESS GRANTED: %s (slot %u) punch=%s %s\n",
               name, slot, punch.punch, punch.detail);

        if (punch.punch[0] != '\0') {
            tft_show_punch(name, strcmp(punch.punch, "in") == 0,
                           punch.duplicate, punch.detail, punch.worked);
        } else {
            tft_show_granted(name, slot, score);
        }
        break;

    case ACCESS_DENIED:
        printf("ACCESS DENIED by server: %s\n", reason);
        tft_show_denied(describe_denial(reason));
        break;

    case ACCESS_UNVERIFIED:
        /*
         * Offline fallback: the sensor only stores enrolled
         * fingers, so a local match is accepted when the server
         * cannot be asked.
         */
        printf("Server unavailable - accepting local match (slot %u)\n",
               slot);
        tft_show_granted(NULL, slot, score);
        break;
    }

    finish_attempt();
}

/* =========================================================
 * MAIN APPLICATION
 * ========================================================= */

void app_main(void)
{
    printf("\n");
    printf("============================================\n");
    printf("         EMPLOYEES ACCESS DEVICE\n");
    printf("============================================\n");
    printf("Device ID : %s\n", DEVICE_ID);
    printf("Firmware  : %s\n", FIRMWARE_VERSION);
    printf("Server    : %s\n", SERVER_BASE_URL);
    printf("Bluetooth : %s\n", BLE_DEVICE_NAME);
    printf("============================================\n");

    if (!tft_init()) {
        ESP_LOGE("TFT", "TFT initialization failed");
    } else {
        tft_show_startup();
    }

    /* Wi-Fi (also initialises NVS, which Bluetooth needs) */
    wifi_init();

    /* Network time for the clock; syncs in the background */
    setenv("TZ", TIMEZONE, 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&sntp_config);

    if (!ble_enroll_init()) {
        ESP_LOGE("BLE", "Bluetooth initialization failed");
    }

    if (wait_for_wifi()) {
        send_heartbeat();
    } else {
        ESP_LOGW(
            SERVER_TAG,
            "Skipping heartbeat because Wi-Fi is unavailable"
        );
    }

    /* R307S UART */
    uart_config_t uart_config = {
        .baud_rate = FP_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(FP_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FP_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FP_UART, FP_TX, FP_RX,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    printf("\n");
    printf("============================================\n");
    printf("              R307S READY\n");
    printf("============================================\n");
    printf("UART      : UART2\n");
    printf("TX        : GPIO17\n");
    printf("RX        : GPIO16\n");
    printf("Baud      : 57600\n");
    printf("============================================\n");

    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!verify_password()) {
        printf("\nSensor authentication failed.\n");
        tft_show_denied("Fingerprint sensor error");
        return;
    }

    uint16_t template_count = 0;

    if (get_template_count(&template_count) &&
        template_count == 0) {
        printf("No fingerprints stored - enroll from the phone.\n");
    }

    while (1) {

        enroll_request_t request;

        if (ble_enroll_take_request(&request, 0)) {
            handle_enroll_request(&request);
        } else {
            run_verification();
        }
    }
}
