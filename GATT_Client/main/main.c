#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

static const char *TAG = "Central_Node";

// The UUIDs we are looking for (must match NODE_A exactly)
static const ble_uuid16_t target_svc_uuid = BLE_UUID16_INIT(0xABCD);
static const ble_uuid16_t target_chr_uuid = BLE_UUID16_INIT(0x1234);

static uint8_t own_addr_type;

// Forward declarations
static void ble_app_scan(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);

// --- 5. Subscription Callback ---
static int on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "Successfully subscribed to NODE_A notifications!");
    } else {
        ESP_LOGE(TAG, "Failed to subscribe. Error: %d", error->status);
    }
    return 0;
}

// --- 4. Characteristic Discovery Callback ---
static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && chr != NULL) {
        // Did we find our target characteristic (0x1234)?
        if (ble_uuid_cmp(&chr->uuid.u, &target_chr_uuid.u) == 0) {
            ESP_LOGI(TAG, "Found target Characteristic! Handle: %d", chr->val_handle);
            
            // To subscribe, we write 0x0100 to the Client Characteristic Configuration Descriptor (CCCD).
            // In standard BLE layouts, the CCCD is located exactly one handle after the characteristic value.
            uint8_t sub_val[2] = {0x01, 0x00};
            ble_gattc_write_flat(conn_handle, chr->val_handle + 1, sub_val, sizeof(sub_val), on_subscribe, NULL);
        }
    }
    return 0;
}

// --- 3. Service Discovery Callback ---
static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == 0 && svc != NULL) {
        // Did we find our target service (0xABCD)?
        if (ble_uuid_cmp(&svc->uuid.u, &target_svc_uuid.u) == 0) {
            ESP_LOGI(TAG, "Found target Service! Searching for characteristics...");
            // Now ask the Peripheral for the characteristics inside this specific service
            ble_gattc_disc_all_chrs(conn_handle, svc->start_handle, svc->end_handle, on_chr_disc, NULL);
        }
    }
    return 0;
}

// --- 2. The Main GAP Event Handler ---
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_hs_adv_fields fields;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            // 2a. A device is broadcasting. Parse its payload.
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

            // Check if the device name matches "NODE_A"
            if (fields.name != NULL && strncmp((char *)fields.name, "NODE_A", fields.name_len) == 0) {
                ESP_LOGI(TAG, "Found NODE_A! Stopping scan and connecting...");
                
                // Stop scanning so we don't overwhelm the radio
                ble_gap_disc_cancel(); 
                
                // Initiate connection to the MAC address we just found
                ble_gap_connect(own_addr_type, &event->disc.addr, 30000, NULL, ble_gap_event, NULL);
            }
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected to NODE_A!");
                // 2b. We are connected. Now we must discover what services it has.
                ble_gattc_disc_all_svcs(event->connect.conn_handle, on_svc_disc, NULL);
            } else {
                ESP_LOGE(TAG, "Connection failed. Resuming scan...");
                ble_app_scan();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Disconnected from NODE_A. Resuming scan...");
            // Always go back to hunting if the connection drops
            ble_app_scan();
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            // 2c. This fires every time NODE_A pushes data to us!
            ESP_LOGI(TAG, ">>> Received Notification! Data: %d", event->notify_rx.om->om_data[0]);
            break;
    }
    return 0;
}

// --- 1. Scanner Configuration ---
static void ble_app_scan(void) {
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    
    disc_params.filter_duplicates = 1; // Ignore redundant advertisements
    disc_params.passive = 0;           // Active scan (requests more data like device name)
    disc_params.itvl = 0;              // Default scan interval
    disc_params.window = 0;            // Default scan window

    ESP_LOGI(TAG, "Starting scan for NODE_A...");
    ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
}

// --- Initialization ---
static void ble_app_on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_app_scan(); // Start hunting
}

void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(ble_host_task);
}