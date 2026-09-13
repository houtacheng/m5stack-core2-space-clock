# Space Clock Native for M5Stack Core2

Fast native C++ firmware for the M5Stack Core2, with original artwork and a
touch-friendly clock, alarm, meditation and Bitfocus Companion interface.

## Main features

- Original animated space clock and large split-flap clock face
- Twenty alarms with weekday repeat, sound, snooze and Bottom2 light effects
- 3×2 Bitfocus Companion Satellite panel with four independently configured pages
- Automatic Companion failover: local TCP 16622 first, Internet WSS/443 second
- Meditation timer with two presets, reminders, ambience and Bottom2 lighting
- NTP time, major-city time zones, 12/24-hour display and automatic brightness
- Screen timeout with touch and motion wake
- Browser configuration, alarms, MQTT guide and manual OTA upload
- MQTT state publishing and remote modification of settings
- GitHub firmware checks plus optional scheduled automatic update

## First setup

Copy `wifi_secrets.example.h` to `wifi_secrets.h` if a build-time Wi-Fi fallback
is wanted. The private file is ignored by Git. Without it, define the same two
macros locally or use the `SpaceClock-Setup` captive portal.

After connection, the device IP is shown at the top of each clock face. Open
that IP in a browser to configure the clock.

## Firmware update

On the Core2 open **Settings → Firmware update**:

- **Check** reads `firmware/manifest.json` from GitHub.
- **Install** appears only when the manifest version is newer.
- Tap **Automatic update** to enable or disable unattended installation.
- Tap **Daily check** to select the hour (device local time).

Keep the device on USB power during installation. Existing preferences are
preserved. The browser's manual `.bin` upload remains available as a recovery
option.

## Build

Select **M5Stack Core2** in Arduino IDE. The project requires M5Unified/M5GFX,
ArduinoJson, WiFiManager and PubSubClient. The downloadable OTA image is
`firmware/SpaceClockNative-OTA.bin`.
