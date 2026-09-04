#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#if defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_hs.h>
#include <host/ble_store.h>
#endif
#include <Preferences.h>
#include <vector>
#include "dashboard_html.h"

// ================= Configuration =================
#define BRIDGE_VERSION            "1.5.0"
#define DEFAULT_BLE_PIN           808978
#define MESHCORE_SERVICE_UUID     "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define MESHCORE_RX_CHAR_UUID     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define MESHCORE_TX_CHAR_UUID     "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

#define TCP_PORT                  5000
#define WS_PORT                   5001
#define HTTP_PORT                 80

#define MAX_FRAME_SIZE            172
#define USB_SERIAL_TX_FRAME_START 0x3c // '<'
#define USB_SERIAL_RX_FRAME_START 0x3e // '>'

// Discovered BLE device structure
struct DiscoveredDevice {
    String address;
    String name;
    int rssi;
    bool isMeshCandidate;
    bool hasServiceUUID;
    unsigned long lastSeen;
};

// ================= State Variables =================
static std::vector<DiscoveredDevice> discoveredDevices;
static Preferences preferences;

// Wi-Fi & Soft AP Portal State
static String wifiSSID = "";
static String wifiPass = "";
static bool isAPMode = false;
static String apSSID = "MeshCore-Bridge-Setup";
static DNSServer dnsServer;
static const byte DNS_PORT = 53;
static bool wifiConnectPending = false;
static bool wifiScanning = false;

static String configuredTargetMac = "";
static uint32_t configuredPinCode = DEFAULT_BLE_PIN;
static bool autoConnectEnabled = true;
static bool bleScanning = false;
static unsigned long scanStartTime = 0;

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

// Sanitize string for valid JSON output
static String sanitizeJSONString(const String& input) {
    String out = "";
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if ((uint8_t)c >= 32 && (uint8_t)c <= 126) {
            out += c;
        }
    }
    return out;
}

// Forward declarations
void startBLEScan(int durationSec = 5, bool clearList = false);
bool connectToMeshCoreDevice();
void sendFrameToMeshCore(const uint8_t* data, size_t len);
void broadcastFrameToClients(const uint8_t* data, size_t len);
String getStatusJSON();
String getDevicesJSON();
String getBondsJSON();
String getWiFiScanJSON();
void handleGetBonds();
void handleClearBonds();
void handleDeleteBond();
void handleClearDiscoveredCache();
void handleGetWiFiScan();
void handleSaveWiFi();
void handleForgetWiFi();
void handleCaptivePortal();
void savePreferences();
void loadPreferences();
void forgetPreferences();
void saveWiFiPreferences(const String& ssid, const String& pass);
void forgetWiFiPreferences();
bool connectWiFi(const String& ssid, const String& pass, int timeoutSec = 12);
void startAPMode();
void stopAPMode();

static bool bleAuthCompleted = false;

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
        bleAuthCompleted = true;
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
static void onScanComplete(BLEScanResults scanResults) {
    bleScanning = false;
    Serial.printf("[SCAN] Scan complete. Total discovered devices: %d\n", (int)discoveredDevices.size());
}

class BridgeAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        String addr = advertisedDevice.getAddress().toString().c_str();
        String rawName = advertisedDevice.getName().c_str();
        int rssi = advertisedDevice.getRSSI();

        // Sanitize name to ASCII printable characters
        String name = "";
        for (size_t i = 0; i < rawName.length(); i++) {
            char c = rawName[i];
            if ((uint8_t)c >= 32 && (uint8_t)c <= 126) {
                name += c;
            }
        }
        name.trim();

        bool hasUUID = advertisedDevice.isAdvertisingService(BLEUUID(MESHCORE_SERVICE_UUID));
        bool matchName = name.startsWith("MeshCore") ||
                         name.startsWith("Whisper")  ||
                         name.startsWith("WisCore")  ||
                         name.startsWith("HT-")      ||
                         name.startsWith("Seeed")    ||
                         name.startsWith("Lilygo")   ||
                         name.startsWith("LowMesh");
        bool isCandidate = hasUUID || matchName;

        bool found = false;
        for (auto& dev : discoveredDevices) {
            if (dev.address.equalsIgnoreCase(addr)) {
                found = true;
                if (name.length() > 0 && dev.name.length() == 0) {
                    dev.name = name;
                }
                dev.rssi = rssi;
                dev.isMeshCandidate = dev.isMeshCandidate || isCandidate;
                dev.hasServiceUUID = dev.hasServiceUUID || hasUUID;
                dev.lastSeen = millis();
                break;
            }
        }
        if (!found && discoveredDevices.size() < 60) {
            DiscoveredDevice d;
            d.address = addr;
            d.name = name;
            d.rssi = rssi;
            d.isMeshCandidate = isCandidate;
            d.hasServiceUUID = hasUUID;
            d.lastSeen = millis();
            discoveredDevices.push_back(d);
        }

        // Auto-connect to configured target MAC if set and enabled
        if (!bleConnected && !bleConnecting && autoConnectEnabled && configuredTargetMac.length() > 0) {
            if (addr.equalsIgnoreCase(configuredTargetMac)) {
                Serial.printf("[SCAN] Found configured target '%s' (%s, RSSI: %d dBm)\n",
                              name.c_str(), addr.c_str(), rssi);
                BLEDevice::getScan()->stop();
                bleScanning = false;
                if (pTargetAddress) delete pTargetAddress;
                pTargetAddress = new BLEAddress(advertisedDevice.getAddress());
                bleConnectedDeviceName = name.length() > 0 ? name : "MeshCore Device";
                bleConnectedAddress = addr;
                bleLastRSSI = rssi;
                bleConnecting = true;
            }
        }
    }
};

void startBLEScan(int durationSec, bool clearList) {
    BLEScan* pScan = BLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
        delay(50);
    }
    if (clearList) {
        discoveredDevices.clear();
    }
    Serial.printf("[BLE] Starting background scan for %d seconds...\n", durationSec);
    pScan->clearResults();
    bleScanning = true;
    scanStartTime = millis();
    pScan->start(durationSec, onScanComplete, false);
}

// ================= Preferences (NVS) Storage =================
void loadPreferences() {
    preferences.begin("mesh_bridge", false);
    wifiSSID = preferences.getString("wifi_ssid", "");
    wifiPass = preferences.getString("wifi_pass", "");
    configuredTargetMac = preferences.getString("target_mac", "");
    configuredPinCode = preferences.getUInt("pin_code", DEFAULT_BLE_PIN);
    autoConnectEnabled = preferences.getBool("auto_conn", true);
    preferences.end();

    blePinCode = configuredPinCode;
    Serial.printf("[PREFS] Loaded WiFi: '%s' | Target MAC: '%s' | PIN: %u | AutoConnect: %d\n",
                  wifiSSID.c_str(), configuredTargetMac.c_str(), configuredPinCode, autoConnectEnabled);
}

void savePreferences() {
    preferences.begin("mesh_bridge", false);
    preferences.putString("target_mac", configuredTargetMac);
    preferences.putUInt("pin_code", configuredPinCode);
    preferences.putBool("auto_conn", autoConnectEnabled);
    preferences.end();
    Serial.printf("[PREFS] Saved Target MAC: '%s' | PIN: %u | AutoConnect: %d\n",
                  configuredTargetMac.c_str(), configuredPinCode, autoConnectEnabled);
}

void saveWiFiPreferences(const String& ssid, const String& pass) {
    preferences.begin("mesh_bridge", false);
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", pass);
    preferences.end();
    wifiSSID = ssid;
    wifiPass = pass;
    Serial.printf("[PREFS] Saved WiFi SSID: '%s'\n", wifiSSID.c_str());
}

void forgetWiFiPreferences() {
    preferences.begin("mesh_bridge", false);
    preferences.remove("wifi_ssid");
    preferences.remove("wifi_pass");
    preferences.end();
    wifiSSID = "";
    wifiPass = "";
    Serial.println("[PREFS] WiFi credentials forgotten.");
}

void forgetPreferences() {
    preferences.begin("mesh_bridge", false);
    preferences.remove("target_mac");
    preferences.end();
    configuredTargetMac = "";
    Serial.println("[PREFS] Target device forgotten.");
}

// ================= Wi-Fi & Soft AP Management =================
bool connectWiFi(const String& ssid, const String& pass, int timeoutSec) {
    if (ssid.length() == 0) return false;
    Serial.printf("[WiFi] Connecting to '%s'...\n", ssid.c_str());
    WiFi.disconnect();
    delay(100);
    WiFi.mode(isAPMode ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : nullptr);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start < (unsigned long)timeoutSec * 1000)) {
        delay(250);
        Serial.print(".");
        if (isAPMode) {
            dnsServer.processNextRequest();
            httpServer.handleClient();
        }
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected! IP: %s | Gateway: %s | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str(),
                      WiFi.RSSI());
        return true;
    }
    Serial.printf("[WiFi] Connection to '%s' timed out.\n", ssid.c_str());
    return false;
}

void startAPMode() {
    if (isAPMode) return;
    isAPMode = true;
    WiFi.mode(WIFI_AP_STA);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "MeshCore-Bridge-%02X%02X", mac[4], mac[5]);
    apSSID = String(apName);

    WiFi.softAP(apSSID.c_str());
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[WiFi AP] Soft AP started: '%s' | Portal IP: %s\n", apSSID.c_str(), apIP.toString().c_str());

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("[DNS] Captive Portal DNS active (* -> 192.168.4.1)");
}

void stopAPMode() {
    if (!isAPMode) return;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    isAPMode = false;
    Serial.println("[WiFi AP] Soft AP stopped. Running in Station mode.");
}

bool connectToMeshCoreDevice() {
    if (!pTargetAddress) return false;

    Serial.printf("[BLE] Connecting to %s (%s) with PIN %u...\n",
                  bleConnectedDeviceName.c_str(), pTargetAddress->toString().c_str(), blePinCode);

#if defined(CONFIG_NIMBLE_ENABLED)
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY;
    ble_hs_cfg.sm_their_key_dist = BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY;
#endif
    BLESecurity::setPassKey(true, blePinCode);

    if (!pClient) {
        pClient = BLEDevice::createClient();
        pClient->setClientCallbacks(new BridgeClientCallbacks());
    } else if (pClient->isConnected()) {
        pClient->disconnect();
        delay(100);
    }

    bleAuthCompleted = false;

    if (!pClient->connect(*pTargetAddress)) {
        Serial.println("[BLE] Connection failed.");
        bleConnecting = false;
        return false;
    }

    Serial.println("[BLE] Initiating MITM pairing security procedure...");
#if defined(CONFIG_NIMBLE_ENABLED)
    ble_gap_security_initiate(pClient->getConnId());
#endif

    // Wait for authentication & bonding to complete
    unsigned long waitStart = millis();
    while (!bleAuthCompleted && (millis() - waitStart < 5000)) {
        httpServer.handleClient();
        if (!pClient->isConnected()) {
            Serial.println("[BLE] Disconnected during authentication.");
            bleConnecting = false;
            return false;
        }
        delay(50);
    }
    delay(300);

    Serial.println("[BLE] Discovering NUS service & characteristics...");
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
        Serial.println("[BLE] Subscribing to TX notifications...");
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
    pRxCharacteristic->writeValue((uint8_t*)data, len, false);
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
            } else if (req.indexOf("GET /api/devices") >= 0) {
                String json = getDevicesJSON();
                String resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + String(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                tcpClient.print(resp);
            } else if (req.indexOf("GET /api/bonds") >= 0) {
                String json = getBondsJSON();
                String resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + String(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                tcpClient.print(resp);
            } else {
                size_t htmlLen = sizeof(DASHBOARD_HTML) - 1;
                String resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + String(htmlLen) + "\r\nConnection: close\r\n\r\n";
                tcpClient.print(resp);
                tcpClient.write(reinterpret_cast<const uint8_t *>(DASHBOARD_HTML), htmlLen);
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

// ================= JSON Responses =================
String getStatusJSON() {
    String json = "{";
    json += "\"version\":\"" + String(BRIDGE_VERSION) + "\",";
    json += "\"ble_connected\":" + String(bleConnected ? "true" : "false") + ",";
    json += "\"ble_connecting\":" + String(bleConnecting ? "true" : "false") + ",";
    json += "\"ble_device_name\":\"" + sanitizeJSONString(bleConnectedDeviceName) + "\",";
    json += "\"ble_mac\":\"" + sanitizeJSONString(bleConnectedAddress) + "\",";
    json += "\"ble_rssi\":" + String(bleLastRSSI) + ",";
    json += "\"ble_pin\":" + String(blePinCode) + ",";
    json += "\"ble_rx\":" + String(blePacketsRx) + ",";
    json += "\"ble_tx\":" + String(blePacketsTx) + ",";
    json += "\"target_mac\":\"" + sanitizeJSONString(configuredTargetMac) + "\",";
    json += "\"auto_conn\":" + String(autoConnectEnabled ? "true" : "false") + ",";
    json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"is_ap_mode\":" + String(isAPMode ? "true" : "false") + ",";
    json += "\"ap_ssid\":\"" + sanitizeJSONString(apSSID) + "\",";
    json += "\"ap_ip\":\"" + (isAPMode ? WiFi.softAPIP().toString() : "") + "\",";
    json += "\"wifi_ssid\":\"" + sanitizeJSONString(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiSSID) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
    json += "\"ip_address\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : (isAPMode ? WiFi.softAPIP().toString() : "")) + "\",";
    json += "\"tcp_port\":" + String(TCP_PORT) + ",";
    json += "\"ws_port\":" + String(WS_PORT) + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime_sec\":" + String(millis() / 1000);
    json += "}";
    return json;
}

String getWiFiScanJSON() {
    int n = WiFi.scanNetworks(false, false);
    String json = "{\"count\":" + String(n >= 0 ? n : 0) + ",\"networks\":[";
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"ssid\":\"" + sanitizeJSONString(WiFi.SSID(i)) + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
            json += "}";
        }
        WiFi.scanDelete();
    }
    json += "]}";
    return json;
}

String getDevicesJSON() {
    String json = "{\"scanning\":" + String(bleScanning ? "true" : "false") + ",";
    json += "\"target_mac\":\"" + sanitizeJSONString(configuredTargetMac) + "\",";
    json += "\"pin\":" + String(blePinCode) + ",";
    json += "\"auto_conn\":" + String(autoConnectEnabled ? "true" : "false") + ",";
    json += "\"connected\":" + String(bleConnected ? "true" : "false") + ",";
    json += "\"connected_mac\":\"" + sanitizeJSONString(bleConnected ? bleConnectedAddress : "") + "\",";
    json += "\"connected_name\":\"" + sanitizeJSONString(bleConnected ? bleConnectedDeviceName : "") + "\",";
    json += "\"devices\":[";
    for (size_t i = 0; i < discoveredDevices.size(); i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"address\":\"" + sanitizeJSONString(discoveredDevices[i].address) + "\",";
        json += "\"name\":\"" + sanitizeJSONString(discoveredDevices[i].name) + "\",";
        json += "\"rssi\":" + String(discoveredDevices[i].rssi) + ",";
        json += "\"is_mesh\":" + String(discoveredDevices[i].isMeshCandidate ? "true" : "false") + ",";
        json += "\"connected\":" + String(bleConnected && bleConnectedAddress.equalsIgnoreCase(discoveredDevices[i].address) ? "true" : "false");
        json += "}";
    }
    json += "]}";
    return json;
}

struct StoredBond {
    String address;
    bool authenticated;
    bool sc;
    bool ltk_present;
    bool irk_present;
    uint8_t key_size;
};

static std::vector<StoredBond> getBondedPeersList() {
    std::vector<StoredBond> list;
#if defined(CONFIG_NIMBLE_ENABLED)
    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, [](int obj_type, union ble_store_value *val, void *cookie) -> int {
        if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC && val && cookie) {
            auto* pList = (std::vector<StoredBond>*)cookie;
            StoredBond b;
            char addrStr[18];
            snprintf(addrStr, sizeof(addrStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                     val->sec.peer_addr.val[5], val->sec.peer_addr.val[4],
                     val->sec.peer_addr.val[3], val->sec.peer_addr.val[2],
                     val->sec.peer_addr.val[1], val->sec.peer_addr.val[0]);
            b.address = String(addrStr);
            b.authenticated = val->sec.authenticated;
            b.sc = val->sec.sc;
            b.ltk_present = val->sec.ltk_present;
            b.irk_present = val->sec.irk_present;
            b.key_size = val->sec.key_size;
            pList->push_back(b);
        }
        return 0;
    }, &list);
#endif
    return list;
}

String getBondsJSON() {
    std::vector<StoredBond> bonds = getBondedPeersList();
    String json = "{\"bonds_count\":" + String(bonds.size()) + ",\"bonds\":[";
    for (size_t i = 0; i < bonds.size(); i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"address\":\"" + sanitizeJSONString(bonds[i].address) + "\",";
        json += "\"authenticated\":" + String(bonds[i].authenticated ? "true" : "false") + ",";
        json += "\"sc\":" + String(bonds[i].sc ? "true" : "false") + ",";
        json += "\"ltk_present\":" + String(bonds[i].ltk_present ? "true" : "false") + ",";
        json += "\"irk_present\":" + String(bonds[i].irk_present ? "true" : "false") + ",";
        json += "\"key_size\":" + String(bonds[i].key_size);
        json += "}";
    }
    json += "]}";
    return json;
}

// ================= HTTP Web Server Handlers =================
void handleRoot() {
    httpServer.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleStatusJSON() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.send(200, "application/json", getStatusJSON());
}

void handleGetDevices() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.send(200, "application/json", getDevicesJSON());
}

void handleGetBonds() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.send(200, "application/json", getBondsJSON());
}

void handleClearBonds() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    if (bleConnected && pClient) {
        pClient->disconnect();
        bleConnected = false;
        bleConnecting = false;
    }
#if defined(CONFIG_NIMBLE_ENABLED)
    ble_store_clear();
#endif
    Serial.println("[BLE SEC] Cleared all stored security bonds from keystore.");
    httpServer.send(200, "application/json", "{\"status\":\"All security bonds cleared\"}");
}

void handleDeleteBond() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    String mac = httpServer.arg("mac");
    if (mac.length() == 0) {
        httpServer.send(400, "application/json", "{\"error\":\"Missing mac parameter\"}");
        return;
    }
#if defined(CONFIG_NIMBLE_ENABLED)
    BLEAddress bleAddr(mac.c_str());
    uint8_t *native = bleAddr.getNative();
    ble_addr_t addr;
    for (int i = 0; i < 6; i++) {
        addr.val[i] = native[5 - i];
    }
    addr.type = BLE_ADDR_PUBLIC;
    ble_store_util_delete_peer(&addr);
    addr.type = BLE_ADDR_RANDOM;
    ble_store_util_delete_peer(&addr);
#endif
    Serial.printf("[BLE SEC] Deleted stored bond for %s\n", mac.c_str());
    httpServer.send(200, "application/json", "{\"status\":\"Bond deleted\",\"address\":\"" + sanitizeJSONString(mac) + "\"}");
}

void handleClearDiscoveredCache() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    discoveredDevices.clear();
    BLEDevice::getScan()->clearResults();
    Serial.println("[SCAN] Cleared discovered devices cache.");
    httpServer.send(200, "application/json", "{\"status\":\"Discovered devices cache cleared\"}");
}

void handleStartScan() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    startBLEScan(5, false);
    httpServer.send(200, "application/json", "{\"status\":\"Scan started\"}");
}

void handleConnect() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    String address = "";
    uint32_t pin = blePinCode;
    bool save = true;
    bool autoConn = true;

    if (httpServer.hasArg("address")) address = httpServer.arg("address");
    if (httpServer.hasArg("pin")) pin = (uint32_t)httpServer.arg("pin").toInt();
    if (httpServer.hasArg("save")) save = (httpServer.arg("save") == "true" || httpServer.arg("save") == "1");
    if (httpServer.hasArg("auto")) autoConn = (httpServer.arg("auto") == "true" || httpServer.arg("auto") == "1");

    address.trim();
    if (address.length() == 0) {
        httpServer.send(400, "application/json", "{\"error\":\"Missing address parameter\"}");
        return;
    }

    blePinCode = pin > 0 ? pin : DEFAULT_BLE_PIN;
    BLESecurity sec;
    sec.setPassKey(true, blePinCode);

    if (save) {
        configuredTargetMac = address;
        configuredPinCode = blePinCode;
        autoConnectEnabled = autoConn;
        savePreferences();
    }

    if (bleConnected && pClient) {
        pClient->disconnect();
        bleConnected = false;
    }

    if (pTargetAddress) delete pTargetAddress;
    pTargetAddress = new BLEAddress(address.c_str());
    bleConnectedDeviceName = address;
    for (const auto& dev : discoveredDevices) {
        if (dev.address.equalsIgnoreCase(address) && dev.name.length() > 0) {
            bleConnectedDeviceName = dev.name;
            break;
        }
    }
    bleConnectedAddress = address;
    bleConnecting = true;

    httpServer.send(200, "application/json", "{\"status\":\"Connecting\",\"address\":\"" + address + "\",\"pin\":" + String(blePinCode) + "}");
}

void handleDisconnect() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    if (bleConnected && pClient) {
        pClient->disconnect();
        bleConnected = false;
        bleConnecting = false;
        pRxCharacteristic = nullptr;
        pTxCharacteristic = nullptr;
    }
    httpServer.send(200, "application/json", "{\"status\":\"Disconnected\"}");
}

void handleForget() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    forgetPreferences();
    httpServer.send(200, "application/json", "{\"status\":\"Target forgotten\"}");
}

void handleGetWiFiScan() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        httpServer.send(200, "application/json", "{\"scanning\":true,\"count\":0,\"networks\":[]}");
        return;
    }
    if (n >= 0) {
        String json = "{\"scanning\":false,\"count\":" + String(n) + ",\"networks\":[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"ssid\":\"" + sanitizeJSONString(WiFi.SSID(i)) + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
            json += "}";
        }
        json += "]}";
        WiFi.scanDelete();
        wifiScanning = false;
        httpServer.send(200, "application/json", json);
        return;
    }
    // Scan not active yet or failed, start asynchronous scan
    wifiScanning = true;
    WiFi.scanNetworks(true);
    httpServer.send(200, "application/json", "{\"scanning\":true,\"count\":0,\"networks\":[]}");
}

void handleSaveWiFi() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    String ssid = "";
    String pass = "";
    if (httpServer.hasArg("ssid")) ssid = httpServer.arg("ssid");
    if (httpServer.hasArg("password")) pass = httpServer.arg("password");
    if (httpServer.hasArg("pass")) pass = httpServer.arg("pass");
    ssid.trim();

    if (ssid.length() == 0) {
        httpServer.send(400, "application/json", "{\"error\":\"SSID cannot be empty\"}");
        return;
    }

    saveWiFiPreferences(ssid, pass);
    wifiConnectPending = true;
    httpServer.send(200, "application/json", "{\"status\":\"Connecting\",\"ssid\":\"" + sanitizeJSONString(ssid) + "\"}");
}

void handleForgetWiFi() {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    forgetWiFiPreferences();
    WiFi.disconnect(true);
    if (!isAPMode) {
        startAPMode();
    }
    httpServer.send(200, "application/json", "{\"status\":\"WiFi forgotten, Soft AP active\",\"ap_ssid\":\"" + sanitizeJSONString(apSSID) + "\"}");
}

void handleCaptivePortal() {
    if (isAPMode && WiFi.status() != WL_CONNECTED) {
        httpServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        httpServer.send(302, "text/plain", "");
    } else {
        httpServer.send_P(200, "text/html", DASHBOARD_HTML);
    }
}

// ================= Setup & Loop =================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n========================================");
    Serial.printf("  ESP32-C3 MeshCore BLE Bridge v%s\n", BRIDGE_VERSION);
    Serial.println("========================================");

    // 1. Load preferences from NVS
    loadPreferences();

    // 2. Connect to saved WiFi Network, or start Soft AP Captive Portal if unable to connect
    bool wifiOk = false;
    if (wifiSSID.length() > 0) {
        wifiOk = connectWiFi(wifiSSID, wifiPass, 12);
    }

    if (!wifiOk) {
        Serial.println("[WiFi] No valid saved Wi-Fi connection. Launching Soft AP setup portal...");
        startAPMode();
    }

    // 3. Start mDNS
    if (MDNS.begin("meshcore-ble-bridge")) {
        Serial.println("[mDNS] Responder started: http://meshcore-ble-bridge.local");
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("meshcore", "tcp", TCP_PORT);
        MDNS.addService("meshcore-ws", "tcp", WS_PORT);
    }

    // 4. Start TCP Server on port 5000
    tcpServer.begin();
    tcpServer.setNoDelay(true);
    Serial.printf("[TCP] Server listening on port %d\n", TCP_PORT);

    // 5. Start WebSocket Server on port 5001
    wsServer.begin();
    wsServer.onEvent(onWebSocketEvent);
    Serial.printf("[WS] Server listening on port %d\n", WS_PORT);

    // 6. Start HTTP Web Server on port 80
    httpServer.on("/", handleRoot);
    httpServer.on("/config", handleRoot);
    httpServer.on("/status", handleStatusJSON);
    httpServer.on("/api/devices", handleGetDevices);
    httpServer.on("/api/bonds", handleGetBonds);
    httpServer.on("/api/bonds/clear", handleClearBonds);
    httpServer.on("/api/bonds/delete", handleDeleteBond);
    httpServer.on("/api/cache/clear-devices", handleClearDiscoveredCache);
    httpServer.on("/api/wifi/scan", handleGetWiFiScan);
    httpServer.on("/api/wifi/save", handleSaveWiFi);
    httpServer.on("/api/wifi/forget", handleForgetWiFi);
    httpServer.on("/api/scan", handleStartScan);
    httpServer.on("/api/connect", handleConnect);
    httpServer.on("/api/disconnect", handleDisconnect);
    httpServer.on("/api/forget", handleForget);

    // Captive portal probe redirects
    httpServer.on("/hotspot-detect.html", handleCaptivePortal);
    httpServer.on("/canonical.html", handleCaptivePortal);
    httpServer.on("/generate_204", handleCaptivePortal);
    httpServer.on("/gen_204", handleCaptivePortal);
    httpServer.on("/ncsi.txt", handleCaptivePortal);
    httpServer.on("/connecttest.txt", handleCaptivePortal);
    httpServer.on("/redirect", handleCaptivePortal);
    httpServer.on("/success.txt", handleCaptivePortal);
    httpServer.onNotFound(handleCaptivePortal);

    httpServer.enableCORS(true);
    httpServer.begin();
    Serial.printf("[HTTP] Web dashboard & config ready on port %d\n", HTTP_PORT);

    // 7. Initialize BLE Device and Security
    BLEDevice::init("ESP32C3-MeshBridge");
    BLEDevice::setSecurityCallbacks(new BridgeSecurityCallbacks());

    BLESecurity sec;
    sec.setForceAuthentication(false);
    sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    sec.setCapability(ESP_IO_CAP_KBDISP);
    sec.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec.setPassKey(true, blePinCode);

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new BridgeAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println("[BLE] Starting initial discovery scan...");
    startBLEScan(5, true);
}

static unsigned long lastScanAttempt = 0;
static unsigned long lastStatusPrint = 0;
static unsigned long lastWiFiCheck = 0;

void loop() {
    // 0. Handle Captive Portal DNS requests
    if (isAPMode) {
        dnsServer.processNextRequest();
    }

    // 1. Handle HTTP web requests (port 80)
    httpServer.handleClient();

    // 1b. Handle pending Wi-Fi connection from Web UI
    if (wifiConnectPending) {
        wifiConnectPending = false;
        Serial.printf("[WiFi] Initiating connection to '%s'...\n", wifiSSID.c_str());
        bool ok = connectWiFi(wifiSSID, wifiPass, 12);
        if (ok && isAPMode) {
            stopAPMode();
        }
    }

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

    // 4. Handle WiFi reconnection in Station mode
    if (!isAPMode && wifiSSID.length() > 0 && millis() - lastWiFiCheck > 15000) {
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
            Serial.println("[BLE] Connection attempt failed, will retry scan in 5 seconds...");
            delay(1000);
            bleConnecting = false;
        }
    } else if (!bleConnected) {
        if (autoConnectEnabled && configuredTargetMac.length() > 0) {
            if (millis() - lastScanAttempt > 10000 && !bleScanning) {
                lastScanAttempt = millis();
                startBLEScan(5, false);
            }
        }
    }

    // Scan timeout watchdog
    if (bleScanning && millis() - scanStartTime > 8000) {
        Serial.println("[SCAN] Watchdog: scan timed out, resetting bleScanning");
        BLEDevice::getScan()->stop();
        bleScanning = false;
    }

    // 6. Periodic status print
    if (millis() - lastStatusPrint > 10000) {
        lastStatusPrint = millis();
        String netStr = WiFi.status() == WL_CONNECTED ? ("STA: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")") : (isAPMode ? ("AP: " + apSSID + " (192.168.4.1)") : "DISCONNECTED");
        Serial.printf("[STATUS] %s | BLE: %s (%s, RSSI: %d) | Target: %s | TCP: %s | WS Clients: %u | Free Heap: %u\n",
                      netStr.c_str(),
                      bleConnected ? "CONNECTED" : (bleConnecting ? "CONNECTING" : "DISCONNECTED"),
                      bleConnectedDeviceName.c_str(),
                      bleLastRSSI,
                      configuredTargetMac.length() > 0 ? configuredTargetMac.c_str() : "NONE",
                      tcpClient && tcpClient.connected() ? "CONNECTED" : "WAITING",
                      wsServer.connectedClients(),
                      ESP.getFreeHeap());
    }

    yield();
}
