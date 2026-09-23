# Space Clock Native for M5Stack Core2

Fast native C++ firmware for the M5Stack Core2, with original artwork and a
touch-friendly clock, alarm, meditation and Bitfocus Companion interface.

## Main features

- Original animated space clock and large split-flap clock face
- Twenty alarms with weekday repeat, sound, snooze and Bottom2 light effects
- 3×2 Bitfocus Companion Satellite panel with four independently configured pages
- Automatic Companion failover: local TCP 16622 first, Internet WSS/443 second
- Meditation timer with two presets, reminders, ambience and Bottom2 lighting
- Emotion observation wizard entered by long-pressing Companion on the clock; PocketBase API submission is confirmed on the final review page
- Configurable emotion-entry reminders: disabled, interval-based, or up to three fixed times, with optional vibration/audio and a selectable reminder length
- NTP time, major-city time zones, 12/24-hour display and automatic brightness
- Screen timeout with touch and motion wake
- Long-press the clock's settings gear to toggle the Bottom2 night light; set its color and brightness in the web settings
- Ten private saved Wi-Fi profiles with non-blocking automatic reconnect; legacy Wi-Fi credentials remain the first connection path
- Bilingual (Traditional Chinese/English), categorized browser configuration with independent per-page saving, alarms, MQTT guide and manual OTA upload
- MQTT state publishing and remote modification of settings
- GitHub firmware checks plus optional scheduled automatic update

## First setup

Copy `wifi_secrets.example.h` to `wifi_secrets.h` if a build-time Wi-Fi fallback
is wanted. The private file is ignored by Git. Without it, define the same two
macros locally or use the `SpaceClock-Setup` captive portal.

After connection, the device IP is shown at the top of each clock face. Open
that IP in a browser to configure the clock.

## Emotion observation

Open the device's settings page and choose **Emotion journal / 情緒觀察**. Save
the API base URL first, then sign in with the PocketBase user account used by
the journal. The password is not written to Preferences; the returned bearer
token and account ID are kept on the Core2 so the device can submit a record.
Use **Forget this device's API login** to remove them. The current settings page
uses local HTTP, so sign in only from a trusted Wi-Fi network and preferably use
a dedicated account. The device does not enable ESP32 flash encryption, so its
stored token is not protected against physical flash extraction.

The form starts when you long-press the left **Companion** button on the clock.
It captures the device's local time and guides you through trigger, body
reaction, emotion, intensity, observation duration and grounding. On the final
review page, tap **Confirm** and then **Send** to create the PocketBase `entries`
record over HTTPS. The API base URL defaults to `https://emotion.theoakhouse.org`.
The HTTPS client validates the server against the embedded ISRG Root X1 trust
anchor; if the API changes certificate authority, the firmware trust anchor must
be updated before it can connect.

Reminder settings are saved independently in the same web tab. Interval mode
supports 15, 30, 60, 120 or 240 minutes. Fixed mode supports three daily times;
each can be enabled separately. The **Later** device action snoozes a reminder
for ten minutes.

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

For a public release build, define `SPACE_CLOCK_PUBLIC_BUILD` so that ignored
local Wi-Fi credentials are excluded from the binary:

```sh
arduino-cli compile --fqbn m5stack:esp32:m5stack_core2 \
  --build-property build.extra_flags=-DSPACE_CLOCK_PUBLIC_BUILD SpaceClockNative
```
