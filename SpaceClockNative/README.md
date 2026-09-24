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
the journal. Use **Check database connection** to verify a saved login. The
password is not written to Preferences; the returned bearer token and account
ID are kept on the Core2 so the device can submit a record.
Use **Forget this device's API login** to remove them. The current settings page
uses local HTTP, so sign in only from a trusted Wi-Fi network and preferably use
a dedicated account. The device does not enable ESP32 flash encryption, so its
stored token is not protected against physical flash extraction.

The form starts when you long-press the left **Companion** button on the clock.
Its top-left light is green only after the saved account has been authenticated
against the database; editing stays locked while the light is yellow or red.
It captures the device's local time and guides you through trigger, body
reaction, emotion, intensity, observation duration and grounding. Short-press
**Cancel** to return to the clock, or long-press it for on-device reminder
settings. On the final review page, tap **Confirm** and then **Send** to create
the PocketBase `entries` record over HTTPS. The API base URL defaults to
`https://emotion.theoakhouse.org`. The HTTPS client validates the server against
embedded ISRG Root X1 and Google Trust Services Root R4 trust anchors.

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

Select **M5Stack Core2** (M5Stack ESP32 board package 3.2.5, PSRAM **Enabled**,
default 16 MB OTA partition layout) in Arduino IDE. The project requires M5Unified/M5GFX,
ArduinoJson, WiFiManager and PubSubClient. The downloadable OTA image is
`firmware/SpaceClockNative-OTA.bin`.

For a public release build, define `SPACE_CLOCK_PUBLIC_BUILD` so that ignored
local Wi-Fi credentials are excluded from the binary:

```sh
bash tools/build_public.sh
```

The script validates embedded font bitmaps and uses
`compiler.cpp.extra_flags=-DSPACE_CLOCK_PUBLIC_BUILD`. **Do not replace
`build.extra_flags`**: doing so removes the Core2 PSRAM initialization define
and can leave the full-screen Matrix canvas unavailable. The firmware now
rejects builds without `BOARD_HAS_PSRAM`. Set `SPACE_CLOCK_ARDUINO_CLI` if the CLI
is not on PATH; `SPACE_CLOCK_PYTHON` can select a Python runtime.

OTA updates install only the application image, leaving existing NVS Wi-Fi,
MQTT, alarm and other settings intact. Do not erase flash or upload a partition
table when performing a routine update.

Font changes can be checked with `python3 -m unittest discover -s tools -p
test_font_tools.py` (Pillow required for generation tests). The validator checks
nonempty glyph pixels and coverage, and the public-build check verifies the same
bitmap data is present in the OTA image. When rasterizing a glyph with Pillow,
use the same `anchor="ls"` for both its bounding box and its drawing operation.
