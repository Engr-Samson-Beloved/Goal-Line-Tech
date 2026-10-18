# Goal-post controller wiring

The production controller uses four active-low E3F NPN/open-collector sensor inputs:

| Sensor | ESP32 pin |
| --- | ---: |
| 1 | GPIO13 |
| 2 | GPIO14 |
| 3 | GPIO27 |
| 4 | GPIO26 |

Connect the controller and sensor interface to a common ground. The inputs use the
ESP32's internal pull-ups and consider a LOW signal blocked. Each GPIO must receive
a conditioned 0–3.3 V/open-collector signal. Do not connect a 5 V, 12 V, or raw
industrial sensor output directly to an ESP32 GPIO; use the appropriate level
shifter, optocoupler, or transistor interface.

One active sensor produces `GOAL`. Two or more sensors active within the grouping
window produce `CHECK VAR`. An event remains latched until all sensor inputs have
been clear for 300 ms. Sensor events are accepted only while the wrist unit is in
an active match.

## Optional HUSKYLENS assistant

HUSKYLENS connects to the goal-post controller over I2C. It does not connect to
the ESP32-CAM and it does not carry match video.

| HUSKYLENS | ESP32 controller |
| --- | ---: |
| SDA | GPIO21 |
| SCL | GPIO22 |
| GND | GND |
| VCC | Regulated 5 V supply |

Set HUSKYLENS to I2C protocol mode and train the ball as ID 1. The controller
checks for address `0x32` at startup and every five seconds while disconnected
and the match is inactive. It never retries camera setup during active play.
If the camera is absent or a request fails, sensor processing and ESP-NOW wrist
communication continue in sensor-only fallback mode.

The shipped configuration uses monitor-only mode:

```cpp
constexpr bool HUSKY_DECISION_ENABLED = false;
```

In this mode, a sensor event queries HUSKYLENS and prints the result on Serial,
but the camera cannot change GOAL or CHECK VAR. After physically mounting and
calibrating the view, set the measured `HUSKY_GOAL_ZONE_X_MIN` and
`HUSKY_GOAL_ZONE_X_MAX`, then change `HUSKY_DECISION_ENABLED` to `true`.
With assistance enabled, a single-sensor event becomes CHECK VAR when the
connected HUSKYLENS answers successfully but does not see ball ID 1 in the goal
zone. A missing or failed HUSKYLENS still falls back to the original sensor-only
decision.

Install the official `HUSKYLENS` Arduino library before compiling in Arduino
IDE. The PlatformIO build downloads the same official library from its GitHub
repository.
