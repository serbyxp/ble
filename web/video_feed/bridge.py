"""WebRTC video bridge for the ESP32 BLE automation toolkit."""

from __future__ import annotations

import asyncio
import os
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Deque, Dict, List, Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

try:
    import gi  # type: ignore

    gi.require_version("Gst", "1.0")
    gi.require_version("GstWebRTC", "1.0")
    from gi.repository import GLib, Gst, GstWebRTC  # type: ignore
except (ImportError, ValueError) as exc:  # pragma: no cover - runtime dependency
    raise RuntimeError(
        "PyGObject with GStreamer support is required for the video bridge"
    ) from exc

Gst.init(None)


@dataclass
class FrameInfo:
    """Metadata for the most recent decoded frame."""

    width: int
    height: int
    format: str
    timestamp: float


class FrameBuffer:
    """Thread-safe container for the most recent RGBA frame."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._frame: Optional[bytes] = None
        self._info: Optional[FrameInfo] = None

    def update(self, frame: bytes, width: int, height: int, pixel_format: str) -> None:
        with self._lock:
            self._frame = bytes(frame)
            self._info = FrameInfo(
                width=width,
                height=height,
                format=pixel_format,
                timestamp=time.time(),
            )

    def snapshot(self, copy: bool = True) -> Optional[tuple[bytes, FrameInfo]]:
        with self._lock:
            if self._frame is None or self._info is None:
                return None
            data = bytes(self._frame) if copy else self._frame
            return data, self._info


class _WebRTCBroadcaster:
    """Wraps a GStreamer pipeline that forwards the UDP stream to WebRTC."""

    def __init__(
        self,
        *,
        udp_host: str,
        udp_port: int,
        rtp_caps: str,
        frame_buffer: FrameBuffer,
        stun_server: Optional[str] = None,
    ) -> None:
        self.udp_host = udp_host
        self.udp_port = udp_port
        self.rtp_caps = rtp_caps
        self.frame_buffer = frame_buffer
        self.stun_server = stun_server

        self._pipeline: Optional[Gst.Pipeline] = None
        self._webrtc: Optional[Gst.Element] = None
        self._appsink: Optional[Gst.Element] = None
        self._loop: Optional[GLib.MainLoop] = None
        self._loop_thread: Optional[threading.Thread] = None
        self._pending_ice: Deque[Dict[str, object]] = deque()
        self._pending_lock = threading.Lock()
        self._started = threading.Event()

    # ------------------------------------------------------------------
    # Lifecycle management
    # ------------------------------------------------------------------
    def start(self) -> None:
        if self._started.is_set():
            return
        with self._pending_lock:
            self._pending_ice.clear()
        self._build_pipeline()
        self._loop = GLib.MainLoop()
        self._loop_thread = threading.Thread(target=self._loop.run, daemon=True)
        self._loop_thread.start()
        while self._loop and not self._loop.is_running():  # pragma: no cover - startup sync
            time.sleep(0.01)
        self._invoke(self._set_pipeline_state, Gst.State.PLAYING)
        self._started.set()

    def stop(self) -> None:
        if not self._started.is_set():
            return
        self._invoke(self._set_pipeline_state, Gst.State.NULL)
        if self._loop and self._loop.is_running():
            self._loop.quit()
        if self._loop_thread:
            self._loop_thread.join(timeout=2.0)
        self._loop = None
        self._loop_thread = None
        with self._pending_lock:
            self._pending_ice.clear()
        self._pipeline = None
        self._webrtc = None
        self._appsink = None
        self._started.clear()

    # ------------------------------------------------------------------
    # Signalling helpers
    # ------------------------------------------------------------------
    async def create_answer(self, offer_sdp: str) -> str:
        if not self._started.is_set():
            raise RuntimeError("Video bridge is not ready")
        return await asyncio.to_thread(self._handle_offer_sync, offer_sdp)

    async def add_ice_candidate(
        self, *, candidate: str, sdp_mid: Optional[str], sdp_mline_index: int
    ) -> None:
        if not self._started.is_set():
            raise RuntimeError("Video bridge is not ready")
        await asyncio.to_thread(
            self._invoke,
            self._add_remote_candidate,
            sdp_mline_index,
            candidate,
        )

    def drain_local_candidates(self) -> List[Dict[str, object]]:
        if not self._started.is_set():
            return []
        with self._pending_lock:
            items = list(self._pending_ice)
            self._pending_ice.clear()
        return items

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------
    def _build_pipeline(self) -> None:
        pipeline = Gst.Pipeline.new("bridge-pipeline")

        udpsrc = Gst.ElementFactory.make("udpsrc", "source")
        udpsrc.set_property("address", self.udp_host)
        udpsrc.set_property("port", self.udp_port)
        udpsrc.set_property("caps", Gst.Caps.from_string(self.rtp_caps))

        jitter = Gst.ElementFactory.make("rtpjitterbuffer", "jitter")
        depay = Gst.ElementFactory.make("rtph264depay", "depay")
        parser = Gst.ElementFactory.make("h264parse", "parser")
        parser.set_property("config-interval", -1)
        tee = Gst.ElementFactory.make("tee", "tee")

        decode_queue = Gst.ElementFactory.make("queue", "decode-queue")
        decoder = Gst.ElementFactory.make("avdec_h264", "decoder")
        convert = Gst.ElementFactory.make("videoconvert", "convert")
        capsfilter = Gst.ElementFactory.make("capsfilter", "frame-caps")
        capsfilter.set_property(
            "caps", Gst.Caps.from_string("video/x-raw,format=RGBA")
        )
        appsink = Gst.ElementFactory.make("appsink", "frame-sink")
        appsink.set_property("emit-signals", True)
        appsink.set_property("drop", True)
        appsink.set_property("max-buffers", 1)
        appsink.set_property("sync", False)

        webrtc_queue = Gst.ElementFactory.make("queue", "webrtc-queue")
        webrtc_parse = Gst.ElementFactory.make("h264parse", "webrtc-parse")
        webrtc_parse.set_property("config-interval", 1)
        pay = Gst.ElementFactory.make("rtph264pay", "pay")
        pay.set_property("pt", 96)
        pay.set_property("config-interval", 1)
        webrtcbin = Gst.ElementFactory.make("webrtcbin", "webrtcbin")
        if self.stun_server:
            webrtcbin.set_property("stun-server", self.stun_server)
        webrtcbin.set_property("bundle-policy", 2)  # max-bundle

        elements = [
            udpsrc,
            jitter,
            depay,
            parser,
            tee,
            decode_queue,
            decoder,
            convert,
            capsfilter,
            appsink,
            webrtc_queue,
            webrtc_parse,
            pay,
            webrtcbin,
        ]

        for element in elements:
            if element is None:
                raise RuntimeError("Failed to create required GStreamer element")
            pipeline.add(element)

        if not Gst.Element.link_many(udpsrc, jitter, depay, parser, tee):
            raise RuntimeError("Failed to link primary pipeline")
        if not Gst.Element.link_many(decode_queue, decoder, convert, capsfilter, appsink):
            raise RuntimeError("Failed to link decode branch")
        if not Gst.Element.link_many(webrtc_queue, webrtc_parse, pay):
            raise RuntimeError("Failed to link WebRTC branch")

        tee_decode_pad = tee.get_request_pad("src_%u")
        queue_decode_sink = decode_queue.get_static_pad("sink")
        if tee_decode_pad is None or queue_decode_sink is None:
            raise RuntimeError("Failed to acquire decode tee pads")
        tee_decode_pad.link(queue_decode_sink)

        tee_webrtc_pad = tee.get_request_pad("src_%u")
        queue_webrtc_sink = webrtc_queue.get_static_pad("sink")
        if tee_webrtc_pad is None or queue_webrtc_sink is None:
            raise RuntimeError("Failed to acquire WebRTC tee pads")
        tee_webrtc_pad.link(queue_webrtc_sink)

        pay_src = pay.get_static_pad("src")
        webrtc_sink = webrtcbin.get_request_pad("sink_%u")
        if pay_src is None or webrtc_sink is None:
            raise RuntimeError("Failed to link payloader to WebRTC")
        pay_src.link(webrtc_sink)

        appsink.connect("new-sample", self._on_new_sample)
        webrtcbin.connect("on-ice-candidate", self._on_ice_candidate)

        self._pipeline = pipeline
        self._webrtc = webrtcbin
        self._appsink = appsink

    # ------------------------------------------------------------------
    def _set_pipeline_state(self, state: Gst.State) -> None:
        if self._pipeline is None:
            return
        self._pipeline.set_state(state)

    def _handle_offer_sync(self, offer_sdp: str) -> str:
        def _process() -> str:
            if self._webrtc is None:
                raise RuntimeError("WebRTC pipeline is not initialised")

            result, sdp_message = Gst.SDPMessage.new_from_text(offer_sdp)
            if result != Gst.SDPResult.OK:
                raise ValueError("Invalid SDP offer")

            offer = GstWebRTC.WebRTCSessionDescription.new(
                GstWebRTC.WebRTCSDPType.OFFER, sdp_message
            )

            set_remote_promise = Gst.Promise.new()
            self._webrtc.emit("set-remote-description", offer, set_remote_promise)
            set_remote_promise.wait()

            create_promise = Gst.Promise.new()
            self._webrtc.emit("create-answer", None, create_promise)
            create_promise.wait()
            reply = create_promise.get_reply()
            answer = reply.get_value("answer")

            set_local_promise = Gst.Promise.new()
            self._webrtc.emit("set-local-description", answer, set_local_promise)
            set_local_promise.wait()

            return answer.sdp.as_text()

        return self._invoke(_process)

    def _add_remote_candidate(self, mline_index: int, candidate: str) -> None:
        if self._webrtc is None:
            raise RuntimeError("WebRTC pipeline is not initialised")
        self._webrtc.emit("add-ice-candidate", mline_index, candidate)

    def _on_new_sample(self, sink: Gst.Element) -> Gst.FlowReturn:
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR
        buffer = sample.get_buffer()
        caps = sample.get_caps()
        structure = caps.get_structure(0)
        width = structure.get_value("width")
        height = structure.get_value("height")
        success, map_info = buffer.map(Gst.MapFlags.READ)
        if not success:
            return Gst.FlowReturn.ERROR
        try:
            data = map_info.data
            self.frame_buffer.update(bytes(data), width, height, "RGBA")
        finally:
            buffer.unmap(map_info)
        return Gst.FlowReturn.OK

    def _on_ice_candidate(self, webrtcbin: Gst.Element, mlineindex: int, candidate: str) -> None:
        payload: Dict[str, object] = {
            "candidate": candidate,
            "sdpMid": "video",
            "sdpMLineIndex": int(mlineindex),
        }
        with self._pending_lock:
            self._pending_ice.append(payload)

    def _invoke(self, func, *args):
        result: Dict[str, object] = {}
        event = threading.Event()

        def _wrapper():
            try:
                result["value"] = func(*args)
            except Exception as exc:  # pragma: no cover - pass-through
                result["error"] = exc
            finally:
                event.set()
            return False

        GLib.idle_add(_wrapper)
        event.wait()
        if "error" in result:
            raise result["error"]
        return result.get("value")


class OfferPayload(BaseModel):
    sdp: str = Field(..., description="Base64-free SDP offer from the browser")


class IcePayload(BaseModel):
    candidate: str = Field(..., description="ICE candidate string")
    sdpMid: Optional[str] = Field(None, description="Media identifier")
    sdpMLineIndex: int = Field(..., ge=0, description="SDP m-line index")


class BridgeApp:
    """Expose WebRTC signalling endpoints for the automation UI."""

    def __init__(
        self,
        *,
        frame_buffer: FrameBuffer,
        udp_host: Optional[str] = None,
        udp_port: Optional[int] = None,
        rtp_caps: Optional[str] = None,
        stun_server: Optional[str] = None,
        prefix: str = "/video",
    ) -> None:
        udp_host = udp_host or os.getenv("VIDEO_UDP_HOST", "0.0.0.0")
        udp_port = udp_port or int(os.getenv("VIDEO_UDP_PORT", "5700"))
        rtp_caps = rtp_caps or os.getenv(
            "VIDEO_RTP_CAPS",
            "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000",
        )
        stun_server = stun_server or os.getenv("VIDEO_STUN_SERVER")

        self.router = APIRouter(prefix=prefix, tags=["video"])
        self._broadcaster = _WebRTCBroadcaster(
            udp_host=udp_host,
            udp_port=udp_port,
            rtp_caps=rtp_caps,
            frame_buffer=frame_buffer,
            stun_server=stun_server,
        )
        self._stun_server = stun_server
        self._setup_routes()

    # ------------------------------------------------------------------
    def attach(self, app) -> None:
        app.include_router(self.router)
        app.add_event_handler("startup", self._broadcaster.start)
        app.add_event_handler("shutdown", self._broadcaster.stop)

    # ------------------------------------------------------------------
    def _setup_routes(self) -> None:
        @self.router.post("/offer")
        async def handle_offer(payload: OfferPayload) -> Dict[str, str]:
            try:
                answer_sdp = await self._broadcaster.create_answer(payload.sdp)
            except Exception as exc:  # pragma: no cover - propagate as HTTP error
                raise HTTPException(status_code=400, detail=str(exc)) from exc
            return {"type": "answer", "sdp": answer_sdp}

        @self.router.post("/ice")
        async def post_ice(payload: IcePayload) -> Dict[str, str]:
            try:
                await self._broadcaster.add_ice_candidate(
                    candidate=payload.candidate,
                    sdp_mid=payload.sdpMid,
                    sdp_mline_index=payload.sdpMLineIndex,
                )
            except Exception as exc:  # pragma: no cover - propagate as HTTP error
                raise HTTPException(status_code=400, detail=str(exc)) from exc
            return {"status": "ok"}

        @self.router.get("/ice")
        async def get_ice() -> Dict[str, List[Dict[str, object]]]:
            candidates = self._broadcaster.drain_local_candidates()
            return {"candidates": candidates}

        @self.router.get("/config")
        async def get_config() -> Dict[str, object]:
            ice_servers: List[Dict[str, str]] = []
            if self._stun_server:
                ice_servers.append({"urls": self._stun_server})
            return {"iceServers": ice_servers}


__all__ = ["BridgeApp", "FrameBuffer", "FrameInfo"]
