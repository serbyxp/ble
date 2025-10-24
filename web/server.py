#!/usr/bin/env python3
"""
FastAPI control surface for the ESP32 BLE HID bridge.

The API exposes endpoints that mirror the UART JSON commands used by the
firmware. A lightweight web UI (served from /) lets you type text, trigger
key combinations, control media keys, and move/click the mouse.

Launch with:
    uvicorn web.server:app --reload

Before starting the server set UART_PORT (e.g. COM3) and UART_BAUD as needed,
or use the /api/config endpoint / Connect UI form to switch ports at runtime.
"""

from __future__ import annotations

import json
import asyncio
import os
import threading
import time
from pathlib import Path
from typing import List, Literal, Optional

import serial  # type: ignore
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

DEFAULT_PORT = os.getenv("UART_PORT", "COM3")
DEFAULT_BAUD = int(os.getenv("UART_BAUD", "115200"))
DEFAULT_LISTEN_SECONDS = float(os.getenv("UART_LISTEN_SECONDS", "0.5"))

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
INDEX_FILE = STATIC_DIR / "index.html"


def _normalize_list(value, default=None):
    if value is None:
        return default
    if isinstance(value, str):
        return [value]
    return list(value)


class SerialBridge:
    def __init__(self, port: str, baud: int) -> None:
        self._lock = threading.Lock()
        self._serial: Optional[serial.Serial] = None
        self.port = port
        self.baud = baud
        self.timeout = 0.1

    def connect(self, port: Optional[str] = None, baud: Optional[int] = None) -> None:
        port = port or self.port
        baud = baud or self.baud

        try:
            ser = serial.Serial(port=port, baudrate=baud, timeout=self.timeout)
        except serial.SerialException as exc:
            raise HTTPException(
                status_code=400, detail=f"Unable to open serial port {port}: {exc}"
            ) from exc

        with self._lock:
            if self._serial and self._serial.is_open:
                self._serial.close()
            self._serial = ser
            self.port = port
            self.baud = baud

    def ensure_connection(self) -> None:
        with self._lock:
            if not self._serial or not self._serial.is_open:
                self.connect(self.port, self.baud)

    def send(self, payload: dict, listen: float = DEFAULT_LISTEN_SECONDS) -> List[str]:
        self.ensure_connection()
        serialized = json.dumps(payload, separators=(",", ":"))
        responses: List[str] = []

        with self._lock:
            assert self._serial is not None  # for type checkers
            self._serial.reset_input_buffer()
            self._serial.write(serialized.encode("utf-8") + b"\n")
            self._serial.flush()

            deadline = time.time() + max(listen, 0)
            while listen > 0 and time.time() < deadline:
                raw = self._serial.readline()
                if not raw:
                    continue
                try:
                    text = raw.decode("utf-8", errors="replace").rstrip()
                except UnicodeDecodeError:
                    text = raw.hex()
                if text:
                    responses.append(text)

        return responses

    async def send_async(
        self, payload: dict, listen: float = DEFAULT_LISTEN_SECONDS
    ) -> List[str]:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, self.send, payload, listen)

    def close(self) -> None:
        with self._lock:
            if self._serial and self._serial.is_open:
                self._serial.close()
            self._serial = None


bridge = SerialBridge(port=DEFAULT_PORT, baud=DEFAULT_BAUD)


class ListenMixin(BaseModel):
    listen: Optional[float] = Field(
        DEFAULT_LISTEN_SECONDS, description="Seconds to listen for UART responses"
    )


class TextPayload(ListenMixin):
    text: str
    newline: bool = False
    charDelayMs: int = Field(6, ge=0, le=1000)
    repeat: int = Field(1, ge=1, le=10)


class KeyComboPayload(ListenMixin):
    keys: List[str]
    holdMs: int = Field(80, ge=0, le=1000)


class RawKeyboardPayload(ListenMixin):
    action: str
    keys: Optional[List[str]] = None
    key: Optional[str] = None
    code: Optional[int] = None
    holdMs: Optional[int] = Field(None, ge=0, le=1000)
    repeat: Optional[int] = Field(None, ge=1, le=10)


class MousePayload(ListenMixin):
    action: Literal["move", "click", "press", "release", "releaseAll", "release_all"] = "move"
    dx: Optional[int] = 0
    dy: Optional[int] = 0
    wheel: Optional[int] = 0
    pan: Optional[int] = 0
    buttons: Optional[List[str]] = None


class MediaPayload(ListenMixin):
    key: str


class RawPayload(ListenMixin):
    payload: dict


class ConfigPayload(BaseModel):
    port: str
    baud: int = Field(DEFAULT_BAUD, ge=1200, le=921600)


app = FastAPI(title="UART HID Control")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.on_event("startup")
def startup() -> None:
    try:
        bridge.connect()
    except HTTPException as exc:
        # Delay connection issues until first API call so UI can adjust port.
        print(f"[server] Warning: {exc.detail}")


@app.on_event("shutdown")
def shutdown() -> None:
    bridge.close()


@app.get("/", include_in_schema=False)
def index() -> FileResponse:
    return FileResponse(INDEX_FILE)


@app.get("/api/config")
def get_config() -> dict:
    return {"port": bridge.port, "baud": bridge.baud}


@app.post("/api/config")
def set_config(payload: ConfigPayload) -> dict:
    bridge.connect(payload.port, payload.baud)
    return {"status": "ok", "port": bridge.port, "baud": bridge.baud}


@app.post("/api/text")
def send_text(payload: TextPayload) -> dict:
    uart_payload: dict[str, object] = {
        "device": "keyboard",
        "action": "write",
        "text": payload.text,
        "newline": payload.newline,
        "charDelayMs": payload.charDelayMs,
        "repeat": payload.repeat,
    }
    responses = bridge.send(uart_payload, listen=payload.listen or 0)
    return {"status": "ok", "sent": uart_payload, "responses": responses}


@app.post("/api/keyboard")
def send_keyboard(payload: RawKeyboardPayload) -> dict:
    uart_payload: dict[str, object] = {
        "device": "keyboard",
        "action": payload.action,
    }
    if payload.keys is not None:
        uart_payload["keys"] = payload.keys
    if payload.key is not None:
        uart_payload["key"] = payload.key
    if payload.code is not None:
        uart_payload["code"] = payload.code
    if payload.holdMs is not None:
        uart_payload["holdMs"] = payload.holdMs
    if payload.repeat is not None:
        uart_payload["repeat"] = payload.repeat

    responses = bridge.send(uart_payload, listen=payload.listen or 0)
    return {"status": "ok", "sent": uart_payload, "responses": responses}


@app.post("/api/mouse")
def send_mouse(payload: MousePayload) -> dict:
    uart_payload: dict[str, object] = {
        "device": "mouse",
        "action": payload.action,
    }
    if payload.action == "move":
        uart_payload["dx"] = payload.dx or 0
        uart_payload["dy"] = payload.dy or 0
        uart_payload["wheel"] = payload.wheel or 0
        uart_payload["pan"] = payload.pan or 0
    if payload.buttons:
        uart_payload["buttons"] = payload.buttons

    responses = bridge.send(uart_payload, listen=payload.listen or 0)
    return {"status": "ok", "sent": uart_payload, "responses": responses}


@app.post("/api/media")
def send_media(payload: MediaPayload) -> dict:
    uart_payload: dict[str, object] = {
        "device": "consumer",
        "keys": [payload.key],
    }
    responses = bridge.send(uart_payload, listen=payload.listen or 0)
    return {"status": "ok", "sent": uart_payload, "responses": responses}


@app.post("/api/raw")
def send_raw(payload: RawPayload) -> dict:
    responses = bridge.send(payload.payload, listen=payload.listen or 0)
    return {"status": "ok", "sent": payload.payload, "responses": responses}


@app.websocket("/ws/hid")
async def websocket_hid(websocket: WebSocket) -> None:
    await websocket.accept()
    await websocket.send_json(
        {"type": "hello", "status": "ok", "port": bridge.port, "baud": bridge.baud}
    )

    try:
        while True:
            data = await websocket.receive_json()
            msg_type = data.get("type")
            request_id = data.get("requestId")
            listen = max(float(data.get("listen", 0) or 0), 0.0)

            if not msg_type:
                if request_id is not None:
                    await websocket.send_json(
                        {
                            "status": "error",
                            "detail": "Missing message type",
                            "requestId": request_id,
                        }
                    )
                continue

            try:
                responses: List[str] = []

                if msg_type == "mouse_move":
                    payload = {
                        "device": "mouse",
                        "action": "move",
                        "dx": int(data.get("dx", 0) or 0),
                        "dy": int(data.get("dy", 0) or 0),
                        "wheel": int(data.get("wheel", 0) or 0),
                        "pan": int(data.get("pan", 0) or 0),
                    }
                    responses = await bridge.send_async(payload, listen)
                elif msg_type == "mouse_click":
                    buttons = _normalize_list(data.get("buttons"), ["left"])
                    payload = {
                        "device": "mouse",
                        "action": "click",
                        "buttons": buttons,
                    }
                    responses = await bridge.send_async(payload, listen)
                elif msg_type == "mouse_press":
                    buttons = _normalize_list(data.get("buttons"), ["left"])
                    payload = {
                        "device": "mouse",
                        "action": "press",
                        "buttons": buttons,
                    }
                    responses = await bridge.send_async(payload, listen)
                elif msg_type == "mouse_release":
                    buttons = _normalize_list(data.get("buttons"), ["left"])
                    payload = {
                        "device": "mouse",
                        "action": "release",
                        "buttons": buttons,
                    }
                    responses = await bridge.send_async(payload, listen)
                elif msg_type == "mouse_release_all":
                    payload = {"device": "mouse", "action": "releaseAll"}
                    responses = await bridge.send_async(payload, listen)
                elif msg_type == "keyboard_press":
                    keys = _normalize_list(data.get("keys"), [])
                    if keys:
                        payload = {
                            "device": "keyboard",
                            "action": "press",
                            "keys": keys,
                        }
                        responses = await bridge.send_async(payload, listen)
                elif msg_type == "keyboard_release":
                    keys = _normalize_list(data.get("keys"), [])
                    if keys:
                        payload = {
                            "device": "keyboard",
                            "action": "release",
                            "keys": keys,
                        }
                        responses = await bridge.send_async(payload, listen)
                elif msg_type == "keyboard_release_all":
                    payload = {"device": "keyboard", "action": "releaseAll"}
                    responses = await bridge.send_async(payload, listen)
                elif msg_type == "ping":
                    responses = []
                else:
                    raise ValueError(f"Unsupported message type: {msg_type}")

                if request_id is not None:
                    await websocket.send_json(
                        {
                            "status": "ok",
                            "type": msg_type,
                            "requestId": request_id,
                            "responses": responses,
                        }
                    )
            except HTTPException as exc:
                if request_id is not None:
                    await websocket.send_json(
                        {
                            "status": "error",
                            "type": msg_type,
                            "requestId": request_id,
                            "detail": exc.detail,
                        }
                    )
            except Exception as exc:  # pylint: disable=broad-except
                if request_id is not None:
                    await websocket.send_json(
                        {
                            "status": "error",
                            "type": msg_type,
                            "requestId": request_id,
                            "detail": str(exc),
                        }
                    )
    except WebSocketDisconnect:
        return
