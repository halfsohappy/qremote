# led_controller — qremote LED strip receiver

Receiving half of the qremote system: a Waveshare ESP32-S3-LCD-1.69 driving
4x RGBW 24V LED strips through a PCA9685, plus a solenoid prop effect. It
listens for `light_cmd_t` packets (see [`../shared/light_protocol.h`](../shared/light_protocol.h))
sent over ESP-NOW by [`../src`](../src) — the OSC dongle plugged into the
Mac running QLab.

```
QLab --OSC/UDP:53000--> dongle (../src) --ESP-NOW--> this board --I2C/PCA9685--> LED strips
```

## QLab side

Drive it with Network (OSC) cues in your cue list, addressed to the dongle
(`qremote.local:53000`), all-integer args:

| Address              | Args                              |
|----------------------|------------------------------------|
| `/light/strip`       | `strip(0-3) r g b w fadeMs`        |
| `/light/all`         | `r g b w fadeMs`                   |
| `/light/off`         | `[fadeMs]` (defaults to 0 = snap)  |
| `/light/solenoid`    | `pulseMs`                          |

`r/g/b/w` are 0-255; the dongle scales them to the PCA9685's 12-bit range.
`fadeMs` of 0 snaps instantly, otherwise this board crossfades in software
over that duration.

## Build / flash

```sh
cd led_controller
pio run
pio run -t upload
pio device monitor
```

If the board is not detected for upload, hold **BOOT**, tap **RST**, release **BOOT**.

## Pinout

See [`../README.md`](../README.md) pin table (this project reuses the same
board) — [`include/board_pins.h`](include/board_pins.h) additionally defines
`PIN_SOLENOID` (GPIO16) for the added solenoid driver.

## Notes

- This unit's screen and buzzer are dead, so the TFT_eSPI display stack
  isn't wired up here — status is USB serial when tethered, plus a boot
  flash and a faint "waiting for signal" heartbeat on strip 0's white
  channel when no `/light` command has arrived in 5s (see `stepHeartbeat()`
  in `src/main.cpp`). If you're building for a unit with a working screen,
  reintroducing TFT_eSPI status output would be a good next step.
- ESP-NOW uses broadcast on a fixed channel (`LIGHT_ESPNOW_CHANNEL` in
  `light_protocol.h`) — no pairing needed for a single receiver. Multiple
  receivers can listen to the same broadcasts as-is.
- Serial commands from the original bring-up sketch still work for bench
  testing (`i`, `c`, `s`, `a`, `x`, `v`) and won't conflict with live
  ESP-NOW control unless you toggle test mode with `t`.
