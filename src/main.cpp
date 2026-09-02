#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLE2902.h>
#include <BLESecurity.h>

// ================= Configuration =================
#define BRIDGE_VERSION            "1.3.0"
#define DEFAULT_BLE_PIN           808978
#define MESHCORE_SERVICE_UUID     "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define MESHCORE_RX_CHAR_UUID     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define MESHCORE_TX_CHAR_UUID     "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

#define WIFI_SSID                 "YOUR_WIFI_SSID"
#define WIFI_PASS                 "YOUR_WIFI_PASSWORD"
#define TCP_PORT                  5000
#define WS_PORT                   5001
#define HTTP_PORT                 80

#define MAX_FRAME_SIZE            172
#define USB_SERIAL_TX_FRAME_START 0x3c // '<'
#define USB_SERIAL_RX_FRAME_START 0x3e // '>'

// ================= State Variables =================
static BLEAddress* pTargetAddress = nullptr;
static BLEClient* pClient = nullptr;
static BLERemoteCharacteristic* pRxCharacteristic = nullptr;
static BLERemoteCharacteristic* pTxCharacteristic = nullptr;

static bool bleConnecting = false;
static bool bleConnected = false;
static String bleConnectedDeviceName = "";
static String bleConnectedAddress = "";
static int bleLastRSSI = 0;
static uint32_t blePacketsRx = 0;
static uint32_t blePacketsTx = 0;
static uint32_t bleConnectTime = 0;
static uint32_t blePinCode = DEFAULT_BLE_PIN;

static WiFiServer tcpServer(TCP_PORT);
static WiFiClient tcpClient;
static WebSocketsServer wsServer(WS_PORT);
static WebServer httpServer(HTTP_PORT);

// TCP frame parser state
static uint8_t tcpRxBuffer[MAX_FRAME_SIZE + 4];
static size_t tcpRxLen = 0;
static size_t tcpExpectedLen = 0;
static uint8_t tcpParseState = 0; // 0: Idle, 1: Got '<', 2: Got Len1, 3: Reading payload

// Global Statistics
static uint32_t tcpClientsTotal = 0;
static uint32_t wsClientsTotal = 0;
static uint32_t tcpPacketsRx = 0;
static uint32_t tcpPacketsTx = 0;
static uint32_t wsPacketsRx = 0;
static uint32_t wsPacketsTx = 0;

// Forward declarations
void startBLEScan();
bool connectToMeshCoreDevice();
void sendFrameToMeshCore(const uint8_t* data, size_t len);
void broadcastFrameToClients(const uint8_t* data, size_t len);
String getDashboardHTML();
String getStatusJSON();

// ================= BLE Security Callbacks =================
class BridgeSecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override {
        Serial.printf("[BLE SEC] PIN requested -> returning %u\n", blePinCode);
        return blePinCode;
    }
    void onPassKeyNotify(uint32_t pass_key) override {
        Serial.printf("[BLE SEC] Passkey notify: %06u\n", pass_key);
    }
    bool onConfirmPIN(uint32_t pass_key) override {
        Serial.printf("[BLE SEC] Confirm PIN: %06u -> accepted\n", pass_key);
        return true;
    }
    bool onSecurityRequest() override {
        Serial.println("[BLE SEC] Security request -> accepted");
        return true;
    }
#if defined(CONFIG_NIMBLE_ENABLED)
    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        Serial.printf("[BLE SEC] Auth complete: enc=%d, auth=%d, bond=%d\n",
                      desc->sec_state.encrypted, desc->sec_state.authenticated, desc->sec_state.bonded);
    }
#endif
};

// ================= BLE Notifications (From MeshCore device) =================
static void onBLETxNotify(
  BLERemoteCharacteristic* pCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
    if (length == 0 || length > MAX_FRAME_SIZE) return;

    blePacketsRx++;
    Serial.printf("[BLE -> BRIDGE] Got %d bytes (code 0x%02X)\n", length, pData[0]);

    // Forward to all active TCP and WebSocket clients
    broadcastFrameToClients(pData, length);
}

// ================= BLE Client Callbacks =================
class BridgeClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) override {
        Serial.println("[BLE] Connected to MeshCore device!");
        bleConnected = true;
        bleConnecting = false;
        bleConnectTime = millis();
    }
    void onDisconnect(BLEClient* pclient) override {
        Serial.println("[BLE] Disconnected from MeshCore device!");
        bleConnected = false;
        bleConnecting = false;
        pRxCharacteristic = nullptr;
        pTxCharacteristic = nullptr;
    }
};

// ================= BLE Advertised Device Callbacks =================
class BridgeAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        String name = advertisedDevice.getName().c_str();
        String addr = advertisedDevice.getAddress().toString().c_str();
        int rssi = advertisedDevice.getRSSI();

        bool matchUUID = advertisedDevice.isAdvertisingService(BLEUUID(MESHCORE_SERVICE_UUID));
        bool matchName = name.startsWith("MeshCore") ||
                         name.startsWith("Whisper") ||
                         name.startsWith("WisCore") ||
                         name.startsWith("HT-") ||
                         name.startsWith("Seeed") ||
                         name.startsWith("Lilygo") ||
                         name.startsWith("LowMesh");

        if (matchUUID || matchName) {
            Serial.printf("[SCAN] Found MeshCore candidate: '%s' (%s, RSSI: %d dBm)\n",
                          name.c_str(), addr.c_str(), rssi);
            BLEDevice::getScan()->stop();

            if (pTargetAddress) delete pTargetAddress;
            pTargetAddress = new BLEAddress(advertisedDevice.getAddress());
            bleConnectedDeviceName = name.length() > 0 ? name : "MeshCore Device";
            bleConnectedAddress = addr;
            bleLastRSSI = rssi;
            bleConnecting = true;
        }
    }
};

void startBLEScan() {
    if (bleConnected || bleConnecting) return;
    Serial.println("[BLE] Starting scan for MeshCore devices...");
    BLEScan* pScan = BLEDevice::getScan();
    pScan->clearResults();
    pScan->start(5, false);
}

bool connectToMeshCoreDevice() {
    if (!pTargetAddress) return false;

    Serial.printf("[BLE] Connecting to %s (%s)...\n",
                  bleConnectedDeviceName.c_str(), pTargetAddress->toString().c_str());

    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new BridgeClientCallbacks());

    if (!pClient->connect(*pTargetAddress)) {
        Serial.println("[BLE] Connection failed.");
        bleConnecting = false;
        return false;
    }

    Serial.println("[BLE] Setting MTU to 256...");
    pClient->setMTU(256);

    BLERemoteService* pRemoteService = pClient->getService(BLEUUID(MESHCORE_SERVICE_UUID));
    if (!pRemoteService) {
        Serial.println("[BLE] NUS Service not found.");
        pClient->disconnect();
        bleConnecting = false;
        return false;
    }

    pRxCharacteristic = pRemoteService->getCharacteristic(BLEUUID(MESHCORE_RX_CHAR_UUID));
    if (!pRxCharacteristic) {
        Serial.println("[BLE] RX Characteristic not found.");
        pClient->disconnect();
        bleConnecting = false;
        return false;
    }

    pTxCharacteristic = pRemoteService->getCharacteristic(BLEUUID(MESHCORE_TX_CHAR_UUID));
    if (!pTxCharacteristic) {
        Serial.println("[BLE] TX Characteristic not found.");
        pClient->disconnect();
        bleConnecting = false;
        return false;
    }

    if (pTxCharacteristic->canNotify()) {
        pTxCharacteristic->registerForNotify(onBLETxNotify);
        Serial.println("[BLE] Subscribed to TX notifications successfully!");
    }

    bleConnected = true;
    bleConnecting = false;
    return true;
}

// Send frame from clients over BLE to MeshCore RX characteristic
void sendFrameToMeshCore(const uint8_t* data, size_t len) {
    if (!bleConnected || !pRxCharacteristic) {
        Serial.printf("[BRIDGE] Dropping TX frame (%d bytes) - BLE not connected\n", len);
        return;
    }
    if (len > MAX_FRAME_SIZE) {
        Serial.printf("[BRIDGE] Frame too large (%d > %d)\n", len, MAX_FRAME_SIZE);
        return;
    }

    blePacketsTx++;
    Serial.printf("[BRIDGE -> BLE] Sending %d bytes to MeshCore (cmd 0x%02X)\n", len, data[0]);
    pRxCharacteristic->writeValue((uint8_t*)data, len, true);
}

// Wrap frame with MeshCore USB/TCP framing: '>' [len LSB] [len MSB] [payload]
void broadcastFrameToClients(const uint8_t* data, size_t len) {
    if (len > MAX_FRAME_SIZE) return;

    uint8_t packet[MAX_FRAME_SIZE + 3];
    packet[0] = USB_SERIAL_RX_FRAME_START; // '>'
    packet[1] = (uint8_t)(len & 0xFF);
    packet[2] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(packet + 3, data, len);
    size_t packetLen = len + 3;

    // Send to TCP client
    if (tcpClient && tcpClient.connected()) {
        tcpClient.write(packet, packetLen);
        tcpPacketsTx++;
    }

    // Send to WebSocket clients
    if (wsServer.connectedClients() > 0) {
        wsServer.broadcastBIN(packet, packetLen);
        wsPacketsTx++;
    }
}

// Parse incoming data from TCP stream on port 5000 (handles MeshCore frames AND browser HTTP requests)
void handleTCPClientData() {
    if (!tcpClient || !tcpClient.connected()) {
        if (tcpClient) {
            tcpClient.stop();
            Serial.println("[TCP] Client disconnected.");
        }
        return;
    }

    if (!tcpClient.available()) return;

    // If waiting for start of frame, check if this is an HTTP request from a browser
    if (tcpParseState == 0) {
        int firstByte = tcpClient.peek();
        if (firstByte == 'G' || firstByte == 'P' || firstByte == 'H' || firstByte == 'O') {
            String req = "";
            unsigned long startWait = millis();
            while (tcpClient.connected() && millis() - startWait < 1500) {
                while (tcpClient.available()) {
                    char c = tcpClient.read();
                    req += c;
                    if (req.endsWith("\r\n\r\n")) break;
                }
                if (req.endsWith("\r\n\r\n")) break;
                delay(2);
            }
            Serial.printf("[TCP:5000] HTTP Request: %s\n", req.substring(0, req.indexOf('\r')).c_str());

            if (req.indexOf("GET /status") >= 0) {
                String json = getStatusJSON();
                String resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + String(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                tcpClient.print(resp);
            } else {
                String html = getDashboardHTML();
                String resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + String(html.length()) + "\r\nConnection: close\r\n\r\n" + html;
                tcpClient.print(resp);
            }
            tcpClient.flush();
            tcpClient.stop();
            return;
        }
    }

    // Binary MeshCore companion stream parser
    while (tcpClient.available()) {
        int c = tcpClient.read();
        if (c < 0) break;

        switch (tcpParseState) {
            case 0: // Waiting for '<'
                if (c == USB_SERIAL_TX_FRAME_START) {
                    tcpParseState = 1;
                }
                break;
            case 1: // Got '<', read length LSB
                tcpExpectedLen = (uint8_t)c;
                tcpParseState = 2;
                break;
            case 2: // Read length MSB
                tcpExpectedLen |= ((uint16_t)c) << 8;
                tcpRxLen = 0;
                if (tcpExpectedLen == 0 || tcpExpectedLen > MAX_FRAME_SIZE) {
                    tcpParseState = 0; // Invalid length
                } else {
                    tcpParseState = 3;
                }
                break;
            case 3: // Read payload
                tcpRxBuffer[tcpRxLen++] = (uint8_t)c;
                if (tcpRxLen >= tcpExpectedLen) {
                    tcpPacketsRx++;
                    sendFrameToMeshCore(tcpRxBuffer, tcpExpectedLen);
                    tcpParseState = 0;
                }
                break;
        }
    }
}

// ================= WebSocket Event Handler =================
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WS] Client #%u disconnected.\n", num);
            break;
        case WStype_CONNECTED: {
            IPAddress ip = wsServer.remoteIP(num);
            Serial.printf("[WS] Client #%u connected from %s\n", num, ip.toString().c_str());
            wsClientsTotal++;
            break;
        }
        case WStype_BIN: {
            if (length == 0) break;
            wsPacketsRx++;

            // Check if client sent framed packet starting with '<' (0x3C)
            if (payload[0] == USB_SERIAL_TX_FRAME_START && length >= 3) {
                size_t payloadLen = payload[1] | (payload[2] << 8);
                if (length >= 3 + payloadLen) {
                    sendFrameToMeshCore(payload + 3, payloadLen);
                    break;
                }
            }
            // Or raw frame
            sendFrameToMeshCore(payload, length > MAX_FRAME_SIZE ? MAX_FRAME_SIZE : length);
            break;
        }
        case WStype_TEXT:
            Serial.printf("[WS] Text message from #%u: %s\n", num, payload);
            break;
        default:
            break;
    }
}

// ================= HTTP Content Generators =================
String getDashboardHTML() {
    String localIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Connecting...";
    String html = "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ESP32 MeshCore Console</title>";
    html += "<style>";
    html += ":root { --bg: #0d1117; --panel: #161b22; --border: #30363d; --text: #c9d1d9; --accent: #58a6ff; --green: #238636; --green-txt: #3fb950; --red: #da3633; --yellow: #d29922; }";
    html += "* { box-sizing: border-box; }";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 16px; }";
    html += ".container { max-width: 900px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }";
    html += ".header { display: flex; justify-content: space-between; align-items: center; background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 12px 16px; }";
    html += ".title { font-size: 20px; font-weight: bold; color: var(--accent); display: flex; align-items: center; gap: 8px; }";
    html += ".badge { padding: 4px 10px; border-radius: 12px; font-size: 12px; font-weight: bold; display: inline-flex; align-items: center; gap: 4px; }";
    html += ".bg-green { background: rgba(35,134,54,0.2); color: var(--green-txt); border: 1px solid var(--green); }";
    html += ".bg-red { background: rgba(218,54,51,0.2); color: #f85149; border: 1px solid var(--red); }";
    html += ".bg-yellow { background: rgba(210,153,34,0.2); color: var(--yellow); border: 1px solid var(--yellow); }";
    html += ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }";
    html += ".card { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 16px; }";
    html += ".card-title { font-size: 13px; font-weight: bold; text-transform: uppercase; color: #8b949e; letter-spacing: 0.5px; margin-bottom: 12px; }";
    html += ".row { display: flex; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid rgba(255,255,255,0.05); font-size: 13px; }";
    html += ".row:last-child { border-bottom: none; }";
    html += ".label { color: #8b949e; }";
    html += ".val { font-family: monospace; font-weight: bold; color: #f0f6fc; }";
    html += ".btn { background: #21262d; border: 1px solid var(--border); color: #c9d1d9; padding: 8px 14px; border-radius: 6px; font-size: 13px; font-weight: 500; cursor: pointer; transition: all 0.2s; display: inline-flex; align-items: center; gap: 6px; }";
    html += ".btn:hover { background: #30363d; border-color: #8b949e; }";
    html += ".btn-primary { background: var(--green); border-color: rgba(240,246,252,0.1); color: #fff; }";
    html += ".btn-primary:hover { background: #2ea043; }";
    html += ".btn-blue { background: #1f6feb; border-color: rgba(240,246,252,0.1); color: #fff; }";
    html += ".btn-blue:hover { background: #388bfd; }";
    html += ".btn-group { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 8px; }";
    html += "input, select, textarea { background: #0d1117; border: 1px solid var(--border); color: #c9d1d9; padding: 8px 12px; border-radius: 6px; font-size: 14px; width: 100%; outline: none; }";
    html += "input:focus, select:focus, textarea:focus { border-color: var(--accent); }";
    html += ".chat-box { height: 260px; overflow-y: auto; background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 12px; display: flex; flex-direction: column; gap: 8px; }";
    html += ".chat-msg { background: #161b22; border: 1px solid var(--border); border-radius: 6px; padding: 8px 12px; font-size: 13px; }";
    html += ".chat-msg.tx { border-left: 3px solid var(--accent); }";
    html += ".chat-msg.rx { border-left: 3px solid var(--green-txt); }";
    html += ".chat-msg.sys { border-left: 3px solid var(--yellow); color: #8b949e; font-size: 12px; }";
    html += ".chat-sender { font-weight: bold; color: var(--accent); margin-bottom: 2px; display: flex; justify-content: space-between; font-size: 12px; }";
    html += ".log-box { height: 220px; overflow-y: auto; background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-family: monospace; font-size: 12px; }";
    html += ".log-line { padding: 3px 6px; border-radius: 4px; margin-bottom: 2px; }";
    html += ".log-tx { color: #58a6ff; background: rgba(88,166,255,0.08); }";
    html += ".log-rx { color: #3fb950; background: rgba(63,185,80,0.08); }";
    html += ".log-err { color: #f85149; background: rgba(248,81,73,0.08); }";
    html += ".log-info { color: #8b949e; }";
    html += "</style></head><body>";
    html += "<div class='container'>";

    // Header
    html += "<div class='header'>";
    html += "<div class='title'>⚡ MeshCore BLE Bridge</div>";
    html += "<div style='display:flex; gap:8px;'>";
    html += "<span id='bleBadge' class='badge bg-yellow'>BLE: Scanning...</span>";
    html += "<span id='wsBadge' class='badge bg-yellow'>WS: Connecting...</span>";
    html += "</div></div>";

    // Main Grid
    html += "<div class='grid'>";

    // Card 1: Node Info & Radio
    html += "<div class='card'>";
    html += "<div class='card-title'>Connected Radio Profile</div>";
    html += "<div class='row'><span class='label'>Node Name</span><span class='val' id='valNodeName'>-</span></div>";
    html += "<div class='row'><span class='label'>Public Key</span><span class='val' id='valPubKey' style='font-size:11px; word-break:break-all;'>-</span></div>";
    html += "<div class='row'><span class='label'>Battery</span><span class='val' id='valBattery'>-</span></div>";
    html += "<div class='row'><span class='label'>LoRa Frequency</span><span class='val' id='valFreq'>-</span></div>";
    html += "<div class='row'><span class='label'>Bandwidth / SF / CR</span><span class='val' id='valRadioParams'>-</span></div>";
    html += "<div class='row'><span class='label'>TX Power</span><span class='val' id='valTxPower'>-</span></div>";
    html += "<div class='btn-group' style='margin-top:12px;'>";
    html += "<button class='btn btn-primary' onclick='cmdAppStart()'>🔄 Query Node</button>";
    html += "<button class='btn' onclick='cmdSendAdvert()'>📡 Send Beacon / Advert</button>";
    html += "<button class='btn' onclick='cmdGetBattery()'>🔋 Battery</button>";
    html += "<button class='btn' onclick='cmdSyncTime()'>⏱️ Sync Time</button>";
    html += "</div></div>";

    // Card 2: Bridge & Network Status
    html += "<div class='card'>";
    html += "<div class='card-title'>Bridge Endpoints</div>";
    html += "<div class='row'><span class='label'>WiFi Network</span><span class='val'>" + String(WIFI_SSID) + "</span></div>";
    html += "<div class='row'><span class='label'>Bridge IP</span><span class='val'>" + localIP + "</span></div>";
    html += "<div class='row'><span class='label'>Native TCP Port</span><span class='val'>" + localIP + ":5000</span></div>";
    html += "<div class='row'><span class='label'>WebSocket Port</span><span class='val'>ws://" + localIP + ":5001</span></div>";
    html += "<div class='row'><span class='label'>BLE Device</span><span class='val'>" + (bleConnectedDeviceName.length() > 0 ? bleConnectedDeviceName : "-") + " (" + String(bleLastRSSI) + " dBm)</span></div>";
    html += "<div class='row'><span class='label'>BLE Packets (RX / TX)</span><span class='val'>" + String(blePacketsRx) + " / " + String(blePacketsTx) + "</span></div>";
    html += "<div class='btn-group' style='margin-top:12px;'>";
    html += "<button class='btn' onclick='cmdPollMessages()'>📩 Check Messages</button>";
    html += "<button class='btn' onclick='location.reload()'>🔄 Refresh Page</button>";
    html += "</div></div>";

    html += "</div>"; // End grid

    // Card 3: Chat & Messaging Interface
    html += "<div class='card'>";
    html += "<div class='card-title'>Channel Messages & Mesh Chat</div>";
    html += "<div class='chat-box' id='chatBox'>";
    html += "<div class='chat-msg sys'>Mesh chat initialized. Send a message below to broadcast over LoRa mesh!</div>";
    html += "</div>";
    html += "<div style='display:flex; gap:8px; margin-top:10px;'>";
    html += "<select id='channelSelect' style='width:140px;'>";
    html += "<option value='0'>Channel 0 (Public)</option>";
    html += "<option value='1'>Channel 1</option>";
    html += "<option value='2'>Channel 2</option>";
    html += "<option value='3'>Channel 3</option>";
    html += "</select>";
    html += "<input type='text' id='msgInput' placeholder='Type a message to send over LoRa mesh...' onkeypress='if(event.key===\"Enter\") sendMessage()' />";
    html += "<button class='btn btn-blue' onclick='sendMessage()'>Send 📤</button>";
    html += "</div>";
    html += "</div>";

    // Card 4: Packet Inspector & Raw Console
    html += "<div class='card'>";
    html += "<div class='card-title' style='display:flex; justify-content:space-between; align-items:center;'>";
    html += "<span>Live Protocol Packet Stream</span>";
    html += "<button class='btn' style='padding:2px 8px; font-size:11px;' onclick='document.getElementById(\"logBox\").innerHTML=\"\"'>Clear</button>";
    html += "</div>";
    html += "<div class='log-box' id='logBox'></div>";
    html += "<div style='display:flex; gap:8px; margin-top:8px;'>";
    html += "<input type='text' id='hexInput' placeholder='Send raw Hex command (e.g. 01 04 00 00 00 00 00 00 42 72 69 64 67 65)' />";
    html += "<button class='btn' onclick='sendRawHex()'>Send Hex</button>";
    html += "</div></div>";

    // Client-side JavaScript
    html += "<script>";
    html += "let ws = null;";
    html += "let myNodeName = 'BridgeWeb';";
    html += "const logBox = document.getElementById('logBox');";
    html += "const chatBox = document.getElementById('chatBox');";

    html += "function log(msg, type='info') {";
    html += "  const time = new Date().toLocaleTimeString();";
    html += "  const div = document.createElement('div');";
    html += "  div.className = 'log-line log-' + type;";
    html += "  div.textContent = `[${time}] ${msg}`;";
    html += "  logBox.appendChild(div);";
    html += "  logBox.scrollTop = logBox.scrollHeight;";
    html += "}";

    html += "function addChat(sender, text, type='rx', timeStr) {";
    html += "  const time = timeStr || new Date().toLocaleTimeString();";
    html += "  const div = document.createElement('div');";
    html += "  div.className = 'chat-msg ' + type;";
    html += "  div.innerHTML = `<div class='chat-sender'><span>${sender}</span><span>${time}</span></div><div>${text}</div>`;";
    html += "  chatBox.appendChild(div);";
    html += "  chatBox.scrollTop = chatBox.scrollHeight;";
    html += "}";

    html += "function wrap(payload) {";
    html += "  const buf = new Uint8Array(3 + payload.length);";
    html += "  buf[0] = 0x3c;";
    html += "  buf[1] = payload.length & 0xff;";
    html += "  buf[2] = (payload.length >> 8) & 0xff;";
    html += "  buf.set(payload, 3);";
    html += "  return buf;";
    html += "}";

    html += "function send(payload, label='CMD') {";
    html += "  if (!ws || ws.readyState !== WebSocket.OPEN) {";
    html += "    log('Cannot send: WebSocket disconnected', 'err');";
    html += "    return;";
    html += "  }";
    html += "  const packet = wrap(payload);";
    html += "  ws.send(packet.buffer);";
    html += "  const hex = Array.from(payload).map(b => b.toString(16).padStart(2,'0').toUpperCase()).join(' ');";
    html += "  log(`TX [${label}]: ${hex}`, 'tx');";
    html += "}";

    html += "function cmdAppStart() {";
    html += "  const nameBytes = new TextEncoder().encode('WebConsole');";
    html += "  const payload = new Uint8Array(8 + nameBytes.length);";
    html += "  payload[0] = 0x01; payload[1] = 0x04;";
    html += "  payload.set(nameBytes, 8);";
    html += "  send(payload, 'CMD_APP_START');";
    html += "}";

    html += "function cmdSendAdvert() {";
    html += "  send(new Uint8Array([0x07]), 'CMD_SEND_ADVERT');";
    html += "  addChat('System', 'Broadcasted node advertisement/beacon over mesh', 'sys');";
    html += "}";

    html += "function cmdGetBattery() {";
    html += "  send(new Uint8Array([0x14]), 'CMD_GET_BATTERY');";
    html += "}";

    html += "function cmdSyncTime() {";
    html += "  const now = Math.floor(Date.now() / 1000);";
    html += "  const p = new Uint8Array([0x12, now & 0xff, (now>>8)&0xff, (now>>16)&0xff, (now>>24)&0xff]);";
    html += "  send(p, 'CMD_SET_TIME');";
    html += "  addChat('System', 'Synchronized node clock with local time', 'sys');";
    html += "}";

    html += "function cmdPollMessages() {";
    html += "  send(new Uint8Array([0x0a]), 'CMD_SYNC_NEXT_MESSAGE');";
    html += "}";

    html += "function sendMessage() {";
    html += "  const input = document.getElementById('msgInput');";
    html += "  const text = input.value.trim();";
    html += "  if (!text) return;";
    html += "  const chan = parseInt(document.getElementById('channelSelect').value, 10);";
    html += "  const txtBytes = new TextEncoder().encode(text);";
    html += "  const ts = Math.floor(Date.now() / 1000);";
    html += "  const p = new Uint8Array(7 + txtBytes.length);";
    html += "  p[0] = 0x03; p[1] = 0x00; p[2] = chan & 0x07;";
    html += "  p[3] = ts & 0xff; p[4] = (ts>>8)&0xff; p[5] = (ts>>16)&0xff; p[6] = (ts>>24)&0xff;";
    html += "  p.set(txtBytes, 7);";
    html += "  send(p, `SEND_CHAN_MSG[ch${chan}]`);";
    html += "  addChat(`Me (Ch ${chan})`, text, 'tx');";
    html += "  input.value = '';";
    html += "}";

    html += "function sendRawHex() {";
    html += "  const str = document.getElementById('hexInput').value.replace(/\\s+/g, '');";
    html += "  if (!str) return;";
    html += "  const bytes = new Uint8Array(str.match(/.{1,2}/g).map(byte => parseInt(byte, 16)));";
    html += "  send(bytes, 'RAW_HEX');";
    html += "}";

    html += "function parsePacket(bytes) {";
    html += "  let p = bytes;";
    html += "  if (bytes.length >= 3 && bytes[0] === 0x3e) {";
    html += "    const len = bytes[1] | (bytes[2] << 8);";
    html += "    p = bytes.slice(3, 3 + len);";
    html += "  }";
    html += "  const hex = Array.from(p).map(b => b.toString(16).padStart(2,'0').toUpperCase()).join(' ');";
    html += "  const code = p[0];";
    html += "  log(`RX [Code 0x${code.toString(16).padStart(2,'0')}]: ${hex}`, 'rx');";

    html += "  if (code === 0x05) {"; // RESP_CODE_SELF_INFO
    html += "    const pubKeyHex = Array.from(p.slice(4, 36)).map(b => b.toString(16).padStart(2,'0')).join('');";
    html += "    document.getElementById('valPubKey').textContent = pubKeyHex;";
    html += "    if (p.length >= 60) {";
    html += "      const nameLen = p[57];";
    html += "      if (nameLen > 0 && p.length >= 58 + nameLen) {";
    html += "        const name = new TextDecoder().decode(p.slice(58, 58 + nameLen));";
    html += "        document.getElementById('valNodeName').textContent = name;";
    html += "      }";
    html += "    }";
    html += "    const freq = ((p[44] | (p[45]<<8) | (p[46]<<16) | (p[47]<<24)) / 1000).toFixed(2);";
    html += "    const bw = ((p[48] | (p[49]<<8) | (p[50]<<16) | (p[51]<<24)) / 1000).toFixed(1);";
    html += "    const sf = p[52];";
    html += "    const cr = p[53];";
    html += "    const txPower = p[2];";
    html += "    document.getElementById('valFreq').textContent = `${freq} MHz`;";
    html += "    document.getElementById('valRadioParams').textContent = `BW ${bw}k / SF${sf} / CR 4/${cr}`;";
    html += "    document.getElementById('valTxPower').textContent = `${txPower} dBm`;";
    html += "    addChat('System', 'Received node profile info from MeshCore', 'sys');";
    html += "  } else if (code === 0x06) {"; // RESP_CODE_MSG_SENT
    html += "    log('Message broadcast queued on mesh radio', 'info');";
    html += "  } else if (code === 0x08 || code === 0x11) {"; // CHANNEL_MSG_RECV
    html += "    const chan = p[4] || 0;";
    html += "    const senderBytes = p.slice(5, 37);";
    html += "    const senderHex = Array.from(senderBytes.slice(0,4)).map(b => b.toString(16).padStart(2,'0')).join('');";
    html += "    const text = new TextDecoder().decode(p.slice(37));";
    html += "    addChat(`Node ${senderHex} (Ch ${chan})`, text, 'rx');";
    html += "  } else if (code === 0x0c) {"; // BATTERY
    html += "    const mv = p[1] | (p[2] << 8);";
    html += "    document.getElementById('valBattery').textContent = `${(mv/1000).toFixed(2)} V (${mv} mV)`;";
    html += "  } else if (code === 0x0a) {"; // NO_MORE_MSGS
    html += "    log('No more queued messages on node', 'info');";
    html += "  }";
    html += "}";

    html += "function connectWS() {";
    html += "  const wsUrl = 'ws://' + location.hostname + ':5001';";
    html += "  log(`Connecting WebSocket to ${wsUrl}...`);";
    html += "  ws = new WebSocket(wsUrl);";
    html += "  ws.binaryType = 'arraybuffer';";
    html += "  ws.onopen = () => {";
    html += "    document.getElementById('wsBadge').className = 'badge bg-green';";
    html += "    document.getElementById('wsBadge').textContent = 'WS: Connected';";
    html += "    log('WebSocket connected successfully!', 'info');";
    html += "    setTimeout(cmdAppStart, 200);";
    html += "  };";
    html += "  ws.onclose = () => {";
    html += "    document.getElementById('wsBadge').className = 'badge bg-red';";
    html += "    document.getElementById('wsBadge').textContent = 'WS: Disconnected';";
    html += "    setTimeout(connectWS, 3000);";
    html += "  };";
    html += "  ws.onerror = (e) => log('WebSocket error', 'err');";
    html += "  ws.onmessage = async (e) => {";
    html += "    const buf = new Uint8Array(e.data);";
    html += "    parsePacket(buf);";
    html += "  };";
    html += "}";

    html += "function checkStatus() {";
    html += "  fetch('/status').then(r => r.json()).then(d => {";
    html += "    const b = document.getElementById('bleBadge');";
    html += "    if (d.ble_connected) {";
    html += "      b.className = 'badge bg-green';";
    html += "      b.textContent = `BLE: ${d.ble_device_name} (${d.ble_rssi} dBm)`;";
    html += "    } else {";
    html += "      b.className = 'badge bg-yellow';";
    html += "      b.textContent = 'BLE: Connecting...';";
    html += "    }";
    html += "  }).catch(() => {});";
    html += "}";

    html += "window.onload = () => {";
    html += "  connectWS();";
    html += "  checkStatus();";
    html += "  setInterval(checkStatus, 5000);";
    html += "};";
    html += "</script>";

    html += "</div></body></html>";
    return html;
}

String getStatusJSON() {
    String json = "{";
    json += "\"version\":\"" + String(BRIDGE_VERSION) + "\",";
    json += "\"ble_connected\":" + String(bleConnected ? "true" : "false") + ",";
    json += "\"ble_device_name\":\"" + bleConnectedDeviceName + "\",";
    json += "\"ble_mac\":\"" + bleConnectedAddress + "\",";
    json += "\"ble_rssi\":" + String(bleLastRSSI) + ",";
    json += "\"ble_rx\":" + String(blePacketsRx) + ",";
    json += "\"ble_tx\":" + String(blePacketsTx) + ",";
    json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"wifi_ssid\":\"" + String(WIFI_SSID) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"ip_address\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",";
    json += "\"tcp_port\":" + String(TCP_PORT) + ",";
    json += "\"ws_port\":" + String(WS_PORT) + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime_sec\":" + String(millis() / 1000);
    json += "}";
    return json;
}

void handleRoot() {
    httpServer.send(200, "text/html", getDashboardHTML());
}

void handleStatusJSON() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.send(200, "application/json", getStatusJSON());
}

// ================= Setup & Loop =================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n========================================");
    Serial.printf("  ESP32-C3 MeshCore BLE Bridge v%s\n", BRIDGE_VERSION);
    Serial.println("========================================");

    // 1. Connect to WiFi Network (Station mode)
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // Disable power save mode for instant packet processing
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);

    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected! IP: %s | Gateway: %s | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str(),
                      WiFi.RSSI());
    } else {
        Serial.println("[WiFi] Connection in progress in background...");
    }

    // 2. Start mDNS
    if (MDNS.begin("meshcore-ble-bridge")) {
        Serial.println("[mDNS] Responder started: http://meshcore-ble-bridge.local");
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("meshcore", "tcp", TCP_PORT);
        MDNS.addService("meshcore-ws", "tcp", WS_PORT);
    }

    // 3. Start TCP Server on port 5000
    tcpServer.begin();
    tcpServer.setNoDelay(true);
    Serial.printf("[TCP] Server listening on port %d\n", TCP_PORT);

    // 4. Start WebSocket Server on port 5001
    wsServer.begin();
    wsServer.onEvent(onWebSocketEvent);
    Serial.printf("[WS] Server listening on port %d\n", WS_PORT);

    // 5. Start HTTP Web Server on port 80
    httpServer.on("/", handleRoot);
    httpServer.on("/status", handleStatusJSON);
    httpServer.enableCORS(true);
    httpServer.begin();
    Serial.printf("[HTTP] Web dashboard ready on port %d\n", HTTP_PORT);

    // 6. Initialize BLE Device and Security
    BLEDevice::init("ESP32C3-MeshBridge");
    BLEDevice::setSecurityCallbacks(new BridgeSecurityCallbacks());

    BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    BLESecurity::setCapability(ESP_IO_CAP_KBDISP);
    BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    BLESecurity::setKeySize(16);
    BLESecurity::setPassKey(true, blePinCode);

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new BridgeAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println("[BLE] Starting initial scan for MeshCore devices...");
    startBLEScan();
}

static unsigned long lastScanAttempt = 0;
static unsigned long lastStatusPrint = 0;
static unsigned long lastWiFiCheck = 0;

void loop() {
    // 1. Handle HTTP web requests (port 80)
    httpServer.handleClient();

    // 2. Handle WebSockets (port 5001)
    wsServer.loop();

    // 3. Handle incoming TCP connections (port 5000)
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient newClient = tcpServer.accept();
        if (newClient) {
            tcpClient = newClient;
            tcpClient.setNoDelay(true);
            tcpParseState = 0;
            tcpRxLen = 0;
            tcpClientsTotal++;
            Serial.printf("[TCP] New client connected from %s:%d\n",
                          tcpClient.remoteIP().toString().c_str(), tcpClient.remotePort());
        }
    } else {
        handleTCPClientData();
    }

    // 4. Handle WiFi reconnection
    if (millis() - lastWiFiCheck > 10000) {
        lastWiFiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Reconnecting...");
            WiFi.reconnect();
        }
    }

    // 5. Handle BLE Connection state machine
    if (bleConnecting) {
        if (connectToMeshCoreDevice()) {
            Serial.println("[BLE] Connection established and ready to bridge!");
        } else {
            Serial.println("[BLE] Connection attempt failed, will retry scan in 3 seconds...");
            delay(3000);
            startBLEScan();
        }
    } else if (!bleConnected) {
        if (millis() - lastScanAttempt > 8000) {
            lastScanAttempt = millis();
            startBLEScan();
        }
    }

    // 6. Periodic status print
    if (millis() - lastStatusPrint > 10000) {
        lastStatusPrint = millis();
        Serial.printf("[STATUS] WiFi: %s (%s) | BLE: %s (%s, RSSI: %d) | TCP: %s | WS Clients: %u | Free Heap: %u\n",
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DISCONNECTED",
                      WIFI_SSID,
                      bleConnected ? "CONNECTED" : (bleConnecting ? "CONNECTING" : "DISCONNECTED"),
                      bleConnectedDeviceName.c_str(),
                      bleLastRSSI,
                      tcpClient && tcpClient.connected() ? "CONNECTED" : "WAITING",
                      wsServer.connectedClients(),
                      ESP.getFreeHeap());
    }

    yield();
}
