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
static unsigned long lastScanAttempt = 0;
static unsigned long scanBackoffInterval = 10000;
const unsigned long MIN_SCAN_BACKOFF = 10000;
const unsigned long MAX_SCAN_BACKOFF = 60000;
const unsigned long SCAN_BACKOFF_STEP = 10000;

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
String getDashboardHTML();
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
        scanBackoffInterval = MIN_SCAN_BACKOFF;
        if (bleConnected && bleScanning) {
            BLEDevice::getScan()->stop();
            bleScanning = false;
        }
    }
    void onDisconnect(BLEClient* pclient) override {
        Serial.println("[BLE] Disconnected from MeshCore device!");
        bleConnected = false;
        bleConnecting = false;
        pRxCharacteristic = nullptr;
        pTxCharacteristic = nullptr;
        scanBackoffInterval = MIN_SCAN_BACKOFF;
        lastScanAttempt = millis();
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
                scanBackoffInterval = MIN_SCAN_BACKOFF;
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
    scanBackoffInterval = MIN_SCAN_BACKOFF;
    if (bleConnected && bleScanning) {
        BLEDevice::getScan()->stop();
        bleScanning = false;
    }
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
    httpServer.send(200, "text/html", getDashboardHTML());
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
        httpServer.send(200, "text/html", getDashboardHTML());
    }
}

// ================= HTTP Content Generators =================
String getDashboardHTML() {
    String localIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Connecting...";
    String html = "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ESP32 MeshCore BLE Bridge</title>";
    html += "<style>";
    html += ":root { --bg: #0d1117; --panel: #161b22; --border: #30363d; --text: #c9d1d9; --accent: #58a6ff; --green: #238636; --green-txt: #3fb950; --red: #da3633; --yellow: #d29922; }";
    html += "* { box-sizing: border-box; }";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 16px; }";
    html += ".container { max-width: 920px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }";
    html += ".header { display: flex; justify-content: space-between; align-items: center; background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 12px 16px; flex-wrap: wrap; gap: 10px; }";
    html += ".title { font-size: 19px; font-weight: bold; color: var(--accent); display: flex; align-items: center; gap: 8px; }";
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
    html += ".btn-red { background: rgba(218,54,51,0.2); border-color: var(--red); color: #f85149; }";
    html += ".btn-red:hover { background: var(--red); color: #fff; }";
    html += ".btn-group { display: flex; flex-wrap: wrap; gap: 8px; }";
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
    html += ".device-card { background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 10px 14px; cursor: pointer; transition: all 0.2s; }";
    html += ".device-card:hover { border-color: var(--accent); background: #161b22; }";
    html += ".device-connected { border-color: var(--green) !important; background: rgba(35,134,54,0.08) !important; }";
    html += "</style></head><body>";
    html += "<div class='container'>";

    // Header with Tab Switcher
    html += "<div class='header'>";
    html += "<div style='display:flex; align-items:center; gap:16px; flex-wrap:wrap;'>";
    html += "<div class='title'>⚡ MeshCore BLE Bridge</div>";
    html += "<div class='btn-group'>";
    html += "<button id='btnTabConsole' class='btn btn-primary' onclick='switchTab(\"console\")'>📡 Mesh Console</button>";
    html += "<button id='btnTabConfig' class='btn' onclick='switchTab(\"config\")'>⚙️ Bluetooth & Config</button>";
    html += "</div></div>";
    html += "<div style='display:flex; gap:8px; flex-wrap:wrap;'>";
    html += "<span id='wifiHeaderBadge' class='badge bg-yellow' style='cursor:pointer;' onclick='switchTab(\"config\")'>WiFi: Checking...</span>";
    html += "<span id='bleBadge' class='badge bg-yellow' style='cursor:pointer;' onclick='switchTab(\"config\")'>BLE: Connecting...</span>";
    html += "<span id='wsBadge' class='badge bg-yellow'>WS: Connecting...</span>";
    html += "</div></div>";

    // Tab 1: Mesh Console & Radio Dashboard
    html += "<div id='tabConsole' style='display:flex; flex-direction:column; gap:16px;'>";
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
    html += "<div class='row'><span class='label'>Network Mode</span><span class='val' id='valNetMode'>-</span></div>";
    html += "<div class='row'><span class='label'>WiFi / AP SSID</span><span class='val' id='valWiFiSSID'>-</span></div>";
    html += "<div class='row'><span class='label'>Bridge IP</span><span class='val' id='valBridgeIP'>" + localIP + "</span></div>";
    html += "<div class='row'><span class='label'>Native TCP Port</span><span class='val' id='valTcpPort'>" + localIP + ":5000</span></div>";
    html += "<div class='row'><span class='label'>WebSocket Port</span><span class='val' id='valWsPort'>ws://" + localIP + ":5001</span></div>";
    html += "<div class='row'><span class='label'>BLE Device</span><span class='val' id='valBleDev'>" + (bleConnectedDeviceName.length() > 0 ? bleConnectedDeviceName : "-") + " (" + String(bleLastRSSI) + " dBm)</span></div>";
    html += "<div class='row'><span class='label'>BLE Packets (RX / TX)</span><span class='val' id='valBlePackets'>" + String(blePacketsRx) + " / " + String(blePacketsTx) + "</span></div>";
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
    html += "</div></div>";

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
    html += "</div>"; // End tabConsole

    // Tab 2: Bluetooth Device Scanner & Configuration
    html += "<div id='tabConfig' style='display:none; flex-direction:column; gap:16px;'>";

    // Card: Wi-Fi Setup & Captive Portal
    html += "<div class='card' id='wifiSetupCard' style='border: 1px solid #1f6feb;'>";
    html += "<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>";
    html += "<div class='card-title' style='margin-bottom:0; color:#58a6ff;'>📶 Wi-Fi Network Configuration & Soft AP</div>";
    html += "<span id='wifiModeBadge' class='badge bg-yellow'>Detecting...</span>";
    html += "</div>";
    html += "<div id='wifiStatusDesc' style='font-size:13px; color:#c9d1d9; margin-bottom:12px;'>";
    html += "Select your local Wi-Fi network below or enter credentials manually to connect the bridge.";
    html += "</div>";
    html += "<div class='grid' style='grid-template-columns: 1fr 1fr; gap:12px;'>";
    html += "<div>";
    html += "<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:4px;'>";
    html += "<label class='label'>Select Wi-Fi Network</label>";
    html += "<button class='btn' style='padding:2px 8px; font-size:11px;' onclick='scanWiFi()'>🔄 Scan Wi-Fi</button>";
    html += "</div>";
    html += "<select id='wifiSelect' style='margin-bottom:6px;' onchange='onSelectWiFiNetwork()'>";
    html += "<option value=''>-- Click 'Scan Wi-Fi' to discover networks --</option>";
    html += "</select>";
    html += "<input type='text' id='wifiManualSsid' placeholder='Or enter SSID manually...' />";
    html += "</div>";
    html += "<div>";
    html += "<label class='label' style='display:block; margin-bottom:4px;'>Wi-Fi Password</label>";
    html += "<div style='display:flex; gap:6px;'>";
    html += "<input type='password' id='wifiPass' placeholder='Enter Wi-Fi password (blank if open)' />";
    html += "<button class='btn' type='button' style='padding:4px 10px; font-size:12px;' onclick='togglePassVisibility()'>👁️</button>";
    html += "</div>";
    html += "</div></div>";
    html += "<div style='margin-top:12px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;'>";
    html += "<div id='wifiFeedback' style='font-size:13px; font-family:monospace; min-height:20px;'></div>";
    html += "<div class='btn-group'>";
    html += "<button class='btn btn-primary' id='btnSaveWifi' onclick='saveWiFi()'>💾 Save & Connect</button>";
    html += "<button class='btn btn-red' id='btnForgetWifi' onclick='forgetWiFi()'>🗑️ Forget Wi-Fi & Start Soft AP</button>";
    html += "</div></div>";
    html += "</div>"; // End Card Wi-Fi

    // Card: Active Connection Status
    html += "<div class='card'>";
    html += "<div class='card-title'>Active Bluetooth Connection</div>";
    html += "<div id='activeConnBox' style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;'>";
    html += "<div>";
    html += "<div style='font-size:16px; font-weight:bold; color:#f0f6fc;' id='activeDevName'>No device connected</div>";
    html += "<div style='font-family:monospace; font-size:13px; color:#8b949e;' id='activeDevAddr'>-</div>";
    html += "</div>";
    html += "<div style='display:flex; gap:8px; align-items:center;'>";
    html += "<span id='activeDevRSSI' class='badge bg-yellow'>Disconnected</span>";
    html += "<button id='btnDisconnect' class='btn btn-red' style='display:none;' onclick='disconnectTarget()'>🔌 Disconnect</button>";
    html += "</div></div></div>";

    // Card: Device Scanner
    html += "<div class='card'>";
    html += "<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>";
    html += "<div class='card-title' style='margin-bottom:0;'>Discovered Bluetooth Devices</div>";
    html += "<div style='display:flex; align-items:center; gap:12px; flex-wrap:wrap;'>";
    html += "<label style='font-size:13px; color:#c9d1d9; display:flex; align-items:center; gap:6px; cursor:pointer;'>";
    html += "<input type='checkbox' id='showAllCheck' style='width:auto; cursor:pointer;' onchange='renderDevices()' />";
    html += "Show all Bluetooth devices (unfiltered)";
    html += "</label>";
    html += "<button id='scanBtn' class='btn btn-blue' onclick='startScan()'>🔍 Scan for Devices</button>";
    html += "</div></div>";
    html += "<div id='scanSummary' style='font-size:12px; color:#8b949e; margin-bottom:10px;'>Click 'Scan for Devices' to discover nearby BLE devices.</div>";
    html += "<div id='deviceList' style='display:flex; flex-direction:column; gap:8px; max-height:360px; overflow-y:auto; padding-right:4px;'>";
    html += "<div style='padding:20px; text-align:center; color:#8b949e;'>Click 'Scan for Devices' above to search for nearby Bluetooth devices.</div>";
    html += "</div></div>";

    // Card: Connection & PIN Configuration
    html += "<div class='card'>";
    html += "<div class='card-title'>Bluetooth Connection & PIN Configuration</div>";
    html += "<div class='grid' style='grid-template-columns: 1fr 1fr; gap:12px;'>";
    html += "<div>";
    html += "<label class='label' style='display:block; margin-bottom:4px;'>Target Device MAC Address</label>";
    html += "<input type='text' id='cfgTargetMac' placeholder='e.g. 24:4C:AB:12:34:56' />";
    html += "</div>";
    html += "<div>";
    html += "<label class='label' style='display:block; margin-bottom:4px;'>BLE Pairing PIN / Passkey (Default: 808978)</label>";
    html += "<input type='number' id='cfgPin' value='808978' placeholder='808978' />";
    html += "</div></div>";
    html += "<div style='margin-top:12px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;'>";
    html += "<label style='font-size:13px; color:#c9d1d9; display:flex; align-items:center; gap:6px; cursor:pointer;'>";
    html += "<input type='checkbox' id='cfgAuto' checked style='width:auto; cursor:pointer;' />";
    html += "Automatically reconnect to this device on boot";
    html += "</label>";
    html += "<div class='btn-group'>";
    html += "<button class='btn btn-primary' onclick='connectTarget()'>🔗 Connect to Device</button>";
    html += "<button class='btn' onclick='forgetTarget()'>🗑️ Forget Target</button>";
    html += "</div></div>";
    html += "<div id='connFeedback' style='margin-top:10px; font-size:13px; font-family:monospace; min-height:20px;'></div>";
    html += "</div>"; // End Card

    // Card: Bluetooth Security Keystore & Cache Management
    html += "<div class='card'>";
    html += "<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>";
    html += "<div class='card-title' style='margin-bottom:0;'>🗄️ Bluetooth Keystore & Bonding Cache</div>";
    html += "<div style='display:flex; gap:8px;'>";
    html += "<button class='btn btn-blue' onclick='fetchBonds()'>🔄 Refresh Keystore</button>";
    html += "<button class='btn btn-red' onclick='clearAllBonds()'>🗑️ Clear All Stored Bonds</button>";
    html += "</div></div>";
    html += "<div style='font-size:12px; color:#8b949e; margin-bottom:12px;'>";
    html += "The Bluetooth security keystore holds authenticated pairing keys and bonding records in NVS. If you change a device PIN or experience encryption status 261 errors, clearing the bond cache forces fresh PIN negotiation.";
    html += "</div>";
    html += "<div id='bondsList' style='display:flex; flex-direction:column; gap:8px;'>";
    html += "<div style='padding:15px; text-align:center; color:#8b949e;'>Loading bonded devices keystore...</div>";
    html += "</div>";
    html += "<div style='margin-top:14px; padding-top:12px; border-top:1px solid #30363d; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;'>";
    html += "<span style='font-size:12px; color:#8b949e;'>Scanner Cache: <b id='scanCacheCount'>0</b> device(s) in memory</span>";
    html += "<button class='btn' style='padding:4px 10px; font-size:12px;' onclick='clearDiscoveredCache()'>🧹 Clear Scanner Cache</button>";
    html += "</div>";
    html += "<div id='cacheFeedback' style='margin-top:8px; font-size:13px; font-family:monospace; min-height:18px;'></div>";
    html += "</div>"; // End Card Keystore

    html += "</div>"; // End tabConfig

    // Client-side JavaScript
    html += "<script>";
    html += "let ws = null;";
    html += "let myNodeName = 'BridgeWeb';";
    html += "let devicesList = [];";
    html += "let bondsList = [];";
    html += "let configPollInterval = null;";
    html += "const logBox = document.getElementById('logBox');";
    html += "const chatBox = document.getElementById('chatBox');";

    html += "function scanWiFi() {";
    html += "  const sel = document.getElementById('wifiSelect');";
    html += "  sel.innerHTML = '<option value=\"\">⏳ Scanning nearby Wi-Fi networks...</option>';";
    html += "  function pollScan() {";
    html += "    fetch('/api/wifi/scan').then(r => r.json()).then(d => {";
    html += "      if (d.scanning) {";
    html += "        setTimeout(pollScan, 800);";
    html += "        return;";
    html += "      }";
    html += "      let opts = '<option value=\"\">-- Select a Wi-Fi Network (' + (d.networks ? d.networks.length : 0) + ' found) --</option>';";
    html += "      if (d.networks && d.networks.length > 0) {";
    html += "        d.networks.sort((a,b) => b.rssi - a.rssi);";
    html += "        const seen = new Set();";
    html += "        d.networks.forEach(n => {";
    html += "          if (!n.ssid || seen.has(n.ssid)) return;";
    html += "          seen.add(n.ssid);";
    html += "          const lock = n.secure ? '🔒' : '🔓';";
    html += "          const signal = n.rssi > -65 ? '📶 Strong' : (n.rssi > -80 ? '📶 Medium' : '📶 Weak');";
    html += "          opts += `<option value=\"${encodeURIComponent(n.ssid)}\">${lock} ${n.ssid} (${n.rssi} dBm, ${signal})</option>`;";
    html += "        });";
    html += "      } else {";
    html += "        opts += '<option value=\"\">No networks found. Try scanning again.</option>';";
    html += "      }";
    html += "      sel.innerHTML = opts;";
    html += "    }).catch(e => {";
    html += "      sel.innerHTML = `<option value=\"\">Scan failed: ${e}</option>`;";
    html += "    });";
    html += "  }";
    html += "  pollScan();";
    html += "}";

    html += "function onSelectWiFiNetwork() {";
    html += "  const sel = document.getElementById('wifiSelect');";
    html += "  if (sel.value) {";
    html += "    document.getElementById('wifiManualSsid').value = decodeURIComponent(sel.value);";
    html += "  }";
    html += "}";

    html += "function togglePassVisibility() {";
    html += "  const p = document.getElementById('wifiPass');";
    html += "  p.type = p.type === 'password' ? 'text' : 'password';";
    html += "}";

    html += "function saveWiFi() {";
    html += "  const ssid = document.getElementById('wifiManualSsid').value.trim();";
    html += "  const pass = document.getElementById('wifiPass').value;";
    html += "  if (!ssid) { alert('Please select or enter a Wi-Fi SSID.'); return; }";
    html += "  const fb = document.getElementById('wifiFeedback');";
    html += "  fb.innerHTML = `<span style=\"color:#d29922;\">Connecting to \"${ssid}\"... Please wait.</span>`;";
    html += "  const btn = document.getElementById('btnSaveWifi');";
    html += "  btn.disabled = true;";
    html += "  btn.textContent = '⏳ Connecting...';";
    html += "  const params = new URLSearchParams();";
    html += "  params.append('ssid', ssid);";
    html += "  params.append('password', pass);";
    html += "  fetch('/api/wifi/save', { method: 'POST', body: params }).then(r => r.json()).then(d => {";
    html += "    fb.innerHTML = `<span style=\"color:#3fb950;\">Credentials saved! Connecting to ${ssid}...</span>`;";
    html += "    setTimeout(() => {";
    html += "      btn.disabled = false;";
    html += "      btn.textContent = '💾 Save & Connect';";
    html += "      checkStatus();";
    html += "    }, 4000);";
    html += "  }).catch(e => {";
    html += "    fb.innerHTML = `<span style=\"color:#da3633;\">Connection error: ${e}</span>`;";
    html += "    btn.disabled = false;";
    html += "    btn.textContent = '💾 Save & Connect';";
    html += "  });";
    html += "}";

    html += "function forgetWiFi() {";
    html += "  if (!confirm('Forget saved Wi-Fi credentials and switch back to Soft AP mode?')) return;";
    html += "  const fb = document.getElementById('wifiFeedback');";
    html += "  fb.innerHTML = '<span style=\"color:#d29922;\">Forgetting Wi-Fi network...</span>';";
    html += "  fetch('/api/wifi/forget', { method: 'POST' }).then(r => r.json()).then(d => {";
    html += "    fb.innerHTML = '<span style=\"color:#3fb950;\">Wi-Fi credentials erased. Soft AP mode active.</span>';";
    html += "    document.getElementById('wifiManualSsid').value = '';";
    html += "    document.getElementById('wifiPass').value = '';";
    html += "    setTimeout(checkStatus, 1500);";
    html += "  });";
    html += "}";

    html += "function switchTab(tab) {";
    html += "  document.getElementById('tabConsole').style.display = tab === 'console' ? 'flex' : 'none';";
    html += "  document.getElementById('tabConfig').style.display = tab === 'config' ? 'flex' : 'none';";
    html += "  document.getElementById('btnTabConsole').className = 'btn ' + (tab === 'console' ? 'btn-primary' : '');";
    html += "  document.getElementById('btnTabConfig').className = 'btn ' + (tab === 'config' ? 'btn-primary' : '');";
    html += "  if (tab === 'config') {";
    html += "    fetchDevices();";
    html += "    fetchBonds();";
    html += "    if (!configPollInterval) configPollInterval = setInterval(() => { fetchDevices(); fetchBonds(); }, 3000);";
    html += "  } else {";
    html += "    if (configPollInterval) { clearInterval(configPollInterval); configPollInterval = null; }";
    html += "  }";
    html += "}";

    html += "function fetchBonds() {";
    html += "  fetch('/api/bonds').then(r => r.json()).then(d => {";
    html += "    bondsList = d.bonds || [];";
    html += "    renderBonds();";
    html += "  }).catch(e => {";
    html += "    console.error('fetchBonds error:', e);";
    html += "    document.getElementById('bondsList').innerHTML = `<div style='color:#da3633; text-align:center; padding:10px;'>Failed to load keystore: ${e}</div>`;";
    html += "  });";
    html += "}";

    html += "function renderBonds() {";
    html += "  const el = document.getElementById('bondsList');";
    html += "  if (bondsList.length === 0) {";
    html += "    el.innerHTML = '<div style=\"padding:15px; text-align:center; color:#8b949e;\">No stored bonding keys found in keystore.</div>';";
    html += "    return;";
    html += "  }";
    html += "  let html = '';";
    html += "  bondsList.forEach(b => {";
    html += "    const authTag = b.authenticated ? '<span class=\"badge bg-green\">MITM Authenticated</span>' : '<span class=\"badge bg-yellow\">Unauthenticated (JustWorks)</span>';";
    html += "    const scTag = b.sc ? '<span class=\"badge bg-green\">Secure Conn</span>' : '<span class=\"badge\" style=\"background:#30363d; color:#8b949e;\">Legacy Sec</span>';";
    html += "    const ltkTag = b.ltk_present ? '<span class=\"badge\" style=\"background:#1f6feb; color:#fff;\">LTK Bond</span>' : '';";
    html += "    const irkTag = b.irk_present ? '<span class=\"badge\" style=\"background:#6f42c1; color:#fff;\">IRK Identity</span>' : '';";
    html += "    html += `<div style=\"background:#0d1117; border:1px solid #30363d; border-radius:6px; padding:10px 14px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;\">`;";
    html += "    html += `<div><div style=\"font-family:monospace; font-weight:bold; font-size:14px; color:#f0f6fc;\">${b.address}</div>`;";
    html += "    html += `<div style=\"display:flex; gap:6px; margin-top:5px; flex-wrap:wrap;\">${authTag} ${scTag} ${ltkTag} ${irkTag} <span class=\"badge\" style=\"background:#21262d; color:#8b949e;\">Key: ${b.key_size * 8} bits</span></div></div>`;";
    html += "    html += `<button class=\"btn btn-red\" style=\"padding:4px 10px; font-size:12px;\" onclick=\"deleteBond('${b.address}')\">Delete Bond 🗑️</button>`;";
    html += "    html += `</div>`;";
    html += "  });";
    html += "  el.innerHTML = html;";
    html += "}";

    html += "function deleteBond(mac) {";
    html += "  if (!confirm(`Delete bonding key for ${mac} from keystore?`)) return;";
    html += "  const fb = document.getElementById('cacheFeedback');";
    html += "  fb.innerHTML = `<span style=\"color:#d29922;\">Deleting bond for ${mac}...</span>`;";
    html += "  const params = new URLSearchParams();";
    html += "  params.append('mac', mac);";
    html += "  fetch('/api/bonds/delete', { method: 'POST', body: params }).then(r => r.json()).then(d => {";
    html += "    fb.innerHTML = `<span style=\"color:#3fb950;\">Bond deleted for ${mac}.</span>`;";
    html += "    fetchBonds();";
    html += "  }).catch(e => {";
    html += "    fb.innerHTML = `<span style=\"color:#da3633;\">Error deleting bond: ${e}</span>`;";
    html += "  });";
    html += "}";

    html += "function clearAllBonds() {";
    html += "  if (!confirm('Clear ALL stored security bonds from keystore? This will disconnect any active BLE connection.')) return;";
    html += "  const fb = document.getElementById('cacheFeedback');";
    html += "  fb.innerHTML = '<span style=\"color:#d29922;\">Clearing security keystore...</span>';";
    html += "  fetch('/api/bonds/clear', { method: 'POST' }).then(r => r.json()).then(d => {";
    html += "    fb.innerHTML = '<span style=\"color:#3fb950;\">All security bonds successfully erased from keystore.</span>';";
    html += "    fetchBonds();";
    html += "    checkStatus();";
    html += "  }).catch(e => {";
    html += "    fb.innerHTML = `<span style=\"color:#da3633;\">Error clearing keystore: ${e}</span>`;";
    html += "  });";
    html += "}";

    html += "function clearDiscoveredCache() {";
    html += "  fetch('/api/cache/clear-devices', { method: 'POST' }).then(r => r.json()).then(d => {";
    html += "    document.getElementById('cacheFeedback').innerHTML = '<span style=\"color:#3fb950;\">Discovered devices cache cleared.</span>';";
    html += "    devicesList = [];";
    html += "    renderDevices();";
    html += "  });";
    html += "}";

    html += "function fetchDevices() {";
    html += "  fetch('/api/devices').then(r => r.json()).then(d => {";
    html += "    devicesList = d.devices || [];";
    html += "    const targetInput = document.getElementById('cfgTargetMac');";
    html += "    if (d.target_mac && !targetInput.value) {";
    html += "      targetInput.value = d.target_mac;";
    html += "    }";
    html += "    const pinInput = document.getElementById('cfgPin');";
    html += "    if (d.pin && !pinInput.dataset.userEdited) pinInput.value = d.pin;";
    html += "    if (typeof d.auto_conn !== 'undefined') document.getElementById('cfgAuto').checked = d.auto_conn;";
    html += "    renderDevices();";
    html += "    const btn = document.getElementById('scanBtn');";
    html += "    if (d.scanning) {";
    html += "      btn.textContent = '⏳ Scanning...';";
    html += "      btn.disabled = true;";
    html += "    } else {";
    html += "      btn.textContent = '🔍 Scan for Devices';";
    html += "      btn.disabled = false;";
    html += "    }";
    html += "  }).catch(e => console.error('fetchDevices error:', e));";
    html += "}";

    html += "function startScan() {";
    html += "  const btn = document.getElementById('scanBtn');";
    html += "  btn.textContent = '⏳ Scanning...';";
    html += "  btn.disabled = true;";
    html += "  document.getElementById('scanSummary').textContent = 'Scanning for nearby Bluetooth devices...';";
    html += "  fetch('/api/scan', { method: 'POST' }).then(() => {";
    html += "    fetchDevices();";
    html += "  }).catch(e => {";
    html += "    console.error('Scan error:', e);";
    html += "    btn.textContent = '🔍 Scan for Devices';";
    html += "    btn.disabled = false;";
    html += "  });";
    html += "}";

    html += "function renderDevices() {";
    html += "  const showAll = document.getElementById('showAllCheck').checked;";
    html += "  const listEl = document.getElementById('deviceList');";
    html += "  const filtered = devicesList.filter(d => showAll || d.is_mesh);";
    html += "  const meshCount = devicesList.filter(d => d.is_mesh).length;";
    html += "  document.getElementById('scanSummary').textContent = `Found ${devicesList.length} device(s) (${meshCount} MeshCore candidate(s))`;";
    html += "  if (filtered.length === 0) {";
    html += "    listEl.innerHTML = `<div style='padding:20px; text-align:center; color:#8b949e;'>${devicesList.length > 0 ? 'No MeshCore devices match candidate filter. Check \"Show all Bluetooth devices\" to see all ' + devicesList.length + ' nearby devices.' : 'No devices found yet. Click \"Scan for Devices\" above.'}</div>`;";
    html += "    return;";
    html += "  }";
    html += "  filtered.sort((a,b) => b.rssi - a.rssi);";
    html += "  let html = '';";
    html += "  filtered.forEach(d => {";
    html += "    const isConn = d.connected;";
    html += "    const name = d.name || 'Unnamed Device';";
    html += "    const tag = d.is_mesh ? `<span class='badge bg-green'>MeshCore / NUS</span>` : `<span class='badge' style='background:#30363d; color:#8b949e;'>Generic BLE</span>`;";
    html += "    const rssiColor = d.rssi > -70 ? '#3fb950' : (d.rssi > -85 ? '#d29922' : '#da3633');";
    html += "    const encName = encodeURIComponent(name);";
    html += "    html += `<div class='device-card ${isConn ? \"device-connected\" : \"\"}' onclick='selectDevice(\"${d.address}\", \"${encName}\")'>`;";
    html += "    html += `<div style='display:flex; justify-content:space-between; align-items:center;'><div>`;";
    html += "    html += `<div style='font-weight:600; font-size:14px; color:#f0f6fc; display:flex; align-items:center; gap:8px;'>${d.is_mesh ? '📻' : '📱'} ${name} ${tag} ${isConn ? '<span class=\"badge bg-green\">Active</span>' : ''}</div>`;";
    html += "    html += `<div style='font-family:monospace; font-size:12px; color:#8b949e; margin-top:3px;'>${d.address}</div></div>`;";
    html += "    html += `<div style='text-align:right;'><span style='font-family:monospace; font-weight:bold; color:${rssiColor}; font-size:13px;'>${d.rssi} dBm</span>`;";
    html += "    html += `<div style='margin-top:4px;'><button class='btn ${isConn ? \"btn-blue\" : \"btn-primary\"}' style='padding:4px 10px; font-size:12px;' onclick='event.stopPropagation(); selectAndConnect(\"${d.address}\", \"${encName}\")'>${isConn ? 'Connected' : 'Connect 🔗'}</button></div>`;";
    html += "    html += `</div></div></div>`;";
    html += "  });";
    html += "  listEl.innerHTML = html;";
    html += "}";

    html += "function selectDevice(addr, encName) {";
    html += "  const name = decodeURIComponent(encName || '');";
    html += "  document.getElementById('cfgTargetMac').value = addr;";
    html += "  document.getElementById('connFeedback').innerHTML = `<span style='color:#58a6ff;'>Selected: <b>${name || addr}</b> (${addr})</span>`;";
    html += "}";

    html += "function selectAndConnect(addr, encName) {";
    html += "  selectDevice(addr, encName);";
    html += "  connectTarget();";
    html += "}";

    html += "function connectTarget() {";
    html += "  const addr = document.getElementById('cfgTargetMac').value.trim();";
    html += "  const pin = document.getElementById('cfgPin').value.trim() || '808978';";
    html += "  const autoConn = document.getElementById('cfgAuto').checked;";
    html += "  if (!addr) { alert('Please enter or select a Bluetooth device MAC address.'); return; }";
    html += "  const fb = document.getElementById('connFeedback');";
    html += "  fb.innerHTML = `<span style='color:#d29922;'>Connecting to ${addr} with PIN ${pin}...</span>`;";
    html += "  const params = new URLSearchParams();";
    html += "  params.append('address', addr);";
    html += "  params.append('pin', pin);";
    html += "  params.append('save', 'true');";
    html += "  params.append('auto', autoConn ? 'true' : 'false');";
    html += "  fetch('/api/connect', { method: 'POST', body: params }).then(r => r.json()).then(d => {";
    html += "    fb.innerHTML = `<span style='color:#3fb950;'>Connection initiated to ${addr}!</span>`;";
    html += "    setTimeout(fetchDevices, 2000);";
    html += "    setTimeout(checkStatus, 2000);";
    html += "  }).catch(e => { fb.innerHTML = `<span style='color:#da3633;'>Connection error: ${e}</span>`; });";
    html += "}";

    html += "function disconnectTarget() {";
    html += "  const fb = document.getElementById('connFeedback');";
    html += "  fb.innerHTML = `<span style='color:#d29922;'>Disconnecting...</span>`;";
    html += "  fetch('/api/disconnect', { method: 'POST' }).then(r => r.json()).then(() => {";
    html += "    fb.innerHTML = `<span style='color:#8b949e;'>Disconnected.</span>`;";
    html += "    fetchDevices();";
    html += "    checkStatus();";
    html += "  });";
    html += "}";

    html += "function forgetTarget() {";
    html += "  if (!confirm('Forget configured target Bluetooth device?')) return;";
    html += "  fetch('/api/forget', { method: 'POST' }).then(r => r.json()).then(() => {";
    html += "    document.getElementById('cfgTargetMac').value = '';";
    html += "    document.getElementById('connFeedback').innerHTML = `<span style='color:#8b949e;'>Target device forgotten.</span>`;";
    html += "    fetchDevices();";
    html += "    checkStatus();";
    html += "  });";
    html += "}";

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

    html += "  if (code === 0x05) {";
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
    html += "  } else if (code === 0x06) {";
    html += "    log('Message broadcast queued on mesh radio', 'info');";
    html += "  } else if (code === 0x08 || code === 0x11) {";
    html += "    const chan = p[4] || 0;";
    html += "    const senderBytes = p.slice(5, 37);";
    html += "    const senderHex = Array.from(senderBytes.slice(0,4)).map(b => b.toString(16).padStart(2,'0')).join('');";
    html += "    const text = new TextDecoder().decode(p.slice(37));";
    html += "    addChat(`Node ${senderHex} (Ch ${chan})`, text, 'rx');";
    html += "  } else if (code === 0x0c) {";
    html += "    const mv = p[1] | (p[2] << 8);";
    html += "    document.getElementById('valBattery').textContent = `${(mv/1000).toFixed(2)} V (${mv} mV)`;";
    html += "  } else if (code === 0x0a) {";
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
    html += "    const aName = document.getElementById('activeDevName');";
    html += "    const aAddr = document.getElementById('activeDevAddr');";
    html += "    const aRssi = document.getElementById('activeDevRSSI');";
    html += "    const btnDisc = document.getElementById('btnDisconnect');";
    html += "    const wBadge = document.getElementById('wifiHeaderBadge');";
    html += "    const wModeBadge = document.getElementById('wifiModeBadge');";
    html += "    const wDesc = document.getElementById('wifiStatusDesc');";
    html += "    const btnForget = document.getElementById('btnForgetWifi');";
    html += "    const valNetMode = document.getElementById('valNetMode');";
    html += "    const valWiFiSSID = document.getElementById('valWiFiSSID');";
    html += "    const valBridgeIP = document.getElementById('valBridgeIP');";
    html += "    const valTcpPort = document.getElementById('valTcpPort');";
    html += "    const valWsPort = document.getElementById('valWsPort');";
    html += "    const valBleDev = document.getElementById('valBleDev');";
    html += "    const valBlePackets = document.getElementById('valBlePackets');";
    html += "    if (valNetMode) valNetMode.textContent = d.is_ap_mode ? 'Soft AP Mode' : 'Station Mode';";
    html += "    if (valWiFiSSID) valWiFiSSID.textContent = d.is_ap_mode ? d.ap_ssid : (d.wifi_ssid + (d.wifi_rssi ? ` (${d.wifi_rssi} dBm)` : ''));";
    html += "    if (valBridgeIP) valBridgeIP.textContent = d.ip_address || '-';";
    html += "    if (valTcpPort) valTcpPort.textContent = `${d.ip_address}:${d.tcp_port}`;";
    html += "    if (valWsPort) valWsPort.textContent = `ws://${d.ip_address}:${d.ws_port}`;";
    html += "    if (valBleDev) valBleDev.textContent = `${d.ble_device_name || '-'} (${d.ble_rssi || 0} dBm)`;";
    html += "    if (valBlePackets) valBlePackets.textContent = `${d.ble_rx} / ${d.ble_tx}`;";
    html += "    if (d.is_ap_mode) {";
    html += "      if (wBadge) { wBadge.className = 'badge bg-yellow'; wBadge.textContent = 'AP: ' + (d.ap_ssid || 'Setup'); }";
    html += "      if (wModeBadge) { wModeBadge.className = 'badge bg-yellow'; wModeBadge.textContent = 'Soft AP: ' + d.ap_ssid; }";
    html += "      if (wDesc) wDesc.innerHTML = `Bridge is in Soft AP mode (<b>${d.ap_ssid}</b> at <b>192.168.4.1</b>). Connect to your home Wi-Fi network below.`;";
    html += "      if (btnForget) btnForget.style.display = 'none';";
    html += "    } else if (d.wifi_connected) {";
    html += "      if (wBadge) { wBadge.className = 'badge bg-green'; wBadge.textContent = 'WiFi: ' + d.wifi_ssid; }";
    html += "      if (wModeBadge) { wModeBadge.className = 'badge bg-green'; wModeBadge.textContent = 'Connected: ' + d.wifi_ssid; }";
    html += "      if (wDesc) wDesc.innerHTML = `Connected to <b>${d.wifi_ssid}</b> (IP: <b>${d.ip_address}</b> | RSSI: <b>${d.wifi_rssi} dBm</b>).`;";
    html += "      if (btnForget) btnForget.style.display = 'inline-flex';";
    html += "    } else {";
    html += "      if (wBadge) { wBadge.className = 'badge bg-red'; wBadge.textContent = 'WiFi: Disconnected'; }";
    html += "      if (wModeBadge) { wModeBadge.className = 'badge bg-red'; wModeBadge.textContent = 'Connecting...'; }";
    html += "      if (wDesc) wDesc.innerHTML = 'Connecting to Wi-Fi...';";
    html += "      if (btnForget) btnForget.style.display = 'inline-flex';";
    html += "    }";
    html += "    if (d.ble_connected) {";
    html += "      b.className = 'badge bg-green';";
    html += "      b.textContent = `BLE: ${d.ble_device_name} (${d.ble_rssi} dBm)`;";
    html += "      aName.textContent = `Connected: ${d.ble_device_name}`;";
    html += "      aAddr.textContent = `${d.ble_mac} (PIN: ${d.ble_pin})`;";
    html += "      aRssi.className = 'badge bg-green';";
    html += "      aRssi.textContent = `${d.ble_rssi} dBm`;";
    html += "      btnDisc.style.display = 'inline-flex';";
    html += "    } else if (d.ble_connecting) {";
    html += "      b.className = 'badge bg-yellow';";
    html += "      b.textContent = 'BLE: Connecting...';";
    html += "      aName.textContent = 'Connecting to Bluetooth device...';";
    html += "      aAddr.textContent = d.target_mac || 'Searching...';";
    html += "      aRssi.className = 'badge bg-yellow';";
    html += "      aRssi.textContent = 'Connecting';";
    html += "      btnDisc.style.display = 'none';";
    html += "    } else {";
    html += "      b.className = 'badge bg-yellow';";
    html += "      b.textContent = 'BLE: Disconnected';";
    html += "      aName.textContent = 'No device connected';";
    html += "      aAddr.textContent = d.target_mac ? `Target: ${d.target_mac}` : 'No target configured';";
    html += "      aRssi.className = 'badge bg-red';";
    html += "      aRssi.textContent = 'Disconnected';";
    html += "      btnDisc.style.display = 'none';";
    html += "    }";
    html += "  }).catch(() => {});";
    html += "}";

    html += "window.onload = () => {";
    html += "  connectWS();";
    html += "  checkStatus();";
    html += "  fetchDevices();";
    html += "  setInterval(checkStatus, 4000);";
    html += "};";
    html += "</script>";

    html += "</div></body></html>";
    return html;
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
    pBLEScan->setInterval(160);
    pBLEScan->setWindow(40);

    Serial.println("[BLE] Starting initial discovery scan...");
    startBLEScan(5, true);
}

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
            scanBackoffInterval = MIN_SCAN_BACKOFF;
        } else {
            Serial.println("[BLE] Connection attempt failed, will retry scan...");
            delay(1000);
            bleConnecting = false;
            lastScanAttempt = millis();
        }
    } else if (!bleConnected) {
        if (autoConnectEnabled && configuredTargetMac.length() > 0) {
            if (millis() - lastScanAttempt > scanBackoffInterval && !bleScanning) {
                lastScanAttempt = millis();
                Serial.printf("[BLE] Offline reconnect scan attempt (interval: %lu ms, backoff: %lu s)...\n",
                              scanBackoffInterval, scanBackoffInterval / 1000);
                startBLEScan(5, false);
                if (scanBackoffInterval < MAX_SCAN_BACKOFF) {
                    scanBackoffInterval = min(MAX_SCAN_BACKOFF, scanBackoffInterval + SCAN_BACKOFF_STEP);
                }
            }
        }
    }

    // Ensure background discovery scans are immediately stopped if BLE is connected
    if (bleConnected && bleScanning) {
        BLEDevice::getScan()->stop();
        bleScanning = false;
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
