# ESP32-C3 MeshCore BLE Bridge (esp32-meshcore-ble-bridge)

This repository contains an ESP32-C3 PlatformIO application that acts as a BLE bridge between a MeshCore BLE peripheral and network clients (both native TCP and WebSockets for web browsers), along with an interactive Web Console and updated client defaults in `meshcore-open`.

---

## 🌟 Features

- **Automatic BLE Client Bridge**:
  - Automatically scans and pairs with MeshCore devices advertising NUS (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`).
  - Supports BLE Security / Bonding with passkey (PIN: `808978`).
  - Subscribes to TX notifications (`6e400003-...`) and forwards to all connected TCP / WebSocket clients.
  - Receives frames from TCP / WebSocket clients and writes to BLE RX (`6e400002-...`).
  - Auto-reconnects with backoff if the BLE device goes out of range or disconnects.

- **Dual Network Transports & Web Console**:
  - **Interactive Web Console & Dashboard (Ports `80` & `5000`)**: Full in-browser web app with live LoRa mesh chat, node queries, beacon broadcast, time sync, battery monitor, and live protocol packet inspector.
  - **Raw TCP Server (Port `5000`)**: Standard MeshCore framing (`<` for TX, `>` for RX) for native apps and CLI clients.
  - **WebSocket Server (Port `5001`)**: Binary WebSocket transport for Web browsers (MeshCore Open Web).

- **WiFi Configuration**:
  - **WiFi Station Mode**: Configurable SSID and Password in `src/main.cpp`.
  - **Power-Save Disabled**: Zero-latency packet processing (`WiFi.setSleep(false)`).
  - **mDNS Responder**: Reachable via `http://meshcore-ble-bridge.local` or `meshcore-ble-bridge.local:5000`.

---

## 📡 Endpoints Summary

| Service | Port | Endpoint | Purpose |
| :--- | :--- | :--- | :--- |
| **Web Console & Chat** | `80` / `5000` | `http://<device-ip>:5000/` or `http://meshcore-ble-bridge.local` | Live Web UI & LoRa mesh chat console |
| **Status API** | `80` / `5000` | `http://<device-ip>:5000/status` | JSON telemetry endpoint |
| **Native TCP** | `5000` | `<device-ip>:5000` | MeshCore Companion TCP protocol |
| **WebSocket** | `5001` | `ws://<device-ip>:5001` | Browser WebSocket transport for Web Client |

---

## 🛠️ MeshCore-Open Web Client Changes

In [`meshcore-open/`](meshcore-open/):
1. **Default Transport**:
   - `lib/main.dart` now defaults `home` to `TcpScreen` on Web platforms instead of Bluetooth scanning.
   - `lib/screens/tcp_screen.dart` pre-populates default bridge host and port (`5000`) on Web.
2. **WebSocket Transport for Web**:
   - `lib/services/tcp_transport_service_web.dart` implements WebSocket binary transport with automatic port mapping (5000 -> 5001) and USB serial frame decoding (`<`/`>`).
3. **Screen Updates**:
   - `lib/screens/scanner_screen.dart`, `lib/screens/usb_screen.dart`, and `lib/screens/chrome_required_screen.dart` include direct TCP / LAN connect actions.
