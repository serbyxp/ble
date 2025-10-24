#!/usr/bin/env python3
"""
High-level demo that recreates the original keyboard/mouse behaviour from
lib/ESP32-BLE-Combo-Examples/KeyboardMouseExample, but drives it through the
UART JSON protocol.

Typical usage (Windows PowerShell):

    py test\hid_demo.py --port COM3

Linux/Raspberry Pi:

    python3 test/hid_demo.py --port /dev/ttyUSB0

For best results open a text editor window on the paired host before running
the script so you can see the keyboard entry and pointer motion.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Iterable, Optional

import serial  # type: ignore

DEFAULT_CHAR_DELAY_MS = 6
DEFAULT_MOUSE_STEP = 120


def wait_for_ready(port: serial.Serial, timeout: float) -> None:
    deadline = time.time() + timeout
    while timeout <= 0 or time.time() < deadline:
        raw = port.readline()
        if not raw:
            continue
        try:
            payload = json.loads(raw.decode("utf-8", errors="ignore"))
        except json.JSONDecodeError:
            continue
        if payload.get("event") == "ready":
            print("[demo] device reported ready")
            return
    raise TimeoutError("timed out waiting for ready event")


def send_command(port: serial.Serial, payload: dict, *, listen: float = 0.3, label: Optional[str] = None) -> None:
    serialized = json.dumps(payload, separators=(",", ":"))
    port.write(serialized.encode("utf-8") + b"\n")
    port.flush()
    if label:
        print(f"[demo] {label}")
    print(f"[demo] -> {serialized}")

    deadline = time.time() + max(listen, 0)
    while listen > 0 and time.time() < deadline:
        raw = port.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").rstrip()
        if text:
            print(f"[demo] <- {text}")


def drip_type(port: serial.Serial, text: str, *, newline: bool = False, char_delay_ms: int = DEFAULT_CHAR_DELAY_MS) -> None:
    payload = {
        "device": "keyboard",
        "action": "write",
        "text": text,
        "newline": newline,
        "charDelayMs": char_delay_ms,
    }
    send_command(port, payload, label=f"type '{text}'")


def tap_keys(port: serial.Serial, *keys: str, hold_ms: int = 80) -> None:
    payload = {
        "device": "keyboard",
        "action": "tap",
        "keys": list(keys),
        "holdMs": hold_ms,
    }
    send_command(port, payload, label=f"tap {'+'.join(keys)}")


def consumer_key(port: serial.Serial, key: str) -> None:
    payload = {
        "device": "consumer",
        "keys": [key],
    }
    send_command(port, payload, label=f"consumer key {key}")


def mouse_move(
    port: serial.Serial,
    dx: int = 0,
    dy: int = 0,
    wheel: int = 0,
    pan: int = 0,
    *,
    label: Optional[str] = None,
    listen: float = 0.1,
) -> None:
    payload = {
        "device": "mouse",
        "action": "move",
        "dx": dx,
        "dy": dy,
        "wheel": wheel,
        "pan": pan,
    }
    send_command(port, payload, label=label, listen=listen)


def mouse_click(
    port: serial.Serial,
    *buttons: str,
    label: Optional[str] = None,
    listen: float = 0.2,
) -> None:
    payload = {
        "device": "mouse",
        "action": "click",
        "buttons": list(buttons) if buttons else None,
    }
    send_command(
        port,
        payload,
        label=label or f"mouse click {','.join(buttons) if buttons else 'left'}",
        listen=listen,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run a canned keyboard/mouse demo via the UART JSON protocol.")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="serial port path (default: %(default)s)")
    parser.add_argument("--baud", type=int, default=115200, help="baud rate (default: %(default)s)")
    parser.add_argument("--wait-ready", type=float, default=3.0, help="seconds to wait for ready event (0 to skip)")
    parser.add_argument("--char-delay", type=int, default=DEFAULT_CHAR_DELAY_MS, help="delay in milliseconds between characters")
    parser.add_argument("--mouse-distance", type=int, default=DEFAULT_MOUSE_STEP, help="total pixels to travel on each side of the box")
    parser.add_argument("--mouse-step", type=int, default=12, help="pixels per incremental mouse move")
    parser.add_argument("--mouse-step-delay", type=float, default=0.15, help="pause between mouse movement commands (seconds)")
    parser.add_argument("--mouse-listen", type=float, default=0.05, help="seconds to listen for responses after each mouse command")
    parser.add_argument("--dry-run", action="store_true", help="print the sequence without sending anything")
    return parser


def open_serial(port: str, baud: int) -> serial.Serial:
    return serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.1,
    )


def move_axis(
    port: serial.Serial,
    *,
    axis: str,
    total: int,
    step: int,
    label: str,
    listen: float,
) -> None:
    remaining = total
    direction = 1 if total >= 0 else -1
    step = abs(step)
    while remaining != 0:
        delta = min(step, abs(remaining))
        if axis == "x":
            mouse_move(
                port,
                dx=direction * delta,
                label=f"{label} ({direction * delta})",
                listen=listen,
            )
        else:
            mouse_move(
                port,
                dy=direction * delta,
                label=f"{label} ({direction * delta})",
                listen=listen,
            )
        remaining -= direction * delta


def move_box(
    port: serial.Serial,
    total_step: int,
    step_size: int,
    pause: float,
    listen: float,
) -> None:
    move_axis(port, axis="y", total=-total_step, step=step_size, label="move mouse up", listen=listen)
    time.sleep(pause)
    move_axis(port, axis="x", total=-total_step, step=step_size, label="move mouse left", listen=listen)
    time.sleep(pause)
    move_axis(port, axis="y", total=total_step, step=step_size, label="move mouse down", listen=listen)
    time.sleep(pause)
    move_axis(port, axis="x", total=total_step, step=step_size, label="move mouse right", listen=listen)
    time.sleep(pause)


def mouse_click_sequence(port: serial.Serial, step_delay: float, listen: float) -> None:
    mouse_move(port, dx=40, dy=40, label="position pointer for clicks", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "left", label="left click", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "right", label="right click", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "middle", label="scroll wheel click", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "back", label="back button click", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "forward", label="forward button click", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "left", "right", label="click left+right simultaneously", listen=listen)
    time.sleep(step_delay)
    mouse_click(port, "left", "right", "middle", label="click left+right+middle simultaneously", listen=listen)
    time.sleep(step_delay)


def run_sequence(port: serial.Serial, args: argparse.Namespace) -> None:
    drip_type(port, "Hello from UART demo!", newline=True, char_delay_ms=args.char_delay)
    time.sleep(0.3)
    tap_keys(port, "ENTER")
    time.sleep(0.3)
    consumer_key(port, "KEY_MEDIA_PLAY_PAUSE")
    time.sleep(0.3)
    tap_keys(port, "CTRL", "ALT", "DELETE", hold_ms=120)

    time.sleep(0.5)
    print("[demo] starting mouse movement pattern")
    move_box(
        port,
        total_step=args.mouse_distance,
        step_size=max(1, args.mouse_step),
        pause=args.mouse_step_delay,
        listen=args.mouse_listen,
    )
    mouse_move(port, wheel=-2, label="scroll down", listen=args.mouse_listen)
    time.sleep(args.mouse_step_delay)
    mouse_click_sequence(port, args.mouse_step_delay, args.mouse_listen)

    print("[demo] sequence complete")


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.dry_run:
        print("[demo] dry run: commands will not be sent\n")

    try:
        ser = open_serial(args.port, args.baud)
    except serial.SerialException as exc:
        print(f"[demo] failed to open {args.port}: {exc}", file=sys.stderr)
        return 2

    if args.wait_ready > 0:
        try:
            wait_for_ready(ser, args.wait_ready)
        except TimeoutError as exc:
            print(f"[demo] warning: {exc}", file=sys.stderr)

    if args.dry_run:
        print("[demo] would run demo sequence now.")
        ser.close()
        return 0

    try:
        run_sequence(ser, args)
    finally:
        ser.close()
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
