#!/usr/bin/env python3
"""
Simple UART driver for the ESP32 BLE HID bridge.

Examples
--------
Send text:
    python3 ble_hid_uart_client.py keyboard --text "Hello world"

Press Ctrl+Alt+Delete:
    python3 ble_hid_uart_client.py keyboard --action tap --keys ctrl,alt,delete

Move the mouse and right-click:
    python3 ble_hid_uart_client.py mouse --action move --dx 50 --dy -10
    python3 ble_hid_uart_client.py mouse --action click --buttons right

By default the tool listens for ~1.5 seconds after each command to print
`status`/`event` messages. Use `--listen-for -1` to keep running until Ctrl+C.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Iterable, List, Optional

import serial  # type: ignore

JSON_DOC_CAPACITY = 512


def _split_tokens(raw: Optional[str]) -> Optional[List[str]]:
    if raw is None:
        return None
    tokens = [token.strip() for token in raw.replace("+", ",").split(",")]
    return [token for token in tokens if token]


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be > 0")
    return parsed


def _non_negative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be >= 0")
    return parsed


def _wait_for_ready(port: serial.Serial, timeout: float) -> None:
    deadline = time.time() + timeout
    while timeout <= 0 or time.time() < deadline:
        line = port.readline()
        if not line:
            continue
        try:
            decoded = json.loads(line.decode("utf-8", errors="ignore"))
        except json.JSONDecodeError:
            continue
        if decoded.get("event") == "ready":
            print("[client] device reported ready")
            return
    raise TimeoutError("timed out waiting for ready event")


def _build_keyboard_command(args: argparse.Namespace) -> dict:
    command = {
        "device": "keyboard",
        "action": args.action,
    }
    tokens = _split_tokens(args.keys)
    if args.text:
        command["text"] = args.text
    if args.newline:
        command["newline"] = True
    if tokens:
        command["keys"] = tokens
    if args.repeat:
        command["repeat"] = args.repeat
    if args.hold_ms is not None:
        command["holdMs"] = args.hold_ms
    if args.char_delay is not None:
        command["charDelayMs"] = args.char_delay
    if args.newline_no_cr:
        command["newlineCarriage"] = False
    return command


def _build_mouse_command(args: argparse.Namespace) -> dict:
    command = {
        "device": "mouse",
        "action": args.action,
    }
    if args.dx is not None:
        command["dx"] = args.dx
    if args.dy is not None:
        command["dy"] = args.dy
    if args.wheel is not None:
        command["wheel"] = args.wheel
    if args.pan is not None:
        command["pan"] = args.pan
    tokens = _split_tokens(args.buttons)
    if tokens:
        command["buttons"] = tokens
    return command


def _build_consumer_command(args: argparse.Namespace) -> dict:
    tokens = _split_tokens(args.keys)
    if not tokens:
        raise SystemExit("consumer action requires --keys")
    command = {
        "device": "consumer",
        "keys": tokens,
    }
    if args.repeat:
        command["repeat"] = args.repeat
    if args.gap_ms is not None:
        command["gapMs"] = args.gap_ms
    return command


def _build_raw_command(args: argparse.Namespace) -> dict:
    try:
        payload = json.loads(args.json)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"invalid JSON payload: {exc}") from exc
    if not isinstance(payload, dict):
        raise SystemExit("JSON root must be an object")
    return payload


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send JSON HID commands over UART.")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="serial port path (default: %(default)s)")
    parser.add_argument("--baud", type=int, default=115200, help="baud rate (default: %(default)s)")
    parser.add_argument("--wait-ready", type=float, default=0.0, help="seconds to wait for ready event (0 to skip)")
    parser.add_argument("--listen-for", type=float, default=1.5, help="seconds to listen for responses (-1 for until Ctrl+C)")
    subparsers = parser.add_subparsers(dest="command", required=True)

    kb = subparsers.add_parser("keyboard", help="send keyboard command")
    kb.add_argument("--action", default="write", choices=["write", "print", "println", "press", "release", "tap", "click", "releaseAll", "release_all"])
    kb.add_argument("--text", help="text payload for write/print/println")
    kb.add_argument("--keys", help="comma or plus separated key names, e.g. CTRL,ALT,DELETE")
    kb.add_argument("--repeat", type=_positive_int, help="repeat count")
    kb.add_argument("--newline", action="store_true", help="append newline after text or key write")
    kb.add_argument("--hold-ms", type=_non_negative_int, dest="hold_ms", help="tap hold duration in milliseconds")
    kb.add_argument("--char-delay", type=_non_negative_int, dest="char_delay", help="delay between characters in milliseconds")
    kb.add_argument("--newline-no-cr", action="store_true", help="omit carriage return when newline is emitted")

    ms = subparsers.add_parser("mouse", help="send mouse command")
    ms.add_argument("--action", default="move", choices=["move", "click", "press", "release", "releaseAll", "release_all"])
    ms.add_argument("--dx", type=int, help="X delta")
    ms.add_argument("--dy", type=int, help="Y delta")
    ms.add_argument("--wheel", type=int, help="vertical wheel delta")
    ms.add_argument("--pan", type=int, help="horizontal wheel delta")
    ms.add_argument("--buttons", help="buttons to act on (comma list, defaults to left for click/press/release)")

    cs = subparsers.add_parser("consumer", help="send consumer/media command")
    cs.add_argument("--keys", required=True, help="comma or plus separated consumer usages, e.g. KEY_MEDIA_PLAY_PAUSE")
    cs.add_argument("--repeat", type=_positive_int, help="repeat count")
    cs.add_argument("--gap-ms", type=_non_negative_int, dest="gap_ms", help="delay between keys in milliseconds")

    raw = subparsers.add_parser("raw", help="send raw JSON string")
    raw.add_argument("json", help="JSON payload to send (must already include device/type)")

    return parser


def _build_payload(args: argparse.Namespace) -> dict:
    if args.command == "keyboard":
        return _build_keyboard_command(args)
    if args.command == "mouse":
        return _build_mouse_command(args)
    if args.command == "consumer":
        return _build_consumer_command(args)
    if args.command == "raw":
        return _build_raw_command(args)
    raise SystemExit(f"unsupported command {args.command}")


def _open_serial(args: argparse.Namespace) -> serial.Serial:
    return serial.Serial(
        port=args.port,
        baudrate=args.baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.1,
    )


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        ser = _open_serial(args)
    except serial.SerialException as exc:
        print(f"[client] failed to open {args.port}: {exc}", file=sys.stderr)
        return 2

    if args.wait_ready > 0:
        ser.reset_input_buffer()
        try:
            _wait_for_ready(ser, args.wait_ready)
        except TimeoutError as exc:
            print(f"[client] {exc}", file=sys.stderr)

    payload = _build_payload(args)
    serialized = json.dumps(payload, separators=(",", ":"))
    if len(serialized) + 1 > JSON_DOC_CAPACITY:
        print("[client] warning: payload exceeds 512 bytes and may be rejected", file=sys.stderr)

    ser.write(serialized.encode("utf-8") + b"\n")
    ser.flush()
    print(f"[client] sent: {serialized}")

    listen_for = args.listen_for
    try:
        if listen_for != 0:
            if listen_for < 0:
                print("[client] listening for responses (Ctrl+C to stop)...")
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    text = raw.decode("utf-8", errors="replace").rstrip()
                    if text:
                        print(f"[ESP32] {text}")
            else:
                deadline = time.time() + listen_for
                while time.time() < deadline:
                    raw = ser.readline()
                    if not raw:
                        continue
                    text = raw.decode("utf-8", errors="replace").rstrip()
                    if text:
                        print(f"[ESP32] {text}")
    except KeyboardInterrupt:
        print("\n[client] stopped listening")

    ser.close()
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
