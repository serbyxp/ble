# Repository Guidelines
- Always use context7 when I need code generation, setup or configuration steps, or
library/API documentation. This means you should automatically use the Context7 MCP
tools to resolve library id and get library docs without me having to explicitly ask.

## Project Structure & Module Organization
- `src/` – ESP32 Arduino firmware (BLE HID bridge). Key modules: `ble_command_processor.*`, `transport_uart.*`, `transport_websocket.*`, entrypoint `main.cpp`.
- `include/` – Public headers for cross-module use.
- `lib/ESP32-BLE-Combo/` – Vendored library; do not modify without upstream sync.
- `dev_server/` – FastAPI UART control panel. App: `dev_server/server.py`; static UI: `dev_server/static/`.
- `test/` – Manual UART client and HID demo scripts. See `test/README` for usage.
- `src/web/` – Minimal static assets used by the firmware.
- `platformio.ini` – PlatformIO environments and build settings.

## Build, Test, and Development Commands
- Build firmware: `pio run -e nodemcu-32s`
- Flash firmware: `pio run -e nodemcu-32s -t upload`
- Serial monitor: `pio device monitor -b 115200`
- Static analysis (optional): `pio check`
- Manual UART test (Linux): `python3 test/ble_hid_uart_client.py --port /dev/ttyUSB0 keyboard --text "Hello"`
- Manual UART test (Windows): `py test\ble_hid_uart_client.py --port COM3 --listen-for 2 keyboard --text "Hello"`
- Browser control panel: `uvicorn dev_server.server:app --reload` (requires `fastapi`, `uvicorn[standard]`, `pyserial`)
  - Config via env: `UART_PORT`, `UART_BAUD`, `UART_LISTEN_SECONDS`

## Coding Style & Naming Conventions
- C++ (firmware): 2-space indent; filenames `snake_case`; classes `PascalCase`; constants `UPPER_SNAKE_CASE`. Prefer small, single‑purpose functions; avoid heap allocation in hot paths.
- Python (tools/server): 4-space indent; `snake_case` for modules/functions; add type hints where reasonable.
- Keep includes local-first, then library headers. Group related functions together; keep headers minimal.

## Testing Guidelines
- Provide a manual test plan in PRs (commands, expected JSON responses). Attach serial logs where useful.
- Unit tests (if added) live in `test/` and can be run with `pio test`; current scripts are manual runners documented in `test/README`.

## Commit & Pull Request Guidelines
- Use concise, descriptive commits (e.g., `feat: add media key mapping`, `fix: debounce UART reads`).
- PRs should include: purpose/why, summary of changes, test plan, linked issues, and screenshots or serial output when relevant.
- Keep PRs focused and small; update docs/comments when behavior changes.

## Security & Configuration Tips
- Do not commit secrets or local paths. `platformio.ini` sets `monitor_speed=115200`; ensure your OS serial port matches. On Windows, ensure `build_dir` exists or drop the override.
