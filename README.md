# ESP32-C3 MeshCore BLE Bridge (esp32-meshcore-ble-bridge)

This repository contains an ESP32-C3 PlatformIO application that acts as a BLE bridge between a MeshCore BLE peripheral and network clients (both native TCP and WebSockets for web browsers), along with an interactive Web Console and updated client defaults in `meshcore-open`.

---

## 🌟 Features

- **Bluetooth Device Manager & Web Configuration**:
  - **Live BLE Device Scanner**: Discover nearby Bluetooth devices on demand from the Web UI.
  - **Candidate Filtering**: Automatically highlights valid MeshCore / Nordic UART candidates.
  - **Show All Devices Filter**: Optional checkbox to display all discovered Bluetooth devices regardless of name or advertising service.
  - **Configurable PIN / Passkey**: Enter custom pairing PINs (defaults to `808978`).
  - **NVS Persistent Memory**: Saves selected target MAC address, PIN, and auto-reconnect preference across reboots.
  - **GATT Bridge**: Subscribes to TX notifications (`6e400003-...`) and forwards to all TCP / WebSocket clients.

- **Dual Network Transports & Web Console**:
  - **Interactive Web Console & Dashboard (Port `80`)**: Full in-browser web app with live LoRa mesh chat, node queries, beacon broadcast, time sync, battery monitor, live protocol packet inspector, and Bluetooth configuration manager.
  - **Raw TCP Server (Port `5000`)**: Standard MeshCore framing (`<` for TX, `>` for RX) for native apps and CLI clients.
  - **WebSocket Server (Port `5001`)**: Binary WebSocket transport for Web browsers (MeshCore Open Web).

- **WiFi Configuration**:
  - **WiFi Station Mode**: Configurable SSID and Password in `src/main.cpp`.
  - **Power-Save Disabled**: Zero-latency packet processing (`WiFi.setSleep(false)`).
  - **mDNS Responder**: Reachable via `http://meshcore-ble-bridge.local`.

---

## 📡 Endpoints Summary

| Service | Port | Endpoint | Purpose |
| :--- | :--- | :--- | :--- |
| **Web Console & Chat** | `80` | `http://<device-ip>/` or `http://meshcore-ble-bridge.local` | Live Web UI & LoRa mesh chat console |
| **Status API** | `80` | `http://<device-ip>/status` | JSON telemetry endpoint |
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
