# BLE Multi-Peripheral Aggregator Project (Bi-Directional)
## Overview
This project demonstrates a robust, bi-directional BLE ecosystem using the NimBLE stack. It features a Central hub (ESP32-S3) that aggregates sensor data from a Peripheral (ESP32-C6/H2) while simultaneously sending control commands back to the node.

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

## Phase 3: Bi-Directional Control
* **Peripheral:** Added a secondary characteristic (0x5678) with a write-access callback.
* **Central:** Implemented a controller_task that discovers the new characteristic handle and executes ble_gattc_write_flat commands.

## Phase 4: Thread Safety & Stability
* **Stack Optimization:** Identified and fixed a stack overflow by increasing the controller_task stack from $2KB$ to $4KB$ to accommodate NimBLE stack overhead.
* **Asynchronous Handling:** Implemented a FreeRTOS Queue on the Central. This decouples the BLE interrupt-style callbacks from the application logic, ensuring the radio task never stalls during data processing.

## Final Architecture: Producer-Consumer Pattern
To ensure system stability, the Central uses a thread-safe "Producer-Consumer" design.

* **The Producer:** The BLE Host task receives notifications and "produces" data into a FreeRTOS Queue.

* **The Consumer:** A dedicated Application Task "consumes" data from the queue, allowing for heavy data processing without blocking the BLE radio.

## How to Run
* **Peripheral:** Flash the ESP32-C6/H2. Monitor the serial output to see incoming commands.

* **Central:** Flash the ESP32-S3. Monitor the serial output to see the live data stream and confirmation of sent commands.