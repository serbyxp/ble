# iOS Video Automation Pipeline

This module provides a self-contained pipeline to surface an iOS device
screen inside the browser while keeping the actual automation work in Python.
The data path is intentionally headless – the captured stream is processed with
OpenCV and forwarded to the browser over WebRTC for remote monitoring.

```
AirPlay (uxplay) ──▶ OpenCV automation ──▶ WebRTC preview
```

The scripts do **not** draw the video locally; all presentation happens in the
browser so that you can leave the automation stack running on a headless
machine.

## Features

* Manages an optional `uxplay` process that receives AirPlay mirroring from the
iOS device.
* Captures the `uxplay` stream with OpenCV so that you can run computer vision
logic to drive the device.
* Exposes the processed frames over WebRTC using FastAPI + `aiortc`, which lets
you monitor the automation session from any modern browser.
* Keeps the pipeline modular so you can plug in your own automation routines or
frame transforms.

## Project layout

```
video_feed/
├── main.py            # FastAPI application exposing the WebRTC endpoint
├── pipeline.py        # Reusable building blocks for uxplay + OpenCV sources
├── requirements.txt   # Python dependencies for the pipeline
├── README.md          # This file
└── static/
    └── index.html     # Minimal WebRTC receiver page for development
```

## Getting started

1. Install the Python dependencies into a virtual environment:

   ```bash
   python -m venv .venv
   source .venv/bin/activate
   pip install -r video_feed/requirements.txt
   ```

2. Ensure `uxplay` is installed on the host. The helper will try to spawn it
   using the command defined in `UXPLAY_COMMAND`. If you prefer to run `uxplay`
   manually, set `UXPLAY_AUTOSTART=0` and update the `VIDEO_SOURCE` environment
   variable to match the stream URL you expose.

3. Launch the WebRTC bridge:

   ```bash
   UXPLAY_COMMAND="uxplay -n ios-automation -S -p 11000" \
   VIDEO_SOURCE="udp://127.0.0.1:11000" \
   uvicorn video_feed.main:app --host 0.0.0.0 --port 8000
   ```

   The defaults target a UDP stream exposed by `uxplay`. Adjust them to match
   your AirPlay setup (see **Configuration** below).

4. Open `http://<host>:8000` in a WebRTC-capable browser. You should see a live
   preview sourced from the automation pipeline. Use that page purely for
   monitoring – your automation loop continues to run inside Python.

## Configuration

The pipeline uses environment variables so that it remains scriptable:

| Variable | Default | Description |
|----------|---------|-------------|
| `UXPLAY_COMMAND` | *(unset)* | Command used to start `uxplay`. If empty, the process is not spawned automatically. |
| `UXPLAY_AUTOSTART` | `1` | Whether to start `uxplay` during FastAPI startup. |
| `UXPLAY_SHUTDOWN_TIMEOUT` | `5` | Seconds to wait for `uxplay` to terminate cleanly on shutdown. |
| `VIDEO_SOURCE` | `udp://127.0.0.1:11000` | OpenCV capture URL. Point this at the socket that `uxplay` publishes. |
| `VIDEO_WIDTH` | *(unset)* | Optional width hint passed to OpenCV. |
| `VIDEO_HEIGHT` | *(unset)* | Optional height hint passed to OpenCV. |
| `VIDEO_FPS` | `30` | Frame rate used when generating WebRTC timestamps. |
| `AUTOMATION_MODULE` | *(unset)* | Optional dotted path to a callable `process(frame)` that will receive each frame before streaming. |

You can write your automation routines inside the repository (e.g.
`automation/pipeline.py`) and expose a `process(frame)` function that receives a
`numpy.ndarray` in BGR format. Set `AUTOMATION_MODULE=automation.pipeline:process`
and every frame will be forwarded to that function before it reaches the
browser.

## Notes

* The included HTML client is intentionally minimal; integrate it into your own
  control surface (or into the existing FastAPI app under `web/`) if desired.
* `uxplay` can output in multiple formats (raw window, TCP, UDP, etc.). The
  default configuration assumes a UDP H.264 stream that OpenCV can decode. Adjust
  the command and source URL if your setup differs.
* Because the automation is headless, the script keeps only the latest frame in
  memory to minimize latency while still exposing the feed to the browser.
* The WebRTC server is limited to a single outgoing track per peer, but it can
  serve multiple clients concurrently; new peer connections create fresh tracks
  against the shared OpenCV source.

## Integrating with the existing control server

If you already run `web/server.py` for BLE/HID control, you can either run the
video pipeline alongside it or mount the FastAPI application under a sub-path
using `web.server`'s routing features. Keeping it in a dedicated folder avoids
mixing concerns and lets you deploy / restart the video pipeline independently
from the BLE bridge.

## Troubleshooting

* If the WebRTC preview stays black, ensure `uxplay` is emitting frames and that
  the `VIDEO_SOURCE` URL matches what OpenCV expects. You can test capture with:

  ```python
  python - <<'PY'
  import cv2
  cap = cv2.VideoCapture("udp://127.0.0.1:11000")
  ret, frame = cap.read()
  print("Frame captured?", ret, frame.shape if ret else None)
  PY
  ```

* For networks with packet loss, increase the `uxplay` buffer or append
  `?fifo_size=5000000&overrun_nonfatal=1` to the UDP URL so FFmpeg tolerates
  jitter.
* Install `gstreamer1.0-tools` and run `gst-launch-1.0 udpsrc ...` to validate
  the raw stream if you are uncertain about the transport layer.

With these building blocks in place you can focus on the OpenCV automation logic
while still having a lightweight browser-based viewport for monitoring.
