# ESP32 BLE HID Firmware

## Wi-Fi operating modes

### Station mode
The firmware saves Wi-Fi credentials in the `wifi` namespace of NVS (`ssid` and `password` keys) and, on boot, attempts to join the stored network as a station using those values. If the stored credentials connect successfully, the device leaves configuration mode and stays on the infrastructure network.【F:src/main.cpp†L33-L35】【F:src/main.cpp†L525-L610】【F:src/main.cpp†L2622-L2654】

### Access point & configuration mode
When no credentials are saved or the station connection fails, the firmware falls back to a configuration access point named `uhid-setup` (password `uhid1234`) on `192.168.4.1`. The access point is brought up with its own gateway/subnet and the captive portal UI bundled into the firmware image.【F:src/main.cpp†L33-L44】【F:src/main.cpp†L1464-L1474】【F:src/main.cpp†L2657-L2663】【F:src/main.cpp†L337-L338】

### Captive portal behaviour
Captive portal support is enabled alongside the access point: DNS queries are redirected to the ESP32, and HTTP requests for the root page or common OS portal probes (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`) all return the embedded configuration UI. This keeps phones and laptops anchored on the setup page while credentials are provisioned.【F:src/main.cpp†L688-L707】【F:src/main.cpp†L1099-L1200】

### Transition back to station-only operation
While connecting to a new network, the firmware can temporarily run in AP+STA mode so that the portal remains reachable. After a successful station connection it schedules the access point to shut down a few seconds later, freeing the radio for BLE and Wi-Fi client duties.【F:src/main.cpp†L1477-L1528】【F:src/main.cpp†L781-L801】【F:src/main.cpp†L1736-L1764】

## WebSocket command transport

Commands can be delivered over USB UART or a Wi-Fi WebSocket. The active transport, along with the UART baud rate, is stored in the `transport` NVS namespace and can be changed through the `/api/transport` REST endpoint in the captive portal. Switching to WebSocket enables the `/ws` and `/ws/hid` endpoints, which stream JSON payloads through FreeRTOS queues so HID actions are processed just like serial input.【F:src/main.cpp†L36-L108】【F:src/main.cpp†L263-L316】【F:src/main.cpp†L1021-L1090】【F:src/main.cpp†L1202-L1288】【F:src/main.cpp†L1290-L1320】

## Resetting Wi-Fi credentials

Because the credentials live in NVS, clearing that namespace returns the device to access-point setup mode. The quickest approach during development is to erase the NVS partition (for example with `pio run -t erase` or `esptool.py erase_flash`); on the next boot, the firmware finds no saved SSID, launches the `uhid-setup` portal, and emits the `wifi_config_mode` event for clients listening on UART/WebSocket.【F:src/main.cpp†L33-L35】【F:src/main.cpp†L525-L610】【F:src/main.cpp†L2657-L2663】

## Build configuration

The PlatformIO environment embeds `src/main/web/index.html` into flash so the captive portal can serve a self-contained UI. Adjust or extend the portal by editing that HTML file and rebuilding; the `board_build.embed_txtfiles` directive handles bundling the asset into the firmware image.【F:platformio.ini†L1-L22】
