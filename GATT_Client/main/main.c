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
static const ble_uuid16_t target_cmd_uuid = BLE_UUID16_INIT(0x5678);
static uint16_t active_conn_handle = 0; 
static uint16_t cmd_chr_handle = 0;

static uint8_t own_addr_type;

static QueueHandle_t ble_data_queue;

// Forward declarations
static void ble_app_scan(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);

// --- Subscription Callback ---
static int on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "Successfully subscribed to NODE_A notifications!");
    } else {
        ESP_LOGE(TAG, "Failed to subscribe. Error: %d", error->status);
    }
    return 0;
}

// --- Characteristic Discovery Callback ---
static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && chr != NULL) {
        
        // Did we find the Read/Notify characteristic (0x1234)?
        if (ble_uuid_cmp(&chr->uuid.u, &target_chr_uuid.u) == 0) {
            ESP_LOGI(TAG, "Found Sensor Characteristic! Subscribing...");
            uint8_t sub_val[2] = {0x01, 0x00};
            ble_gattc_write_flat(conn_handle, chr->val_handle + 1, sub_val, sizeof(sub_val), on_subscribe, NULL);
        }
        
        // NEW: Did we find the Write characteristic (0x5678)?
        else if (ble_uuid_cmp(&chr->uuid.u, &target_cmd_uuid.u) == 0) {
            ESP_LOGI(TAG, "Found Command Characteristic! Saving handle: %d", chr->val_handle);
            // Save this handle so our FreeRTOS task can write to it later
            cmd_chr_handle = chr->val_handle; 
        }
    }
    return 0;
}

// --- Service Discovery Callback ---
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

// --- The Main GAP Event Handler ---
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
                
                // NEW: Save the connection handle
                active_conn_handle = event->connect.conn_handle; 
                
                ble_gattc_disc_all_svcs(event->connect.conn_handle, on_svc_disc, NULL);
            } else {
                ESP_LOGE(TAG, "Connection failed. Resuming scan...");
                ble_app_scan();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Disconnected from NODE_A. Resuming scan...");
            
            // NEW: Clear our saved handles so we don't try to write to a dead connection
            active_conn_handle = 0;
            cmd_chr_handle = 0; 
            
            ble_app_scan();
            break;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint8_t val = event->notify_rx.om->om_data[0];
            
            // SHIP IT: Push the data to the queue and get back to BLE business
            // We use a 0-tick wait because we must never block in this callback
            xQueueSend(ble_data_queue, &val, 0);
            break;
        }
    }
    return 0;
}

// --- Scanner Configuration ---
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

// Task to send periodic commands to the Peripheral
void controller_task(void *pvParameter) {
    uint8_t command_val = 0;

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // Wait 3 seconds
        
        // Only attempt to write if we have an active connection AND we found the characteristic
        if (active_conn_handle != 0 && cmd_chr_handle != 0) {
            
            command_val = !command_val; // Toggle between 0 and 1
            
            // Write to the Peripheral
            int rc = ble_gattc_write_flat(active_conn_handle, cmd_chr_handle, 
                                          &command_val, sizeof(command_val), NULL, NULL);
                                          
            if (rc == 0) {
                ESP_LOGI(TAG, "Successfully sent command: %d", command_val);
            } else {
                ESP_LOGE(TAG, "Failed to send command. Error: %d", rc);
            }
        }
    }
}

void application_processing_task(void *pvParameter) {
    uint8_t received_data;
    while(1) {
        // Blocks here until data arrives in the queue
        if (xQueueReceive(ble_data_queue, &received_data, portMAX_DELAY)) {
            // Now you can do slow things: save to SD, print to screen, etc.
            ESP_LOGW("APP_LOGIC", "Data processed safely in App Task: %d", received_data);
        }
    }
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

    // Create a queue for 10 data points
    ble_data_queue = xQueueCreate(10, sizeof(uint8_t));

    // Launch tasks (Note: boosted stack for controller_task as we discussed!)
    xTaskCreate(application_processing_task, "app_task", 4096, NULL, 5, NULL);
    xTaskCreate(controller_task, "ctrl_task", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(ble_host_task);
}