#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#define VITALS_SERVICE_UUID 0x00FF
#define VITALS_CHAR_UUID 0xFF01
#define VITALS_NUM_HANDLES 4

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&adv_params));
        }
    }
}

static esp_gatt_srvc_id_t vitals_service_id = {
    .is_primary = true,
    .id.inst_id = 0,
    .id.uuid.len = ESP_UUID_LEN_16,
    .id.uuid.uuid.uuid16 = VITALS_SERVICE_UUID,
};

static esp_bt_uuid_t vitals_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid.uuid16 = VITALS_CHAR_UUID,
};

static uint8_t vitals_value[] = {98, 72};

static esp_attr_value_t vitals_attr_value = {
    .attr_max_len = sizeof(vitals_value),
    .attr_len = sizeof(vitals_value),
    .attr_value = vitals_value,
};

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT &&
        param->reg.status == ESP_GATT_OK) {
        ESP_ERROR_CHECK(esp_ble_gatts_create_service(
            gatts_if, &vitals_service_id, VITALS_NUM_HANDLES));
    }

    if (event == ESP_GATTS_CREATE_EVT &&
        param->create.status == ESP_GATT_OK) {
        ESP_ERROR_CHECK(esp_ble_gatts_start_service(
            param->create.service_handle));
        ESP_ERROR_CHECK(esp_ble_gatts_add_char(
            param->create.service_handle,
            &vitals_char_uuid,
            ESP_GATT_PERM_READ,
            ESP_GATT_CHAR_PROP_BIT_READ,
            &vitals_attr_value,
            &(esp_attr_control_t){ .auto_rsp = ESP_GATT_AUTO_RSP }));
    }

    if (event == ESP_GATTS_DISCONNECT_EVT) {
        ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&adv_params));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));
    ESP_ERROR_CHECK(esp_ble_gap_set_device_name("ESP32_Vitals"));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&adv_data));
    while (1) {
        printf("Simulated SpO2: 98%%, heart rate: 72 bpm\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}