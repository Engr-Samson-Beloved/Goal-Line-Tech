# Session camera

The AI-Thinker ESP32-CAM joins `GOAL-LINE-REFEREE` at `192.168.4.2` and polls
the wrist unit for match state. During an active match it serves MJPEG at
`http://192.168.4.2:81/stream` and records a 640×480, 5 fps MJPEG AVI file.

Recordings are stored as `/sessions/session_NNNN.avi`. Recording pauses when
the match is paused and the same session file continues after resume. The file
is finalized when the match is reset or ended. Headers are patched every five
seconds to reduce data loss if power is interrupted.

Open `http://192.168.4.2/` for camera status and `http://192.168.4.2/sessions`
to list or download completed recordings. Downloads are disabled during an
active match to protect recording performance.

Use a FAT32 microSD card. The firmware uses one-bit SD mode to avoid conflicts
with camera and flash pins. Streaming continues if the card is absent, while
recording remains disabled until a card mounts successfully.
