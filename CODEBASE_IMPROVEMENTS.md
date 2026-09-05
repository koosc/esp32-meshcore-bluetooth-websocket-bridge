# ESP32 MeshCore Bluetooth Relay: Stability & Performance Analysis

This document provides a comprehensive technical audit of the current codebase (`src/main.cpp`, `platformio.ini`), focusing on stability, concurrency safety, memory management, network performance, and RF coexistence.

Items are labeled by impact and importance:
* **[HIGH IMPACT]**: Critical for preventing crashes, memory corruption, system hangs, or protocol failures under production load.
* **[LOW IMPACT]**: Architectural, efficiency, or edge-case refinements that improve maintainability, memory headroom, or edge performance.

---

## 1. Concurrency & Thread Safety

### 1.1 Thread-Unsafe WebSocket & TCP Broadcast from BLE Callback Task
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`onBLETxNotify`, lines ~181–193, and `broadcastFrameToClients`, lines ~520–541)
* **Description**: `onBLETxNotify()` is invoked directly from the NimBLE host task (`nimble_host` / event callback context). Inside this callback, it calls `broadcastFrameToClients()`, which directly executes:
  ```cpp
  tcpClient.write(packet, packetLen);
  wsServer.broadcastBIN(packet, packetLen);
  ```
  Neither `WebSocketsServer` nor `WiFiClient` are thread-safe. `wsServer.loop()` and `handleTCPClientData()` run concurrently in the Arduino main loop task (`loopTask`). Calling socket writes and mutating WebSocket client queues from the NimBLE task concurrently with main-loop socket polling creates race conditions within lwIP and `WebSocketsServer`, causing heap corruption (`vListInsert` or `CORRUPT HEAP` panics).
* **Failure Mode**: Sporadic crashes or reboots under heavy RF packet traffic when a WebSocket client is connected.
* **Recommendation**: 
  - Implement a thread-safe FreeRTOS queue (`QueueHandle_t` or `RingbufHandle_t`) for incoming BLE frames.
  - Push received data into the ring buffer inside `onBLETxNotify()`.
  - In `loop()`, drain the queue and call `broadcastFrameToClients()` exclusively from the main loop thread.

---

### 1.2 Unsynchronized Mutex-Less Access to `discoveredDevices`
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`discoveredDevices` vector, lines ~49, ~245–267, ~723–734, ~847)
* **Description**: `discoveredDevices` is a standard `std::vector<DiscoveredDevice>`. BLE advertisement callbacks (`BridgeAdvertisedDeviceCallbacks::onResult()`) execute on the NimBLE task and call `push_back()` and iterate over the vector. Simultaneously, HTTP requests on `loopTask` (`handleGetDevices()`, `handleClearDiscoveredCache()`, `startBLEScan()`) read, modify, and `.clear()` this vector.
* **Failure Mode**: `std::vector` dynamic reallocation during a `push_back()` while the main thread iterates over it results in memory read/write across deallocated heap (`LoadProhibited` / `StoreProhibited` crash).
* **Recommendation**:
  - Guard all reads and writes to `discoveredDevices` with a FreeRTOS mutex (`SemaphoreHandle_t`).
  - Keep lock duration minimal by copying device summaries into a local snapshot when generating JSON.

---

### 1.3 Non-Atomic Shared State Flags Across FreeRTOS Tasks
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (`bleAuthCompleted`, `bleConnected`, `bleConnecting`, lines ~73–74, ~152, ~175)
* **Description**: Flags like `bleAuthCompleted` are written in BLE callback tasks and polled in `connectToMeshCoreDevice()` (`while (!bleAuthCompleted ...)`). They are declared as plain `static bool` without `volatile` or `std::atomic<bool>`.
* **Failure Mode**: Aggressive compiler optimization might cache the variable in a register or reorder memory accesses, causing the polling loop to miss the update or timeout prematurely.
* **Recommendation**: Declare shared flags using `std::atomic<bool>` or `volatile bool`.

---

## 2. Memory & Heap Stability

### 2.1 Monolithic Dynamic String Allocation for Dashboard HTML
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`getDashboardHTML()`, lines ~996–1765)
* **Description**: The dashboard HTML/CSS/JS is ~45 KB. It is built in RAM by chaining hundreds of `html += "..."` statements into a dynamic `String` on the heap. Each append potentially reallocates and copies memory, fragmenting the ESP32-C3’s limited ~320 KB SRAM (where free heap is typically ~60–80 KB).
* **Failure Mode**: When multiple HTTP requests arrive (or during captive portal probing), heap allocation for the 45 KB string can fail, throwing allocation exceptions or crashing the web server.
* **Recommendation**:
  - Store the static HTML/JS/CSS in Flash memory using raw string literals in PROGMEM (`const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(...)rawliteral";`).
  - Serve directly with `httpServer.send_P(200, "text/html", DASHBOARD_HTML)` or gzip-compress it in flash (~8 KB) and serve with `Content-Encoding: gzip`. This eliminates 45 KB of dynamic heap churn per page view.

### 2.2 JSON Generation String Allocations
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (`getStatusJSON`, `getDevicesJSON`, `getBondsJSON`, `getWiFiScanJSON`)
* **Description**: JSON generators instantiate fresh `String` objects and concatenate fields with operator `+`. While individual JSON payloads are small (< 2 KB), periodic polling every 3–4 seconds causes continuous heap fragmentation.
* **Recommendation**: Pre-allocate string capacity using `.reserve(512)` or use a static buffer with `snprintf()`.

### 2.3 Dynamic Heap Allocation for BLEAddress
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (`pTargetAddress = new BLEAddress(...)`, lines ~68, ~277, ~896)
* **Description**: `BLEAddress` is an 8-byte value type, but is allocated with `new` and stored in a raw pointer `pTargetAddress`, requiring manual `delete` calls.
* **Recommendation**: Store `BLEAddress targetAddress;` as a direct value object or wrap in `std::unique_ptr<BLEAddress>` to prevent potential dangling pointer bugs.

---

## 3. Network & Socket Reliability

### 3.1 Single-Client TCP Server Limitation on Port 5000
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`tcpClient`, lines ~84, ~1888–1898)
* **Description**: The bridge only holds a single global `WiFiClient tcpClient`. When a client is connected, incoming connection requests from other clients are ignored or queued in the socket backlog until the current client disconnects. If a connection encounters an ungraceful drop (half-open TCP state without FIN/RST), port 5000 remains blocked until the socket times out.
* **Failure Mode**: Users unable to connect from the MeshCore Web App or CLI if another stale session hasn't cleanly terminated.
* **Recommendation**:
  - Configure TCP keep-alive on `tcpClient` (`setsockopt` with `SO_KEEPALIVE`, `TCP_KEEPIDLE = 5`, `TCP_KEEPINTVL = 3`, `TCP_KEEPCNT = 3`).
  - Alternatively, manage an array of active client sockets (e.g., up to 3–4 concurrent connections) so multiple tools or tabs can monitor the stream.

### 3.2 Port 5000 HTTP Sniffing Timeout Block
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`handleTCPClientData()`, lines ~557–570)
* **Description**: Port 5000 peeks at the first byte for HTTP verbs (`'G'`, `'P'`, `'H'`, `'O'`). If detected, it runs a blocking `while (millis() - startWait < 1500)` loop waiting for `\r\n\r\n`.
  1. Valid binary mesh packets or encrypted frames that happen to begin with byte values `0x47` ('G'), `0x50` ('P'), `0x48` ('H'), or `0x4F` ('O') will be misidentified as HTTP requests.
  2. If the client is slow or sends partial data, the entire main loop freezes for up to 1.5 seconds.
  3. Generating and printing the 45 KB HTML string over TCP port 5000 halts all bridging.
* **Failure Mode**: False-positive packet corruption on binary mesh frames starting with specific bytes, plus loop latency spikes up to 1.5 seconds.
* **Recommendation**:
  - Strictly enforce protocol separation: Port 80 for HTTP / Web Dashboard, Port 5000 exclusively for MeshCore framing (`<len><payload>`).
  - Remove HTTP sniffing on Port 5000 entirely, or make the check non-blocking and require an exact `"GET / "` prefix rather than a single character match.

### 3.3 Blocking Delay in `connectWiFi()`
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`connectWiFi()`, lines ~367–375)
* **Description**: During connection attempts, `connectWiFi()` executes a blocking loop for up to 12 seconds with `delay(250)`. While in station mode (`!isAPMode`), neither `wsServer.loop()` nor `handleTCPClientData()` nor BLE bridging routines are serviced.
* **Failure Mode**: Complete communication dropout and WebSocket disconnection for all existing clients while the device is attempting to connect to Wi-Fi.
* **Recommendation**: Convert Wi-Fi connection state handling into a non-blocking state machine in `loop()` or rely on ESP32 event handlers (`WiFi.onEvent()`).

---

## 4. Bluetooth & RF Coexistence

### 4.1 Aggressive 99% Duty Cycle BLE Scan Starves Wi-Fi
* **Impact**: **HIGH IMPACT**
* **Location**: `src/main.cpp` (`setup()`, lines ~1854–1855)
* **Description**: 
  ```cpp
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  ```
  The ESP32-C3 has a single 2.4 GHz radio shared between Wi-Fi and Bluetooth. A scan window of 99 ms on an interval of 100 ms reserves 99% of the radio time for BLE. This severely starves the Wi-Fi radio, causing dropped TCP packets, high ping times (>500 ms), and broken WebSocket connections.
* **Failure Mode**: Sluggish or timing-out web requests and WebSocket disconnections while BLE scanning is active.
* **Recommendation**: Reduce the scan duty cycle to ~25–35% (e.g., `setInterval(160); setWindow(40);`). When connected to a target device, stop background scanning completely.

### 4.2 Repetitive 5-Second Scan When Disconnected
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (`loop()`, lines ~1922–1928)
* **Description**: When disconnected from the target BLE device, the bridge triggers a 5-second scan every 10 seconds indefinitely (50% scan time).
* **Recommendation**: Implement exponential backoff for reconnection scans (e.g., 5s, 10s, 30s, up to 60s max) to preserve Wi-Fi throughput when the companion radio is powered off.

### 4.3 MTU Negotiation vs. Fixed Buffer Sizing
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (`sendFrameToMeshCore()`, line ~509, and `MAX_FRAME_SIZE = 172`)
* **Description**: The buffer limit is hardcoded to 172 bytes. While MeshCore frames are currently <= 172 bytes, if future protocol versions or packet types exceed this size, packets will be dropped. Additionally, `pRxCharacteristic->writeValue()` relies on the negotiated MTU (247 bytes); if MTU exchange fails, NimBLE will fragment the GATT write into multiple 20-byte ATT packets unless properly configured.
* **Recommendation**: Query `pClient->getMTU()` and verify that the effective ATT payload size accommodates the maximum frame length.

---

## 5. Architectural & Maintainability Refinements

### 5.1 Monolithic Single-File Design
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (~1,950 lines)
* **Description**: All application layers (BLE stack, security store, Wi-Fi captive portal, DNS server, TCP framing, WebSockets, HTTP REST API, embedded dashboard) reside in a single file.
* **Recommendation**: Refactor into distinct modules:
  - `ble_manager.h / .cpp`: BLE scanning, connection, security callbacks, and GATT notifications.
  - `wifi_portal.h / .cpp`: Wi-Fi STA, Soft AP, DNS captive portal, and network scanning.
  - `bridge_stream.h / .cpp`: Framing (`<` / `>`), TCP port 5000, and WebSocket server.
  - `dashboard_html.h`: Gzipped or PROGMEM web dashboard assets.

### 5.2 Missing Hardware / Task Watchdog (WDT)
* **Impact**: **LOW IMPACT**
* **Location**: `src/main.cpp` (`setup()`, `loop()`)
* **Description**: If a deadlock occurs in the BLE driver or during an unhandled Wi-Fi state transition, the ESP32 can hang indefinitely until manually power-cycled.
* **Recommendation**: Initialize the ESP-IDF Task Watchdog Timer (`esp_task_wdt_init()`, `esp_task_wdt_add(NULL)`) with a 10–15 second timeout, and reset it (`esp_task_wdt_reset()`) in `loop()`.

### 5.3 WebSocket Heartbeat / Ping-Pong
* **Impact**: **LOW IMPACT**
* **Location**: Web UI JavaScript & `wsServer`
* **Description**: NAT routers and firewalls may drop idle WebSocket TCP sessions after 60 seconds of inactivity if no mesh packets are received.
* **Recommendation**: Configure periodic WebSocket ping/pong (e.g., every 25 seconds) in `WebSocketsServer` (`wsServer.enableHeartbeat(25000, 3000, 2)`) to keep connection state alive through intermediate middleboxes.

---

## Summary Priority Matrix

| Category | Item | Impact | Effort |
| :--- | :--- | :---: | :---: |
| **Concurrency** | 1.1 Defer BLE notify broadcasts to main thread via FreeRTOS queue | **HIGH** | Medium |
| **Concurrency** | 1.2 Add mutex protection to `discoveredDevices` | **HIGH** | Low |
| **Memory** | 2.1 Move dashboard HTML from dynamic `String` to PROGMEM / Gzip | **HIGH** | Medium |
| **RF Coex** | 4.1 Reduce BLE scan duty cycle (from 99% to ~25%) | **HIGH** | Low |
| **Network** | 3.2 Remove blocking HTTP sniffing on TCP Port 5000 | **HIGH** | Low |
| **Network** | 3.3 Make Wi-Fi connection logic non-blocking | **HIGH** | Medium |
| **Network** | 3.1 Enable TCP socket keep-alive on Port 5000 | **HIGH** | Low |
| **RF Coex** | 4.2 Add exponential backoff for BLE reconnection scans | **LOW** | Low |
| **Memory** | 2.2 Pre-allocate JSON `String` buffers | **LOW** | Low |
| **Maintainability** | 5.1 Modularize `main.cpp` into component files | **LOW** | High |
| **Resilience** | 5.2 Add hardware Task Watchdog Timer (WDT) | **LOW** | Low |
| **Network** | 5.3 Enable WebSocket server heartbeats (ping/pong) | **LOW** | Low |
