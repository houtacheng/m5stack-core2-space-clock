# M5Stack Core2 Space Clock

Native firmware that turns an M5Stack Core2 into a smooth animated clock,
20-alarm clock, meditation timer, emotion-observation logger, MQTT device and
four-page Bitfocus Companion Satellite control panel. Version 2.9.16 gives each
clock navigation button a matched short-press / long-press icon pair:
Companion / Emotion, Meditation / Home Assistant, and Settings / Night Light.
Version 2.9.15 fixes
statistics API parsing behind Caddy/Cloudflare by decoding HTTP transfer framing
before applying the low-memory JSON field filter. Version 2.9.14 adds a
database-backed **My Records** page to emotion observation Page 1. It reads the
authenticated `/api/statistics` summary for all non-deleted records and shows
learning days, entry count, daily average, most common body response and
emotion, strongest emotion, and most-used grounding method. Version 2.9.13 adds a
push-to-talk Home Assistant Assist terminal using the Core2 microphone and
speaker, and rebuilds the medium Traditional Chinese font at one consistent
size so the Page 4 emotion/category selector no longer mixes glyph sizes or
baselines. Version 2.9.12 reverses
the time-field tap direction so the upper half moves backward and the lower half
moves forward, and stores sweating as the journal API's checkbox boolean.
Version 2.9.11 replaces
time-field swipes with upper/lower taps, makes emotion and grounding selectors
consistently bidirectional, requires an emotion before continuing, and adds
secondary breathing. Version 2.9.10 fixes
emotion-entry withdrawal by using the API's required authenticated soft-delete
update instead of a forbidden direct deletion. Alarm dismissal now follows the
active face: astronaut drag, one clockwise gear turn, or a dissolving blue-pill
long press. Version 2.9.9 gives the
emotion-observation wizard a Matrix-themed interface, swipe-based time entry,
per-page reset controls, confirmation plus withdrawal after submission, and
scheduled reminders within a configurable daily window. OTA preserves saved
Wi-Fi, MQTT, alarm and API settings. Version 2.9.8 restored verified HTTPS login
and the live database connection indicator.

The emotion picker now includes ten detailed categories: anger, hurt, sadness,
loss, despair, anxiety, stress, nostalgia, self-denial and failure.

See [SpaceClockNative/README.md](SpaceClockNative/README.md) for setup, features,
building and OTA update instructions. The ready-to-install image is published at
`firmware/SpaceClockNative-OTA.bin`.
