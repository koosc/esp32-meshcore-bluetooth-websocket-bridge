#!/usr/bin/env bash
set -e

REPO="koosc/esp32-meshcore-bluetooth-websocket-bridge"

echo "Creating GitHub issues for $REPO..."

# Issue 1
gh issue create -R "$REPO" \
  --title "Bug: Cross-Task Thread Safety Violation in onBLETxNotify() Causes WebSockets & Socket Heap Corruption" \
  --label "bug,critical,concurrency" \
  --body "### Description
\`onBLETxNotify\` is invoked directly from the NimBLE host task (\`nimble_host\` / event callback context). Inside this callback, it calls \`broadcastFrameToClients()\`, which directly calls:
\`\`\`cpp
tcpClient.write(packet, packetLen);
wsServer.broadcastBIN(packet, packetLen);
\`\`\`
Neither \`WebSocketsServer\` nor \`WiFiClient\` are thread-safe. \`wsServer.loop()\` and \`handleTCPClientData()\` execute concurrently in the Arduino main loop task (\`loopTask\`). Calling socket writes and mutating WebSocket client queues from the NimBLE task concurrently with main-loop socket polling creates race conditions within lwIP and \`WebSocketsServer\`.

### Failure Mode
Sporadic crashes, heap corruption panics (\`vListInsert\` or \`CORRUPT HEAP\`), and broken WebSocket connections under moderate-to-heavy RF packet traffic when a client is connected.

### Location in Code
* [src/main.cpp:181-193](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L181-L193): \`onBLETxNotify()\` callback.
* [src/main.cpp:520-541](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L520-L541): \`broadcastFrameToClients()\` called directly across task boundaries.

### Proposed Fix
1. Implement a thread-safe FreeRTOS ring buffer (\`RingbufHandle_t\`) or FreeRTOS queue (\`QueueHandle_t\`) for incoming BLE frames.
2. In \`onBLETxNotify()\`, push incoming data into the ring buffer without touching sockets or WebSocket state.
3. In \`loop()\`, drain the queue and call \`broadcastFrameToClients()\` exclusively in the main thread context."

echo "Issue 1 created."

# Issue 2
gh issue create -R "$REPO" \
  --title "Bug: Race Condition and Memory Corruption on discoveredDevices Vector without Mutex" \
  --label "bug,critical,concurrency" \
  --body "### Description
\`discoveredDevices\` is a standard \`std::vector<DiscoveredDevice>\`. BLE advertisement callbacks (\`BridgeAdvertisedDeviceCallbacks::onResult()\`) execute on the NimBLE task and call \`push_back()\` and iterate over the vector. Concurrently, HTTP server requests on \`loopTask\` (\`handleGetDevices()\`, \`handleClearDiscoveredCache()\`, \`startBLEScan()\`) read, modify, and \`.clear()\` this vector without any synchronization mechanism.

### Failure Mode
When \`discoveredDevices.push_back()\` triggers a dynamic array reallocation while \`getDevicesJSON()\` or \`onResult()\` is iterating over the vector, or when \`.clear()\` is called mid-iteration, memory is accessed across deallocated heap chunks, triggering fatal \`LoadProhibited\` / \`StoreProhibited\` panics.

### Location in Code
* [src/main.cpp:49](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L49): Global declaration of \`std::vector<DiscoveredDevice> discoveredDevices\`.
* [src/main.cpp:245-267](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L245-L267): \`onResult()\` mutating and pushing to vector on NimBLE thread.
* [src/main.cpp:723-734](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L723-L734): \`getDevicesJSON()\` iterating over vector in main thread.
* [src/main.cpp:847-850](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L847-L850): \`handleClearDiscoveredCache()\` clearing vector in main thread.

### Proposed Fix
1. Instantiate a FreeRTOS mutex: \`SemaphoreHandle_t devicesMutex = xSemaphoreCreateMutex();\`.
2. Wrap all read and write accesses in RAII lock guards or \`xSemaphoreTake(devicesMutex, portMAX_DELAY)\` / \`xSemaphoreGive(devicesMutex)\`.
3. In \`getDevicesJSON()\`, take the lock briefly to copy or snapshot device summaries."

echo "Issue 2 created."

# Issue 3
gh issue create -R "$REPO" \
  --title "Performance/Stability: Heap Exhaustion and Fragmentation from 45KB Dynamic String HTML Allocation" \
  --label "performance,stability,critical" \
  --body "### Description
The web dashboard HTML/CSS/JS is approximately 45 KB in size. In \`getDashboardHTML()\`, it is built entirely in RAM using hundreds of dynamic \`html += \"...\"\` operations on a dynamic \`String\` object. On an ESP32-C3 with ~320 KB SRAM (where free heap is typically ~60–80 KB), repeatedly allocating and reallocating a 45 KB buffer severely fragments heap memory.

### Failure Mode
When multiple HTTP requests arrive (or during captive portal probe bursts), heap allocation for the string fails, leading to out-of-memory errors, truncated web responses, or allocator panics.

### Location in Code
* [src/main.cpp:996-1765](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L996-L1765): \`getDashboardHTML()\` function building 45 KB dynamic \`String\` across 770 lines.

### Proposed Fix
1. Move the static HTML/CSS/JS into Flash memory using \`const char DASHBOARD_HTML[] PROGMEM = R\"rawliteral(...)rawliteral\";\`.
2. Stream the page directly from flash using \`httpServer.send_P(200, \"text/html\", DASHBOARD_HTML)\`.
3. Optionally, pre-compress the HTML with gzip into a byte array in flash (~8 KB) and serve with header \`Content-Encoding: gzip\`, reducing flash usage and cutting browser load time by ~80%."

echo "Issue 3 created."

# Issue 4
gh issue create -R "$REPO" \
  --title "RF Coexistence: 99% BLE Scan Duty Cycle Starves Wi-Fi and Drops WebSocket Packets" \
  --label "performance,stability,critical" \
  --body "### Description
In \`setup()\`, the BLE scan parameters are configured as:
\`\`\`cpp
pBLEScan->setInterval(100);
pBLEScan->setWindow(99);
\`\`\`
The ESP32-C3 has a single 2.4 GHz RF radio shared between Wi-Fi and Bluetooth. A scan window of 99 ms out of a 100 ms interval reserves 99% of the RF frontend time for BLE scanning. This starves the Wi-Fi modem, causing high Wi-Fi packet loss, ping times spiking over 500 ms, TCP retransmissions, and WebSocket disconnections.

### Failure Mode
Web interface becomes sluggish or unresponsive, API endpoints time out, and WebSocket clients frequently disconnect while a BLE discovery scan is active.

### Location in Code
* [src/main.cpp:1854-1855](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L1854-L1855): \`setInterval(100); setWindow(99);\` in \`setup()\`.

### Proposed Fix
1. Reduce the BLE scan duty cycle to ~25–30% (e.g. \`setInterval(160); setWindow(40);\` or \`setInterval(320); setWindow(80);\`).
2. Stop background scanning immediately once a valid target device is connected."

echo "Issue 4 created."

# Issue 5
gh issue create -R "$REPO" \
  --title "Bug/Performance: Port 5000 HTTP Sniffing False Positives Corrupt Binary Packets and Block Loop" \
  --label "bug,performance,critical" \
  --body "### Description
Port 5000 is dedicated to the binary MeshCore companion stream (\`<len><payload>\`). However, \`handleTCPClientData()\` peeks at the first byte for HTTP verbs:
\`\`\`cpp
if (firstByte == 'G' || firstByte == 'P' || firstByte == 'H' || firstByte == 'O') {
    String req = \"\";
    unsigned long startWait = millis();
    while (tcpClient.connected() && millis() - startWait < 1500) {
        ...
    }
}
\`\`\`
1. Valid binary mesh packets, encrypted radio frames, or public keys whose first payload byte happens to be \`0x47\` ('G'), \`0x50\` ('P'), \`0x48\` ('H'), or \`0x4F\` ('O') are misclassified as HTTP requests.
2. The function enters a blocking \`while (millis() - startWait < 1500)\` loop, freezing the entire ESP32 main loop for up to 1.5 seconds.
3. It prints a 45 KB HTML string over Port 5000 and forcibly closes the client connection.

### Failure Mode
Random disconnection and dropped packets on companion clients when transmitting binary frames starting with specific bytes, alongside periodic 1.5-second system latency spikes.

### Location in Code
* [src/main.cpp:557-593](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L557-L593): HTTP sniffing loop on Port 5000.

### Proposed Fix
1. Strictly separate protocols: Port 80 for HTTP / Web Dashboard, Port 5000 exclusively for binary MeshCore framing.
2. Remove the HTTP sniffing logic from Port 5000, or require a non-blocking match of the full \`\"GET / \"\` prefix without freezing \`loop()\`."

echo "Issue 5 created."

# Issue 6
gh issue create -R "$REPO" \
  --title "Performance/Stability: Blocking delay(250) in connectWiFi() Freezes Event Processing for up to 12 Seconds" \
  --label "performance,stability,critical" \
  --body "### Description
During connection attempts in \`connectWiFi()\`:
\`\`\`cpp
unsigned long start = millis();
while (WiFi.status() != WL_CONNECTED && (millis() - start < (unsigned long)timeoutSec * 1000)) {
    delay(250);
    Serial.print(\".\");
    if (isAPMode) {
        dnsServer.processNextRequest();
        httpServer.handleClient();
    }
}
\`\`\`
When connecting in station mode (\`!isAPMode\`), this loop blocks the CPU for up to 12 seconds with repeated \`delay(250)\` calls. During this time:
- \`wsServer.loop()\` is completely frozen (dropping active WebSocket connections).
- \`handleTCPClientData()\` is not serviced.
- BLE bridging routines cannot process or forward packets.

### Failure Mode
Communication blackout on all active client channels whenever a Wi-Fi connection attempt or reconnect cycle occurs.

### Location in Code
* [src/main.cpp:367-375](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L367-L375): Blocking while loop in \`connectWiFi()\`.

### Proposed Fix
1. Refactor Wi-Fi connection handling into a non-blocking asynchronous state machine in \`loop()\`.
2. Alternatively, register an event handler with \`WiFi.onEvent()\` (\`ARDUINO_EVENT_WIFI_STA_GOT_IP\`, \`ARDUINO_EVENT_WIFI_STA_DISCONNECTED\`) to handle state changes asynchronously without blocking delays."

echo "Issue 6 created."

# Issue 7
gh issue create -R "$REPO" \
  --title "Reliability: Single-Client TCP Server Limitation and Lack of Socket Keep-Alive on Port 5000" \
  --label "reliability,stability,critical" \
  --body "### Description
The bridge only tracks a single global \`WiFiClient tcpClient\`:
\`\`\`cpp
if (!tcpClient || !tcpClient.connected()) {
    WiFiClient newClient = tcpServer.accept();
    ...
}
\`\`\`
1. While a client is connected, all other connection attempts on Port 5000 are ignored and queue in the socket backlog.
2. If a client drops ungracefully (e.g. laptop sleep, Wi-Fi roaming, ungraceful socket termination without TCP FIN/RST), \`tcpClient.connected()\` remains true until lwIP times out (which can take several minutes).
3. During this dead-socket window, no other client can connect to Port 5000.

### Failure Mode
Port 5000 becomes unresponsive to new client connections for minutes after a network interruption or abnormal client disconnect.

### Location in Code
* [src/main.cpp:84](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L84): Global \`static WiFiClient tcpClient;\`.
* [src/main.cpp:1888-1898](https://github.com/koosc/esp32-meshcore-bluetooth-websocket-bridge/blob/main/src/main.cpp#L1888-L1898): Single client accept logic in \`loop()\`.

### Proposed Fix
1. Enable TCP socket keep-alive on newly accepted connections via \`setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, ...)\` with \`TCP_KEEPIDLE = 5\`, \`TCP_KEEPINTVL = 3\`, \`TCP_KEEPCNT = 3\`.
2. Support a small client pool (e.g. array of up to 4 \`WiFiClient\` instances) so multiple monitoring sessions can coexist."

echo "Issue 7 created."

echo "All 7 critical issues created successfully!"
