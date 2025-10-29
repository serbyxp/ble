# FastAPI UART Control Panel

This directory contains a small FastAPI application that exposes the ESP32 BLE HID JSON protocol through HTTP and a browser UI. The goal is to make it easy to type text, trigger key combinations, control media keys, and move/click the mouse without running individual CLI commands.

## Dependencies

Install the required Python packages (preferably in a virtual environment):

```powershell
py -m pip install fastapi uvicorn[standard] pyserial
```

or on Linux:

```bash
python3 -m pip install fastapi "uvicorn[standard]" pyserial
```

## Running the server

```powershell
uvicorn web.server:app --reload
```

Environment variables control the initial serial port configuration:

- `UART_PORT` (default: auto-detected – `COM3` on Windows, `/dev/ttyUSB0` on Linux, `/dev/cu.usbserial` on macOS)
- `UART_BAUD` (default: `115200`)
- `UART_LISTEN_SECONDS` (default: `0.5`)

You can also change the port from the browser UI (Connect form) without restarting the server.

Once the server is running, open <http://127.0.0.1:8000/> in your browser. The page provides:

- **Connection panel** – set the serial port and baud rate.
- **Keyboard text** – type text, choose newline and delay, and send it to the ESP32.
- **Key combos** – quick buttons for shortcuts such as Ctrl+Alt+Delete.
- **Media controls** – play/pause, next/previous track, volume, mute, etc.
- **Mouse controls** – directional movement, scrolling/panning, and click buttons.
- **Remote viewer overlay** – drop in your GStreamer/uxplayer `<video>` feed and capture mouse/keyboard input whenever the pointer is inside the frame.
- **Log view** – shows both the commands issued and the JSON responses from the device.

If the ESP32 is not yet in range or paired, the UI will still load; once the device is ready, use the Connect form to reopen the serial port.

If the firmware cannot persist a configuration change, `/api/config` responds with a JSON error such as "Failed to persist configuration. Please retry or power-cycle the device." The UI surfaces this message so you can retry or cycle power before continuing.

### Customising behaviour

- **Character delay** – slow down typing if the host drops characters.
- **Mouse step / delay** – tune movement distance and smoothness.
- **Listen window** – how long to capture responses after each command.
- **Overlay capture** – toggle pointer capture, adjust the listen window, or quickly release control with the toolbar buttons above the viewer.

All commands are routed to the firmware exactly as JSON payloads, so any new behaviours can be crafted via the “raw” API endpoint or by extending the UI. The HTTP endpoints are documented in `web/server.py`.
