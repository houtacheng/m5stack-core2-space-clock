# Space Clock Native for M5Stack Core2

Fast native C++ firmware for the M5Stack Core2, with original artwork and a
touch-friendly clock, alarm, meditation and Bitfocus Companion interface.

Version 2.9.16 displays both actions on every clock navigation button with
normalized icon pairs: Companion / Emotion, Meditation / Home Assistant, and
Settings / Night Light. Short-press and long-press behavior is unchanged.

Version 2.9.15 fixes the My Records parser for statistics responses delivered
through Caddy/Cloudflare. HTTP transfer framing is now decoded before the
low-memory JSON field filter runs, preventing false “missing data” errors.

Version 2.9.14 adds a **My Records** button to emotion observation Page 1 and a
Matrix-styled database statistics screen. The device reads the authenticated
`/api/statistics` endpoint for the full history while excluding deleted records,
with token refresh and explicit offline/login error states.

Version 2.9.13 adds push-to-talk Home Assistant Assist through the Core2
microphone and speaker. It also regenerates the Page 4 Traditional Chinese
font at a consistent size and separates its navigation arrows from the value,
fixing mixed sizes and alignment in longer emotion names.

Version 2.9.12 makes the upper half of each time field move backward and the
lower half move forward. Sweating is now a two-state checkbox and is submitted
to PocketBase as a JSON boolean, matching the web journal.

Version 2.9.11 changes emotion-form time fields to upper/lower tap controls,
uses consistent previous/next selectors, blocks continuation until an emotion
is selected, and adds secondary breathing as a grounding action.

Version 2.9.10 changes emotion-entry withdrawal to the PocketBase-compatible
soft-delete flow (`PATCH` with `deleted: true`), matching the journal API rules.
It also gives each clock face its own animated alarm-dismissal challenge: drag
the astronaut on Space, rotate the large gear clockwise once on Flip, or hold
the blue pill until it dissolves on Matrix. Snooze remains available.

## Main features

- Original animated space clock and large split-flap clock face
- Twenty alarms with weekday repeat, sound, snooze and Bottom2 light effects
- 3×2 Bitfocus Companion Satellite panel with four independently configured pages
- Automatic Companion failover: local TCP 16622 first, Internet WSS/443 second
- Meditation timer with two presets, reminders, ambience and Bottom2 lighting
- Emotion observation wizard entered by long-pressing Companion on the clock; PocketBase API submission is confirmed on the final review page
- Configurable emotion-entry reminders: disabled, scheduled within a daily time window, or up to three fixed times, with optional vibration/audio and a selectable reminder length
- NTP time, major-city time zones, 12/24-hour display and automatic brightness
- Screen timeout with touch and motion wake
- Long-press the clock's settings gear to toggle the Bottom2 night light; set its color and brightness in the web settings
- Ten private saved Wi-Fi profiles with non-blocking automatic reconnect; legacy Wi-Fi credentials remain the first connection path
- Bilingual (Traditional Chinese/English), categorized browser configuration with independent per-page saving, alarms, MQTT guide and manual OTA upload
- MQTT state publishing and remote modification of settings
- Push-to-talk Home Assistant Assist using the built-in microphone and speaker
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

The Matrix-themed form starts when you long-press the left **Companion** button on the clock.
Its top-left light is green only after the saved account has been authenticated
against the database; editing stays locked while the light is yellow or red.
It captures the device's local time and guides you through trigger, body
reaction, emotion, intensity, observation duration and grounding. Short-press
**Cancel** to return to the clock, or long-press it for on-device reminder
settings. Pages 1–7 have a graphical reset control in the upper-right corner;
on page 1, tap the upper half of a date/time field to move backward and its
lower half to move forward. On the final
review page, tap **Send** and then **Confirm send** to create the PocketBase
`entries` record over HTTPS. After submission, **Withdraw** deletes that record,
while **Sent** starts a fresh form. The API base URL defaults to
`https://emotion.theoakhouse.org`. The HTTPS client validates the server against
embedded ISRG Root X1 and Google Trust Services Root R4 trust anchors.
The emotion picker provides ten detailed categories: anger, hurt, sadness,
loss, despair, anxiety, stress, nostalgia, self-denial and failure.

Reminder settings are saved independently in the same web tab. Scheduled mode
uses a daily start/end window and intervals of 10, 15, 30, 60, 120, 180 or 240
minutes, anchored at the start time. Fixed mode supports three daily times; each
can be enabled separately. The **Later** device action snoozes a reminder for ten
minutes.

## Home Assistant Assist

In Home Assistant, open the user profile and create a long-lived access token.
Then open the Core2 browser settings and choose **HASS Assist / HASS 語音助理**:

1. Enable HASS Assist and enter the Home Assistant base URL, for example
   `http://homeassistant.local:8123` or the HTTPS URL used outside the LAN.
2. Paste the long-lived token. A pipeline ID is optional; leaving it blank uses
   Home Assistant's preferred Assist pipeline.
3. Set the reply volume and save this page.
4. On the clock, long-press the middle Meditation icon. Hold the on-screen
   microphone while speaking, then release it to send. The spoken reply plays
   through the Core2 speaker.

The feature sends 16 kHz mono speech through Home Assistant's Assist WebSocket
pipeline and supports MP3 or WAV TTS replies. The token stays in this Core2's
Preferences and is not included in public firmware, GitHub or MQTT. Because the
Core2 settings page itself is local HTTP, enter the token only on trusted Wi-Fi.

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
