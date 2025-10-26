# WebRTC Video Bridge

This module turns the UDP screen capture stream from the ESP32 automation rig into a WebRTC video that the control UI can play without relying on an MJPEG HTTP relay. It uses a GStreamer pipeline to split ("tee") the decoded frames so that existing OpenCV automation keeps receiving raw RGBA pixels, while the untouched H.264 elementary stream is repackaged for browser delivery via `webrtcbin`.

## Runtime requirements

The bridge is written in Python but relies on system GStreamer packages:

- **PyGObject** with GStreamer introspection bindings (`gi.repository.Gst`, `GstWebRTC`).
- **GStreamer plugins** that provide `rtph264depay`, `avdec_h264`, `rtph264pay`, and `webrtcbin`. On Debian/Ubuntu these are typically available in the `gstreamer1.0-libav`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`, and `gstreamer1.0-plugins-base` packages.
- An upstream UDP H.264 stream (default: `0.0.0.0:5700`, payload type 96, 90 kHz clock).

The Python side has no additional third-party dependencies beyond what the main FastAPI application already uses. NumPy/OpenCV remain optional; the `FrameBuffer` simply stores the raw RGBA bytes so automation code can decide how to decode them.

## How it works

1. `udpsrc` ingests the RTP/H.264 feed from the capture hardware.
2. `rtpjitterbuffer` smooths out packet timing, followed by `rtph264depay` and `h264parse`.
3. A `tee` splits the elementary H.264 stream:
   - **Automation branch** – `avdec_h264 ! videoconvert` produces RGBA frames, which are pushed into the shared `FrameBuffer` via an `appsink`.
   - **Browser branch** – `h264parse ! rtph264pay` re-wraps the bitstream before it feeds `webrtcbin`.
4. `webrtcbin` handles DTLS/SRTP encryption and exposes the stream to any browser peer that negotiates a session through the FastAPI signalling routes.

The `BridgeApp` class registers three HTTP endpoints:

- `POST /video/offer` – accepts the browser SDP offer and responds with the bridge's SDP answer.
- `POST /video/ice` – ingests remote ICE candidates from the browser.
- `GET /video/ice` – returns any server-side ICE candidates collected from `webrtcbin`.
- `GET /video/config` – returns ICE server details derived from `VIDEO_STUN_SERVER` for the browser peer.

These routes replace the older `/stream.mjpg` MJPEG feed and are automatically mounted by `web/server.py`.

## Browser workflow

1. The control UI creates an `RTCPeerConnection` and a recv-only video transceiver.
2. The UI posts its SDP offer to `/video/offer` and applies the returned SDP answer.
3. Both sides exchange ICE candidates: the browser calls `/video/ice` for each local candidate, while polling `GET /video/ice` to collect bridge candidates.
4. Once ICE/DTLS complete, the `<video>` element begins rendering the WebRTC stream. The viewer overlay, pointer-lock tooling, and HID controls continue to work exactly as before.

## Configuration

Environment variables tune the pipeline without code changes:

- `VIDEO_UDP_HOST` (default `0.0.0.0`) – UDP interface to bind the `udpsrc` element.
- `VIDEO_UDP_PORT` (default `5700`) – UDP port that receives the RTP stream.
- `VIDEO_RTP_CAPS` – optional override for the source caps if the payload type or codec differs.
- `VIDEO_STUN_SERVER` – optional STUN URI (`stun://host:port`) forwarded to `webrtcbin` for NAT traversal.

On shutdown the FastAPI application stops the GStreamer pipeline cleanly.

## Usage

1. Install the system dependencies and ensure the UDP H.264 feed is active.
2. Launch the FastAPI server (`uvicorn web.server:app --reload`).
3. Open the control UI in a browser and click **Start WebRTC Stream** in the Remote Viewer panel.
4. The page negotiates a WebRTC session using the new signalling endpoints and starts playback once ICE succeeds.

The shared `FrameBuffer` can be imported from `web.video_feed.bridge` by automation scripts that need access to the latest RGBA frame.
