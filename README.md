# Goal Line Technology

This repository contains the production firmware for a four-sensor goal-post
controller, an ESP32-C3 referee wrist unit, an AI-Thinker ESP32-CAM, and an
optional HUSKYLENS vision assistant.

## System connections

```text
Four goal sensors ---- GPIO ----> Goal-post ESP32
HUSKYLENS ------------ I2C -----> Goal-post ESP32
Goal-post ESP32 ------- ESP-NOW -> ESP32-C3 wrist
ESP32-CAM ------------- Wi-Fi ---> ESP32-C3 wrist access point
Phone ----------------- Wi-Fi ---> Referee controls and live camera
```

The wrist creates the `GOAL-LINE-REFEREE` Wi-Fi network on channel 1. The
goal-post controller exchanges commands, heartbeats, events, and acknowledgments
with the wrist through ESP-NOW on the same channel. The ESP32-CAM joins the wrist
network, polls match state from `192.168.4.1`, and uses `192.168.4.2` for its web
page and stream.

## ESP32-C3 wrist wiring

| Connection | ESP32-C3 pin |
| --- | ---: |
| OLED SDA | GPIO4 |
| OLED SCL | GPIO5 |
| OLED VCC | 3.3 V |
| OLED GND | GND |
| Optional alert LED anode through 220-330 ohm resistor | GPIO3 |
| Optional alert LED cathode | GND |

The firmware detects an SSD1306-compatible OLED at I2C address `0x3C` or
`0x3D`. Power the C3 from regulated USB 5 V. Its referee interface is:

- Wi-Fi: `GOAL-LINE-REFEREE`
- Password: `GoalLine2026`
- Controls: `http://192.168.4.1/`

## Goal-post ESP32 wiring

The four digital line-sensor inputs are active HIGH and use the ESP32 internal
pull-downs.

| Sensor | Controller pin |
| --- | ---: |
| Sensor 1 output | GPIO13 |
| Sensor 2 output | GPIO14 |
| Sensor 3 output | GPIO27 |
| Sensor 4 output | GPIO26 |
| Sensor interface ground | GND |

Each GPIO must receive a conditioned 0-3.3 V digital signal: LOW when clear and
HIGH when blocked.
Never connect a raw 5 V or 12 V sensor output directly to an ESP32 GPIO. Use a
level shifter, transistor interface, or optocoupler when required, and join the
interface ground to the controller ground.

One active sensor produces `GOAL`. A second active sensor immediately upgrades
the same passage to `CHECK VAR`. Inputs use an 8 ms debounce, and another event
can begin after every input has been clear for 60 ms. Events are accepted only
while the wrist reports an active match.

## HUSKYLENS wiring

Connect HUSKYLENS to the goal-post controller, not to the ESP32-CAM.

| HUSKYLENS | Goal-post ESP32 |
| --- | ---: |
| SDA | GPIO21 |
| SCL | GPIO22 |
| GND | GND |
| VCC | Regulated 5 V |

Select I2C protocol mode on HUSKYLENS. Its I2C address is `0x32`, and the ball
should be trained as object ID 1. The current production setting is monitor-only:
HUSKYLENS results are logged for calibration but cannot change a match decision.
If it is missing or stops responding, the four sensors and ESP-NOW continue in
sensor-only mode.

## AI-Thinker ESP32-CAM connections

Power the ESP32-CAM through its `5V` and `GND` pins with a stable regulated 5 V
supply capable of at least 1 A. The OV2640 camera is connected internally:

| OV2640 signal | ESP32-CAM pin |
| --- | ---: |
| PWDN | GPIO32 |
| RESET | Not connected (`-1`) |
| XCLK | GPIO0 |
| SCCB SDA | GPIO26 |
| SCCB SCL | GPIO27 |
| Y2, Y3, Y4, Y5 | GPIO5, GPIO18, GPIO19, GPIO21 |
| Y6, Y7, Y8, Y9 | GPIO36, GPIO39, GPIO34, GPIO35 |
| VSYNC | GPIO25 |
| HREF | GPIO23 |
| PCLK | GPIO22 |

The microSD card runs in one-bit mode:

| microSD signal | ESP32-CAM pin |
| --- | ---: |
| CLK | GPIO14 |
| CMD | GPIO15 |
| DATA0 | GPIO2 |

For flashing with a USB serial adapter, GPIO1 is TX, GPIO3 is RX, and GPIO0 is
held to GND only while entering download mode. Remove the GPIO0-to-GND connection
and reset after flashing. Programmer-base boards normally handle these signals.

The camera endpoints are:

- Status and camera page: `http://192.168.4.2/`
- Active-match MJPEG stream: `http://192.168.4.2:81/stream`
- Completed recordings: `http://192.168.4.2/sessions`

The live image starts only when the match state is `ACTIVE`. The pages retry the
stream automatically after startup errors or a pause/resume cycle. Recording
requires a FAT32 microSD card; streaming remains available without a card.

## Expected power-up sequence

1. The wrist displays `GOAL LINE`, `00:00`, and `READY` and starts its Wi-Fi.
2. The goal-post controller starts sending heartbeats; the referee page reports
   `Goal post connected` within a few seconds.
3. The ESP32-CAM joins the wrist Wi-Fi at `192.168.4.2`.
4. Pressing **Start Match** changes the wrist to `PLAY`, starts the timer, opens
   the video stream, and starts recording when a microSD card is ready.

Repeated resets, flickering, or disappearing Wi-Fi usually indicates an
undersized or unstable power supply.

## Firmware folders

- `Firmware/WristUnit` - ESP32-C3 OLED, referee controls, Wi-Fi, and ESP-NOW
- `Firmware/GoalPostController` - four sensors, optional HUSKYLENS, and ESP-NOW
- `Firmware/GoalPostCam` - live MJPEG and match-session recording

The `.pio-wrist`, `.pio-controller`, and `.pio-cam` folders contain the tested
PlatformIO build configurations for the corresponding production devices.
