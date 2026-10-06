#include "ble_enroll.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_random.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "json_util.h"
#include "tft.h"

static const char *TAG = "BLE";

/* Provided by NimBLE's store/config, not declared in a public header */
void ble_store_config_init(void);

/* ---------------------------------------------------------
 * UUIDs (little-endian byte order)
 *
 *   service 6e1a0001-4b5c-4c1e-9f3a-2d7c5e8b9a01
 *   command 6e1a0002-4b5c-4c1e-9f3a-2d7c5e8b9a01  write
 *   status  6e1a0003-4b5c-4c1e-9f3a-2d7c5e8b9a01  read/notify
 * --------------------------------------------------------- */

#define CBIO_UUID(id) \
    BLE_UUID128_INIT(0x01, 0x9a, 0x8b, 0x5e, 0x7c, 0x2d, 0x3a, 0x9f, \
                     0x1e, 0x4c, 0x5c, 0x4b, (id), 0x00, 0x1a, 0x6e)

static const ble_uuid128_t service_uuid = CBIO_UUID(0x01);
static const ble_uuid128_t command_uuid = CBIO_UUID(0x02);
static const ble_uuid128_t status_uuid  = CBIO_UUID(0x03);

#define COMMAND_MAX_LEN  128
#define STATUS_MAX_LEN   200

/* ---------------------------------------------------------
 * State shared between the NimBLE host task and the main task
 * --------------------------------------------------------- */

static QueueHandle_t request_queue;

static volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool status_subscribed;
static volatile bool redraw_needed;

/* Only touched from the NimBLE host task (GAP events) */
static int connection_count;

static uint16_t status_val_handle;
static uint8_t own_addr_type;

static char last_status[STATUS_MAX_LEN] = "{\"state\":\"idle\"}";
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

static void advertise(void);

/* ---------------------------------------------------------
 * Status updates
 * --------------------------------------------------------- */

void ble_enroll_notify(const char *json)
{
    taskENTER_CRITICAL(&status_lock);
    strlcpy(last_status, json, sizeof(last_status));
    taskEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "Status: %s", json);

    uint16_t handle = conn_handle;

    if (handle == BLE_HS_CONN_HANDLE_NONE || !status_subscribed) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));

    if (om == NULL) {
        ESP_LOGW(TAG, "No buffer for status notification");
        return;
    }

    int rc = ble_gatts_notify_custom(handle, status_val_handle, om);

    if (rc != 0) {
        ESP_LOGW(TAG, "Status notification failed: %d", rc);
    }
}

static void notify_error(const char *message)
{
    char json[STATUS_MAX_LEN];

    snprintf(json, sizeof(json),
             "{\"state\":\"error\",\"message\":\"%s\"}", message);

    ble_enroll_notify(json);
}

/* ---------------------------------------------------------
 * GATT access
 * --------------------------------------------------------- */

static int handle_command_write(struct os_mbuf *om)
{
    char command[COMMAND_MAX_LEN + 1];
    uint16_t len = 0;

    if (OS_MBUF_PKTLEN(om) > COMMAND_MAX_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (ble_hs_mbuf_to_flat(om, command, COMMAND_MAX_LEN, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    command[len] = '\0';

    ESP_LOGI(TAG, "Command: %s", command);

    enroll_request_t request = {0};

    if (!json_get_int(command, "employee_id", &request.employee_id) ||
        request.employee_id < 1) {
        notify_error("employee_id must be a positive number");
        return 0;
    }

    if (!json_get_int(command, "finger_index", &request.finger_index) ||
        request.finger_index < 1 || request.finger_index > 10) {
        notify_error("finger_index must be between 1 and 10");
        return 0;
    }

    /* Queue length is 1: a full queue means a request is waiting */
    if (xQueueSend(request_queue, &request, 0) != pdTRUE) {
        notify_error("Device busy, try again");
        return 0;
    }

    ble_enroll_notify("{\"state\":\"queued\"}");

    return 0;
}

static int gatt_access(uint16_t conn, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        return handle_command_write(ctxt->om);

    case BLE_GATT_ACCESS_OP_READ_CHR: {
        char copy[STATUS_MAX_LEN];

        taskENTER_CRITICAL(&status_lock);
        strlcpy(copy, last_status, sizeof(copy));
        taskEXIT_CRITICAL(&status_lock);

        return os_mbuf_append(ctxt->om, copy, strlen(copy)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &command_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &status_uuid.u,
                .access_cb = gatt_access,
                .val_handle = &status_val_handle,
                .flags = BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ---------------------------------------------------------
 * GAP events
 * --------------------------------------------------------- */

/*
 * Wi-Fi shares the radio with Bluetooth, so some connection
 * events get skipped while Wi-Fi is busy. A 30-50 ms interval
 * leaves Wi-Fi room, and a 6 s supervision timeout stops a run of
 * missed events from dropping the link (phones often pick 5 s or
 * less with a 7.5 ms interval).
 *
 * Asked for once the phone has exchanged the MTU (its own setup is
 * done) and again after encryption. Not on connect: a request
 * during the phone's MTU exchange and service discovery stalls
 * Android, which then gives up after its 20 s connect timeout.
 */
static void request_stable_params(uint16_t handle)
{
    struct ble_gap_upd_params params = {
        .itvl_min = 24,                 /* x 1.25 ms = 30 ms */
        .itvl_max = 40,                 /* x 1.25 ms = 50 ms */
        .latency = 0,
        .supervision_timeout = 600,     /* x 10 ms = 6 s */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_update_params(handle, &params);

    if (rc != 0) {
        ESP_LOGW(TAG, "Connection parameter request failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection_count++;

            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                const uint8_t *a = desc.peer_ota_addr.val;

                ESP_LOGI(TAG, "Phone connected (handle %d, %02x:%02x:%02x:%02x:%02x:%02x, "
                         "interval %d x1.25 ms, timeout %d x10 ms)",
                         event->connect.conn_handle,
                         a[5], a[4], a[3], a[2], a[1], a[0],
                         desc.conn_itvl, desc.supervision_timeout);
            }
        }
        /*
         * Keep advertising while connected: a bonded phone can
         * reconnect on its own and would otherwise hide the
         * terminal from the app's device picker.
         */
        advertise();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        /* 0x208 = supervision timeout (link lost), 0x213 = phone closed it */
        ESP_LOGI(TAG, "Phone disconnected (handle %d, reason 0x%03x)",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        if (connection_count > 0) {
            connection_count--;
        }
        if (event->disconnect.conn.conn_handle == conn_handle) {
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            status_subscribed = false;
        }
        redraw_needed = true;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        /* Status updates go to the phone that subscribed last */
        if (event->subscribe.attr_handle == status_val_handle) {
            ESP_LOGI(TAG, "Status updates %s (handle %d)",
                     event->subscribe.cur_notify ? "on" : "off",
                     event->subscribe.conn_handle);

            if (event->subscribe.cur_notify) {
                conn_handle = event->subscribe.conn_handle;
                status_subscribed = true;
            } else if (event->subscribe.conn_handle == conn_handle) {
                status_subscribed = false;
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption %s (status %d)",
                 event->enc_change.status == 0 ? "enabled" : "failed",
                 event->enc_change.status);

        if (event->enc_change.status == 0) {
            request_stable_params(event->enc_change.conn_handle);
        }

        redraw_needed = true;
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {

            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = esp_random() % 1000000,
            };

            ESP_LOGI(TAG, "Pairing passkey: %06" PRIu32, io.passkey);

            tft_show_passkey(io.passkey);

            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The phone forgot the bond: drop ours and pair again */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Connection: interval %d x1.25 ms, timeout %d x10 ms",
                     desc.conn_itvl, desc.supervision_timeout);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        request_stable_params(event->mtu.conn_handle);
        return 0;

    default:
        return 0;
    }
}

/* ---------------------------------------------------------
 * Advertising and host task
 * --------------------------------------------------------- */

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields response = {0};

    /* Flags + 128-bit service UUID fill most of the 31-byte packet */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    /* ...so the name goes in the scan response */
    response.name = (const uint8_t *)BLE_DEVICE_NAME;
    response.name_len = strlen(BLE_DEVICE_NAME);
    response.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);

    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&response);
    }

    if (rc != 0) {
        ESP_LOGE(TAG, "Setting advertising data failed: %d", rc);
        return;
    }

    /*
     * While a phone is connected, advertise slowly (100-150 ms) so
     * the advertising leaves radio time for the connection and Wi-Fi.
     * Restart so a change of interval takes effect.
     */
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = connection_count > 0 ? 160 : 0,     /* x 0.625 ms; 0 = default fast */
        .itvl_max = connection_count > 0 ? 240 : 0,
    };

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);

    /*
     * EALREADY: still advertising from before.
     * ENOMEM: all connection slots in use; resumes on a disconnect.
     */
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOMEM) {
        ESP_LOGE(TAG, "Advertising failed: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);

    ESP_LOGI(TAG, "Advertising as %s", BLE_DEVICE_NAME);

    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------
 * Public API
 * --------------------------------------------------------- */

bool ble_enroll_init(void)
{
    request_queue = xQueueCreate(1, sizeof(enroll_request_t));

    if (request_queue == NULL) {
        return false;
    }

    /* Requires NVS to be initialised (bonds are stored there) */
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Passkey shown on the TFT, entered on the phone */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    if (ble_gatts_count_cfg(gatt_services) != 0 ||
        ble_gatts_add_svcs(gatt_services) != 0) {
        ESP_LOGE(TAG, "Registering GATT services failed");
        return false;
    }

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    /* Room for status JSON in a single notification */
    ble_att_set_preferred_mtu(185);

    ble_store_config_init();

    nimble_port_freertos_init(host_task);

    return true;
}

bool ble_enroll_request_pending(void)
{
    return request_queue != NULL &&
           uxQueueMessagesWaiting(request_queue) > 0;
}

bool ble_enroll_take_request(enroll_request_t *request, TickType_t wait)
{
    return request_queue != NULL &&
           xQueueReceive(request_queue, request, wait) == pdTRUE;
}

bool ble_enroll_take_redraw(void)
{
    if (!redraw_needed) {
        return false;
    }

    redraw_needed = false;

    return true;
}
