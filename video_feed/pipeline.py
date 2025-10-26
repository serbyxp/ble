"""Building blocks for the uxplay ➜ OpenCV ➜ WebRTC pipeline."""

from __future__ import annotations

import asyncio
import importlib
import os
import shlex
import signal
import subprocess
import threading
from dataclasses import dataclass
from typing import Callable, Optional

import cv2  # type: ignore
import numpy as np
from av import VideoFrame  # type: ignore
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import VideoStreamTrack


AutomationHook = Callable[[np.ndarray], np.ndarray]


def load_automation_hook() -> Optional[AutomationHook]:
    """Return a callable that processes frames before they hit WebRTC."""

    spec = os.getenv("AUTOMATION_MODULE")
    if not spec:
        return None

    if ":" in spec:
        module_name, attr = spec.split(":", 1)
    else:
        module_name, attr = spec, "process"

    module = importlib.import_module(module_name)
    hook = getattr(module, attr)
    if not callable(hook):
        raise TypeError(f"Automation hook {spec!r} is not callable")
    return hook  # type: ignore[return-value]


@dataclass
class VideoSourceConfig:
    url: str
    width: Optional[int] = None
    height: Optional[int] = None
    fps: float = 30.0


class OpenCVVideoSource:
    """Thin wrapper around ``cv2.VideoCapture`` with async helpers."""

    def __init__(self, config: VideoSourceConfig) -> None:
        self.config = config
        self._cap = cv2.VideoCapture()
        self._lock = threading.Lock()
        self._opened = False

    def open(self) -> None:
        with self._lock:
            if self._opened:
                return
            if not self._cap.open(self.config.url):
                raise RuntimeError(f"Unable to open video source {self.config.url}")
            if self.config.width:
                self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, float(self.config.width))
            if self.config.height:
                self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, float(self.config.height))
            if self.config.fps:
                self._cap.set(cv2.CAP_PROP_FPS, float(self.config.fps))
            # Try to minimize buffering latency when the backend supports it.
            self._cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            self._opened = True

    def close(self) -> None:
        with self._lock:
            if self._opened:
                self._cap.release()
                self._opened = False

    def _read_frame(self) -> np.ndarray:
        if not self._opened:
            raise RuntimeError("Video source not opened")
        ok, frame = self._cap.read()
        if not ok or frame is None:
            raise RuntimeError("Failed to read frame from source")
        return frame

    async def read(self) -> np.ndarray:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, self._read_frame)


class AutomationVideoTrack(VideoStreamTrack):
    """WebRTC video track that pulls frames from an OpenCV source."""

    def __init__(
        self,
        source: OpenCVVideoSource,
        fps: float,
        automation_hook: Optional[AutomationHook] = None,
    ) -> None:
        super().__init__()
        self.source = source
        self.automation_hook = automation_hook
        self._frame_delay = 1.0 / max(fps, 1.0)

    async def recv(self) -> VideoFrame:
        frame = await self.source.read()
        if self.automation_hook is not None:
            frame = self.automation_hook(frame)

        video_frame = VideoFrame.from_ndarray(frame, format="bgr24")
        video_frame.pts, video_frame.time_base = await self.next_timestamp()
        if self._frame_delay:
            await asyncio.sleep(self._frame_delay)
        return video_frame


class PeerConnectionPool:
    """Book-keeping for active WebRTC peer connections."""

    def __init__(self) -> None:
        self._pcs: set[RTCPeerConnection] = set()

    def add(self, pc: RTCPeerConnection) -> None:
        self._pcs.add(pc)

    def discard(self, pc: RTCPeerConnection) -> None:
        self._pcs.discard(pc)

    async def close(self) -> None:
        await asyncio.gather(*(pc.close() for pc in list(self._pcs)), return_exceptions=True)
        self._pcs.clear()


class UxPlayRunner:
    """Manage a background ``uxplay`` process."""

    def __init__(self, command: Optional[str], shutdown_timeout: float = 5.0) -> None:
        self.command = command
        self.shutdown_timeout = shutdown_timeout
        self._process: Optional[subprocess.Popen[str]] = None

    def start(self) -> None:
        if not self.command:
            return
        if self._process and self._process.poll() is None:
            return
        args = shlex.split(self.command)
        self._process = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def stop(self) -> None:
        if not self._process:
            return
        if self._process.poll() is not None:
            self._process = None
            return

        try:
            self._process.send_signal(signal.SIGTERM)
        except ProcessLookupError:
            self._process = None
            return

        try:
            self._process.wait(timeout=self.shutdown_timeout)
        except subprocess.TimeoutExpired:
            self._process.kill()
        finally:
            self._process = None

    async def stream_logs(self) -> None:
        if not self._process or not self._process.stdout:
            return
        loop = asyncio.get_running_loop()
        reader = asyncio.StreamReader()
        protocol = asyncio.StreamReaderProtocol(reader)
        await loop.connect_read_pipe(lambda: protocol, self._process.stdout)
        while True:
            line = await reader.readline()
            if not line:
                break
            print(f"[uxplay] {line.decode().rstrip()}")


async def create_answer(
    offer: RTCSessionDescription,
    source: OpenCVVideoSource,
    pcs: PeerConnectionPool,
    automation_hook: Optional[AutomationHook],
    fps: float,
) -> RTCSessionDescription:
    pc = RTCPeerConnection()
    pcs.add(pc)

    track = AutomationVideoTrack(source=source, fps=fps, automation_hook=automation_hook)
    pc.addTrack(track)

    @pc.on("connectionstatechange")
    async def on_connection_state_change() -> None:
        if pc.connectionState in ("failed", "closed", "disconnected"):
            await pc.close()
            pcs.discard(pc)

    await pc.setRemoteDescription(offer)
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    return pc.localDescription  # type: ignore[return-value]


def build_video_source_from_env() -> VideoSourceConfig:
    url = os.getenv("VIDEO_SOURCE", "udp://127.0.0.1:11000")
    width = os.getenv("VIDEO_WIDTH")
    height = os.getenv("VIDEO_HEIGHT")
    fps = float(os.getenv("VIDEO_FPS", "30"))
    return VideoSourceConfig(
        url=url,
        width=int(width) if width else None,
        height=int(height) if height else None,
        fps=fps,
    )


def build_uxplay_runner_from_env() -> UxPlayRunner:
    command = os.getenv("UXPLAY_COMMAND")
    timeout = float(os.getenv("UXPLAY_SHUTDOWN_TIMEOUT", "5"))
    return UxPlayRunner(command=command, shutdown_timeout=timeout)


def should_autostart_uxplay() -> bool:
    return os.getenv("UXPLAY_AUTOSTART", "1") not in {"0", "false", "False"}


__all__ = [
    "AutomationVideoTrack",
    "OpenCVVideoSource",
    "PeerConnectionPool",
    "UxPlayRunner",
    "VideoSourceConfig",
    "create_answer",
    "load_automation_hook",
    "build_video_source_from_env",
    "build_uxplay_runner_from_env",
    "should_autostart_uxplay",
]
