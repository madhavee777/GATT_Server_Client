# BLE Multi-Peripheral Aggregator Project
## Overview
This project establishes a custom Bluetooth Low Energy (BLE) Client/Server architecture using the ESP-IDF framework and the NimBLE host stack. It demonstrates a foundational IoT topology where a Central hub actively scans for, connects to, and receives live, synchronized sensor data from a Peripheral node via GATT notifications.

## Hardware Requirements
Central Node (GATT Client): ESP32-S3

## Peripheral Node (GATT Server): 
ESP32-C6 or ESP32-H2

## Software & Configuration
Framework: ESP-IDF (v5.0+)

## BLE Stack: 
NimBLE (Configured via idf.py menuconfig -> Component config -> Bluetooth -> NimBLE - BLE only)

## Architecture & Phases Completed
### Phase 1: The Peripheral (GATT Server)
The ESP32-C6/H2 acts as the GAP Broadcaster and GATT Server.

* **Advertising:** Actively broadcasts the device name (NODE_A) and primary Service UUID (0xABCD).

* **GATT Table:** Hosts a custom Service and a custom Characteristic (0x1234) with READ and NOTIFY properties.

* **Address Inference:** Utilizes ble_hs_id_infer_auto to dynamically resolve and store the hardware MAC address type to prevent hardware access faults during radio initialization.

## Phase 2: The Central (GATT Client)
The ESP32-S3 acts as the GAP Observer/Initiator and GATT Client.

* **Active Scanning:** Scans the environment, parsing advertisement payloads for the target device name (NODE_A).

* **Automated Connection:** Halts the scanner and initiates a connection upon discovering the target.

* **Service Discovery Cascade:** Upon connection, automatically executes a discovery cascade:

1. Discovers the custom Service (0xABCD).
2. Discovers the custom Characteristic (0x1234) within that service to obtain its memory handle.

* **Subscription:** Subscribes to the Peripheral's live data stream by writing 0x0100 to the Client Characteristic Configuration Descriptor (CCCD).

## Phase 3: FreeRTOS Concurrency
The Peripheral utilizes FreeRTOS to decouple BLE communication from application logic.

* **Sensor Task:** A dedicated FreeRTOS task wakes up every 1000ms, simulates changing sensor data, and pushes the data to the NimBLE stack via ble_gatts_chr_updated(), provided the Central is actively subscribed.