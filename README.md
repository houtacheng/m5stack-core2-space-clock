# M5Stack Core2 Space Clock

Native firmware that turns an M5Stack Core2 into a smooth animated clock,
20-alarm clock, meditation timer, emotion-observation logger, MQTT device and
four-page Bitfocus Companion Satellite control panel. Version 2.9.11 replaces
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
