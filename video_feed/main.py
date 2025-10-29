"""FastAPI entry point for the uxplay ➜ OpenCV ➜ WebRTC bridge."""

from __future__ import annotations

import asyncio
import contextlib
from pathlib import Path
from typing import Optional

from aiortc import RTCSessionDescription
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from pydantic import BaseModel

from .pipeline import (
    OpenCVVideoSource,
    PeerConnectionPool,
    build_uxplay_runner_from_env,
    build_video_source_from_env,
    create_answer,
    load_automation_hook,
    should_autostart_uxplay,
)


BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
INDEX_FILE = STATIC_DIR / "index.html"

app = FastAPI(title="iOS Video Automation Bridge")

pcs = PeerConnectionPool()
automation_hook = load_automation_hook()
video_config = build_video_source_from_env()
video_source = OpenCVVideoSource(video_config)
uxplay_runner = build_uxplay_runner_from_env()
uxplay_log_task: Optional[asyncio.Task[None]] = None


class SDPMessage(BaseModel):
    sdp: str
    type: str


@app.on_event("startup")
async def on_startup() -> None:
    global uxplay_log_task

    if should_autostart_uxplay():
        uxplay_runner.start()
        uxplay_log_task = asyncio.create_task(uxplay_runner.stream_logs())

    try:
        video_source.open()
    except RuntimeError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc


@app.on_event("shutdown")
async def on_shutdown() -> None:
    global uxplay_log_task

    await pcs.close()
    video_source.close()
    if uxplay_log_task:
        uxplay_log_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await uxplay_log_task
        uxplay_log_task = None
    uxplay_runner.stop()


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(INDEX_FILE)


@app.post("/offer")
async def offer(sdp: SDPMessage) -> dict[str, str]:
    try:
        offer = RTCSessionDescription(sdp=sdp.sdp, type=sdp.type)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=f"Invalid SDP: {exc}") from exc

    try:
        answer = await create_answer(
            offer=offer,
            source=video_source,
            pcs=pcs,
            automation_hook=automation_hook,
            fps=video_config.fps,
        )
    except Exception as exc:  # pragma: no cover - for runtime diagnostics
        raise HTTPException(status_code=500, detail=f"Failed to negotiate WebRTC: {exc}") from exc

    return {"sdp": answer.sdp, "type": answer.type}


# The FastAPI app is intentionally lightweight; mount it under an existing server
# or run ``uvicorn video_feed.main:app`` for local development.
