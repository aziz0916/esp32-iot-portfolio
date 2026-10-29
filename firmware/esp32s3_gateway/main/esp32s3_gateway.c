#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include <string.h>
#include "sdkconfig.h"
#include "mqtt_client.h"

typedef struct {
    bool connected;
    esp_gatt_if_t gattc_if;
    uint16_t conn_id;
    uint16_t char_handle;
} vitals_read_context_t;

static QueueHandle_t vitals_queue = NULL;
static const char *TAG = "GATEWAY";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static const char *target_name = "ESP32_Vitals";
static esp_gatt_if_t client_if = ESP_GATT_IF_NONE;
static bool connecting = false;
static bool scan_requested = false;
static esp_bd_addr_t target_address;
static esp_ble_addr_type_t target_address_type;
static uint16_t service_start_handle = 0;
static uint16_t service_end_handle = 0;
static esp_bt_uuid_t vitals_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = 0xFF01,
};

static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

static void vitals_read_task(void *arg)
{
    vitals_read_context_t context = {0};

    while (1) {
        // 等待連線狀態更新；已連線時最多等待 5 秒。
        TickType_t wait = context.connected
            ? pdMS_TO_TICKS(5000)
            : portMAX_DELAY;

        if (xQueueReceive(vitals_queue, &context, wait) == pdTRUE) {
            continue;
        }

        if (context.connected) {
            esp_err_t err = esp_ble_gattc_read_char(
                context.gattc_if,
                context.conn_id,
                context.char_handle,
                ESP_GATT_AUTH_REQ_NONE);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Read request failed: %s",
                         esp_err_to_name(err));
            }
        }
    }
}

static void request_scan(void)
{
    if (scan_requested) {
        return;
    }

    scan_requested = true;
    esp_err_t err = esp_ble_gap_start_scanning(0);
    if (err != ESP_OK) {
        scan_requested = false;
        ESP_LOGE(TAG, "Failed to request scan: %s", esp_err_to_name(err));
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT) {
        if (param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            scan_requested = false;

            if (connecting) {
                esp_err_t err = esp_ble_gattc_open(
                    client_if, target_address, target_address_type, true);

                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Connection request failed: %s",
                             esp_err_to_name(err));
                    connecting = false;
                    request_scan();
                }
            }
        } else {
            ESP_LOGE(TAG, "Scan stop failed: %d",
                     param->scan_stop_cmpl.status);
            connecting = false;
        }
    }
    if (event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            scan_requested = false;
            ESP_LOGE(TAG, "Scan start failed: %d",
                    param->scan_start_cmpl.status);
        }
    }

    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            request_scan();
        }
    }

    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT &&
        param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        uint8_t name_len = 0;
        uint8_t *name = esp_ble_resolve_adv_data(
            param->scan_rst.ble_adv,
            ESP_BLE_AD_TYPE_NAME_CMPL,
            &name_len);

        if (name != NULL &&
            name_len == strlen(target_name) &&
            memcmp(name, target_name, name_len) == 0) {
            if (!connecting && client_if != ESP_GATT_IF_NONE) {
                connecting = true;
                ESP_LOGI(TAG, "Found target: %.*s", name_len, (char *)name);
                memcpy(target_address, param->scan_rst.bda, sizeof(target_address));
                target_address_type = param->scan_rst.ble_addr_type;
                ESP_ERROR_CHECK(esp_ble_gap_stop_scanning());
            }
        }
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event,
                                esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            client_if = gattc_if;
            ESP_LOGI(TAG, "GATT client registered");
        } else {
            ESP_LOGE(TAG, "GATT client registration failed: %d",
                     param->reg.status);
        }
    }

    if (event == ESP_GATTC_OPEN_EVT) {
        if (param->open.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Connected to sender");
            ESP_ERROR_CHECK(esp_ble_gattc_search_service(
    gattc_if, param->open.conn_id, NULL));
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", param->open.status);
            connecting = false;
            request_scan();
        }
    }

    if (event == ESP_GATTC_SEARCH_RES_EVT) {
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16) {
            uint16_t uuid = param->search_res.srvc_id.uuid.uuid.uuid16;
            ESP_LOGI(TAG, "Found service: 0x%04X", uuid);

            if (uuid == 0x00FF) {
                service_start_handle = param->search_res.start_handle;
                service_end_handle = param->search_res.end_handle;
                ESP_LOGI(TAG, "Vitals service handles: %u–%u",
                         service_start_handle, service_end_handle);
            }
        }
    }

    if (event == ESP_GATTC_SEARCH_CMPL_EVT) {
        if (param->search_cmpl.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Service search complete");
            if (service_start_handle != 0) {
                esp_gattc_char_elem_t char_result;
                uint16_t count = 1;

                esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(
                    gattc_if,
                    param->search_cmpl.conn_id,
                    service_start_handle,
                    service_end_handle,
                    vitals_char_uuid,
                    &char_result,
                    &count);

                if (status == ESP_GATT_OK && count > 0) {
                    ESP_LOGI(TAG, "Found vitals characteristic, handle: %u",
                            char_result.char_handle);
                    vitals_read_context_t context = {
                        .connected = true,
                        .gattc_if = gattc_if,
                        .conn_id = param->search_cmpl.conn_id,
                        .char_handle = char_result.char_handle,
                    };

                    xQueueOverwrite(vitals_queue, &context);
                } else {
                    ESP_LOGE(TAG, "Vitals characteristic not found: status=%d, count=%u",
                            status, count);
                }
            }
        } else {
            ESP_LOGE(TAG, "Service search failed: %d",
                     param->search_cmpl.status);
        }
    }

    if (event == ESP_GATTC_READ_CHAR_EVT) {
        if (param->read.status == ESP_GATT_OK &&
            param->read.value_len >= 2) {
            ESP_LOGI(TAG, "SpO2: %u%%, heart rate: %u bpm",
                    param->read.value[0], param->read.value[1]);
            if (mqtt_connected && mqtt_client != NULL) {
                char payload[64];

                snprintf(payload, sizeof(payload),
                        "{\"spo2\":%u,\"heart_rate\":%u}",
                        param->read.value[0], param->read.value[1]);

                int message_id = esp_mqtt_client_publish(
                    mqtt_client, "esp32/vitals", payload, 0, 0, 0);

                if (message_id < 0) {
                    ESP_LOGE(TAG, "MQTT publish failed");
                } else {
                    ESP_LOGI(TAG, "MQTT publish requested: %s", payload);
                }
            }
        } else {
            ESP_LOGE(TAG, "Read failed or unexpected length: status=%d, length=%u",
                    param->read.status, param->read.value_len);
        }
    }

    if (event == ESP_GATTC_DISCONNECT_EVT) {
        ESP_LOGW(TAG, "Sender disconnected");

        vitals_read_context_t context = {
            .connected = false,
        };
        xQueueOverwrite(vitals_queue, &context);

        connecting = false;
        service_start_handle = 0;
        service_end_handle = 0;
        request_scan();
    }
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_GATEWAY_MQTT_BROKER_URI,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_handle_t client = mqtt_client;
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi started, connecting");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected and received an IP address");

        static bool mqtt_started = false;
        if (!mqtt_started) {
            mqtt_start();
            mqtt_started = true;
        }
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_GATEWAY_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_GATEWAY_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    vitals_queue = xQueueCreate(1, sizeof(vitals_read_context_t));
    if (vitals_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create vitals queue");
        return;
    }

    if (xTaskCreate(vitals_read_task, "vitals_read", 4096,
                NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create vitals read task");
        vQueueDelete(vitals_queue);
        vitals_queue = NULL;
        return;
    }
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
}
