#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "Peripheral_Node";

// --- 1. UUID Definitions ---
// Using 16-bit UUIDs for simplicity. 
// Custom Service: 0xABCD
// Custom Characteristic: 0x1234
static const ble_uuid16_t gatt_svr_svc_uuid = BLE_UUID16_INIT(0xABCD);
static const ble_uuid16_t gatt_svr_chr_uuid = BLE_UUID16_INIT(0x1234);

// Store the address type inferred by NimBLE
static uint8_t own_addr_type;

static uint8_t sensor_data = 0;      // Our live, changing data
static bool is_subscribed = false;   // Flag to check if Central is listening

// Handle for our characteristic so we can send notifications later
uint16_t custom_chr_val_handle; 
static void ble_app_advertise(void);

// --- 2. GATT Characteristic Access Callback ---
// This function fires when the Central tries to READ or WRITE our characteristic.
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Read from our global live data
        int rc = os_mbuf_append(ctxt->om, &sensor_data, sizeof(sensor_data));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// --- 3. GATT Table Definition ---
// This defines the structure of your data.
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &gatt_svr_chr_uuid.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &custom_chr_val_handle,
            },
            { 0 } // No more characteristics
        },
    },
    { 0 } // No more services
};

// --- 4. GAP Event Handler ---
// This handles connections, disconnections, and subscriptions.
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Central connected!");
            } else {
                ESP_LOGE(TAG, "Connection failed, restarting advertising...");
                // Restart advertising if connection fails
                ble_app_advertise(); 
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Central disconnected. Restarting advertising...");
            // CRITICAL: Always restart advertising when a device disconnects!
            ble_app_advertise();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "Central changed subscription status!");
            is_subscribed = event->subscribe.cur_notify;
            break;
    }
    return 0;
}

// --- 5. Advertising Configuration ---
static void ble_app_advertise(void) {
    struct ble_hs_adv_fields fields;
    const char *device_name;
    memset(&fields, 0, sizeof(fields));

    // Discoverability flags
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    
    // Include device name ("NODE_A") in payload
    device_name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    // Include our Custom Service UUID so the Central knows who we are
    fields.uuids16 = (ble_uuid16_t[]){gatt_svr_svc_uuid};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    // Set advertising data
    ble_gap_adv_set_fields(&fields);

    // Start advertising
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // Undirected connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // General discoverable

    // Start advertising using the inferred address type
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                      &adv_params, ble_gap_event, NULL);
    ESP_LOGI(TAG, "Advertising started...");
}

// --- 6. Synchronization Callback ---
static void ble_app_on_sync(void) {
    // Pass the pointer so NimBLE can write the address type
    ble_hs_id_infer_auto(0, &own_addr_type); 
    ble_app_advertise();           
}

// --- 7. NimBLE Host Task ---
// FreeRTOS task that runs the NimBLE event loop
void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); // This function blocks until nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

void sensor_task(void *pvParameter) {
    while(1) {
        // Block for 1000 ticks (1 second)
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        
        if (is_subscribed) {
            sensor_data++; // Simulate a changing sensor reading
            
            // This powerful NimBLE function tells the stack the characteristic 
            // has changed, automatically triggering a notification to all subscribers.
            ble_gatts_chr_updated(custom_chr_val_handle); 
            
            ESP_LOGI(TAG, "Pushed notification: %d", sensor_data);
        }
    }
}

// --- 8. Main Application Entry Point ---
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize NimBLE port and stack
    nimble_port_init();

    // Initialize GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Load our custom GATT table
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    // Set device name
    ble_svc_gap_device_name_set("NODE_A");

    // Assign the sync callback
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    
    // Start our simulated sensor task
    xTaskCreate(sensor_task, "sensor_task", 2048, NULL, 5, NULL);
    
    // Start the NimBLE host task in FreeRTOS
    nimble_port_freertos_init(ble_host_task);
}