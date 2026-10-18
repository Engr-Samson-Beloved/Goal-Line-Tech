# Session camera

## Power and pins

Use an AI-Thinker ESP32-CAM with a stable regulated 5 V supply capable of at
least 1 A. Connect the supply to the board `5V` and `GND` pins.

The OV2640 uses GPIO32 PWDN, GPIO0 XCLK, GPIO26 SCCB SDA, GPIO27 SCCB SCL,
GPIO5/18/19/21/36/39/34/35 for image data, GPIO25 VSYNC, GPIO23 HREF, and
GPIO22 PCLK. RESET is not connected.

The FAT32 microSD interface runs in one-bit mode using GPIO14 CLK, GPIO15 CMD,
and GPIO2 DATA0. GPIO1 TX and GPIO3 RX are used for flashing and serial logs;
GPIO0 is connected to GND only to enter flashing mode.

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

The camera and wrist pages start the image only during an `ACTIVE` match and
automatically retry a failed or closed MJPEG connection. During `READY`,
`PAUSED`, or `STOPPED`, the page displays that it is waiting for an active match.

Use a FAT32 microSD card. The firmware uses one-bit SD mode to avoid conflicts
with camera and flash pins. Streaming continues if the card is absent, while
recording remains disabled until a card mounts successfully.
