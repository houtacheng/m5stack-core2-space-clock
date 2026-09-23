#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <PubSubClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <math.h>
#include "config.h"
#include "assets.h"
#include "generated/source_han_sans_8.h"
#include "generated/source_han_sans_14.h"
#include "generated/source_han_sans_28_ascii.h"
#include "generated/alarm_sound.h"
#include "generated/chime_sound.h"
#include "generated/stream_sound.h"
#include "generated/drop_sound.h"
#include "generated/rain_sound.h"
#include "generated/insects_sound.h"
#include "wifi_secrets.h"
#include "mqtt_guide.h"

enum class Screen : uint8_t { Clock, Menu, Faces, Companion, Alarms, Settings, Meditation, MeditationSettings, FirmwareUpdate, About };
enum class ClockFace : uint8_t { Space, Minimal, Matrix };
enum class MeditationState : uint8_t { Ready, Running, Paused, Done };

struct Alarm {
  uint8_t hour = 7;
  uint8_t minute = 0;
  uint8_t weekdays = 0; // bits 0..6 = Sun..Sat; zero means one-shot
  bool enabled = false;
  int32_t lastDay = -1;
};

Preferences prefs;
Screen screenNow = Screen::Clock;
ClockFace clockFace = ClockFace::Space;
static constexpr uint8_t ALARM_COUNT = 20;
static constexpr uint8_t ALARMS_PER_PAGE = 4;
static constexpr uint8_t ALARM_PAGE_COUNT = ALARM_COUNT / ALARMS_PER_PAGE;
static constexpr uint8_t ALARM_SCHEMA_VERSION = 2;
Alarm alarms[ALARM_COUNT];
uint8_t alarmPage = 0;
int8_t alarmActive = -1;
int8_t snoozedAlarm = -1;
uint32_t snoozeStarted = 0;
bool astronautDragging = false;
int astronautX = 6, astronautY = 112;
int astronautDX = 1, astronautDY = 1;
uint32_t lastAlarmDragDraw = 0;
bool satelliteTop = true;
struct TimeZoneChoice { const char* city; const char* rule; };
static constexpr TimeZoneChoice TIME_ZONES[] = {
  {"UTC", "UTC0"},
  {"New York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
  {"Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
  {"Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
  {"Los Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
  {"Honolulu", "HST10"},
  {"Mexico City", "CST6"},
  {"Toronto", "EST5EDT,M3.2.0/2,M11.1.0/2"},
  {"Sao Paulo", "<-03>3"},
  {"London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
  {"Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
  {"Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
  {"Johannesburg", "SAST-2"},
  {"Dubai", "GST-4"},
  {"Delhi", "IST-5:30"},
  {"Bangkok", "ICT-7"},
  {"Singapore", "SGT-8"},
  {"Hong Kong", "HKT-8"},
  {"Taipei", "CST-8"},
  {"Tokyo", "JST-9"},
  {"Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
  {"Auckland", "NZST-12NZDT,M9.5.0/2,M4.1.0/3"}
};
static constexpr uint8_t TIME_ZONE_COUNT = sizeof(TIME_ZONES) / sizeof(TIME_ZONES[0]);
uint8_t timeZoneIndex = 18; // Taipei
bool adaptiveBrightness = true;
uint8_t dayBrightness = 80;
uint8_t nightBrightness = 20;
uint8_t alarmVolume = 80;
uint8_t alarmSound = 0;
bool use24HourTime = true;
bool flatVirtualButtonsEnabled = false;
uint16_t screenOffSeconds = 300;
bool meditationSoundEnabled = true;
uint16_t meditationPresetMinutes[2] = {5, 15};
uint8_t meditationStartSound = 1;
uint8_t meditationEndSound = 1;
uint8_t meditationStartVolume = 55;
uint8_t meditationEndVolume = 70;
bool meditationLightEnabled = true;
bool meditationNoiseEnabled = false;
uint8_t meditationNoise = 0;
uint8_t meditationNoiseVolume = 25;
MeditationState meditationState = MeditationState::Ready;
uint32_t meditationDurationSeconds = 300;
uint32_t meditationElapsedBeforeRun = 0;
uint32_t meditationRunStarted = 0;
uint32_t meditationLightEventStarted = 0;
uint32_t meditationAmbientPendingAt = 0;
bool alarmLightEnabled = true;
uint32_t alarmLightColor = 0xFFFFFF;
uint8_t alarmLightBrightness = 35;
uint8_t alarmLightMode = 0;
bool nightLightEnabled = false;
uint32_t nightLightColor = 0xFFF0C8;
uint8_t nightLightBrightness = 18;
uint8_t nightLightMode = 0;
uint16_t nightLightSeconds = 60;
bool screenSleeping = false;
bool automaticFirmwareUpdate = false;
uint8_t firmwareCheckHour = 3;
int32_t lastAutomaticUpdateDay = -1;
String latestFirmwareVersion;
String latestFirmwareUrl;
String firmwareUpdateMessage = "Tap Check to look for a new version.";
bool firmwareUpdateAvailable = false;
bool wakeTouchConsumed = false;
uint32_t lastUserActivity = 0;
uint32_t screenSleepStarted = 0;
uint32_t lastMotionSample = 0;
float previousAccelX = 0, previousAccelY = 0, previousAccelZ = 0;
bool motionBaselineReady = false;
static constexpr uint8_t COMPANION_PAGE_COUNT = 4;
String companionHosts[COMPANION_PAGE_COUNT];
String companionInternetUrls[COMPANION_PAGE_COUNT];
String companionNames[COMPANION_PAGE_COUNT] = {"Main", "Studio", "Remote", "Backup"};
uint16_t companionPorts[COMPANION_PAGE_COUNT] = {16622, 16622, 16622, 16622};
uint8_t companionPage = 0;
uint32_t companionCenterPressedAt = 0;
WiFiClient companionClient;
WebSocketsClient companionWebSocket;
bool companionWebSocketMode = false;
bool companionWebSocketConnected = false;
bool companionUsingInternet = false;
uint32_t lastCompanionLocalProbe = 0;
WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
bool mqttEnabled = false;
String mqttHost;
uint16_t mqttPort = 1883;
String mqttUsername;
String mqttPassword;
String mqttBaseTopic = "spaceclock/core2";
uint32_t lastMqttReconnect = 0;
uint32_t lastMqttPublish = 0;
bool mqttSettingsDirty = true;
bool mqttReconnectRequested = false;
bool mqttDiscoveryDirty = true;
WebServer settingsServer(80);
bool settingsServerReady = false;
bool companionRegistered = false;
static constexpr uint8_t SAVED_WIFI_COUNT = 10;
String savedWifiSsids[SAVED_WIFI_COUNT];
String savedWifiPasswords[SAVED_WIFI_COUNT];
uint32_t lastWifiReconnectAttempt = 0;
String companionDeviceId = "core2";
static constexpr uint8_t COMPANION_COLS = 3;
static constexpr uint8_t COMPANION_ROWS = 2;
static constexpr uint8_t COMPANION_KEYS = COMPANION_COLS * COMPANION_ROWS;
String companionLabels[COMPANION_KEYS] = {"Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6"};
uint16_t companionColors[COMPANION_KEYS] = {0x2945, 0x2945, 0x2945, 0x2945, 0x2945, 0x2945};
struct CompanionImage { uint8_t* data = nullptr; size_t size = 0; };
CompanionImage companionImages[COMPANION_KEYS];
M5Canvas astronautCanvas(&M5.Display);
M5Canvas companionButtonCanvas(&M5.Display);
M5Canvas meditationCardCanvas(&M5.Display);
M5Canvas matrixCanvas(&M5.Display);
static constexpr uint8_t BOTTOM_LED_PIN = 25;
static constexpr uint8_t BOTTOM_LED_COUNT = 10;
Adafruit_NeoPixel bottomLeds(BOTTOM_LED_COUNT, BOTTOM_LED_PIN, NEO_GRB + NEO_KHZ800);
uint32_t lastAlarmLedUpdate = 0;
uint32_t alarmLightEventStarted = 0;
bool alarmLedsOn = false;
uint32_t lastClockDraw = 0, lastAnim = 0;
int lastMinute = -1;
static constexpr uint8_t MATRIX_COLUMNS = 40;
static constexpr uint8_t MATRIX_MAX_ROWS = 32;
float matrixHead[MATRIX_COLUMNS];
float matrixSpeed[MATRIX_COLUMNS];
uint8_t matrixLength[MATRIX_COLUMNS];
uint8_t matrixGlyphs[MATRIX_COLUMNS][MATRIX_MAX_ROWS];
bool matrixActive[MATRIX_COLUMNS];
uint32_t lastMatrixFrame = 0;
uint8_t matrixRainSpeed = 25;       // 10 = calm, 100 = fast
uint8_t matrixRainDensity = 35;     // percentage of visible columns
uint8_t matrixGlyphScale = 1;       // 1x or 2x Source Han glyphs
uint32_t matrixRainColor = 0x00E86B;
uint8_t matrixGlassOpacity = 58;    // dithered black veil over the time card
static const char* const MATRIX_GLYPH_SET[] = {
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
  "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", "M", "N", "P", "Q", "R", "S", "T", "V", "W", "X", "Y", "Z",
  "@", "#", "$", "%", "&", "*", "+", "-", "=", "!", "?", "/", "\\", "<", ">", "[", "]", "{", "}", "~", "^", "|", ":", ";"
};
static constexpr uint8_t MATRIX_GLYPH_COUNT = sizeof(MATRIX_GLYPH_SET) / sizeof(MATRIX_GLYPH_SET[0]);

static constexpr uint16_t BG = 0x0000;
static constexpr uint16_t FG = 0xF79E;
static constexpr uint16_t ACCENT = 0xD229;
static constexpr uint16_t UI_BLUE = 0x2310;
static constexpr uint16_t PANEL = 0x2124;

void useUIFont(uint8_t scale = 1) {
  M5.Display.setFont(&SourceHanSansTC_UI8pt8b);
  M5.Display.setTextSize(scale);
}

void useUIMediumFont() {
  M5.Display.setFont(&SourceHanSansTC_UI14pt8b);
  M5.Display.setTextSize(1);
}

void useUILargeFont() {
  M5.Display.setFont(&SourceHanSansTC_Medium28pt7b);
  M5.Display.setTextSize(1);
}

void haptic(uint8_t ms = 25) {
  M5.Power.setVibration(180);
  delay(ms);
  M5.Power.setVibration(0);
}

void drawBottomBar(const char* left, const char* middle, const char* right) {
  M5.Display.fillRect(0, 215, 320, 25, BG);
  useUIFont(1);
  M5.Display.setTextColor(ACCENT, BG);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(left, 53, 228);
  M5.Display.drawString(middle, 160, 228);
  M5.Display.drawString(right, 267, 228);
}

void title(const char* text) {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(UI_BLUE, TFT_WHITE);
  useUIMediumFont();
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(text, 10, 8);
}

void saveSettings() {
  prefs.begin("spaceclock", false);
  prefs.putUChar("tzCity", timeZoneIndex);
  prefs.putUChar("face", static_cast<uint8_t>(clockFace));
  prefs.putUChar("matrixSpd", matrixRainSpeed);
  prefs.putUChar("matrixDen", matrixRainDensity);
  prefs.putUChar("matrixSize", matrixGlyphScale);
  prefs.putUInt("matrixColor", matrixRainColor);
  prefs.putUChar("matrixGlass", matrixGlassOpacity);
  prefs.putBool("autoBright", adaptiveBrightness);
  prefs.putUChar("dayBright", dayBrightness);
  prefs.putUChar("nightBright", nightBrightness);
  prefs.putUChar("alarmVol", alarmVolume);
  prefs.putUChar("alarmSound", alarmSound);
  prefs.putBool("time24", use24HourTime);
  prefs.putBool("flatBtns", flatVirtualButtonsEnabled);
  prefs.putUShort("screenOff", screenOffSeconds);
  prefs.putBool("fwAuto", automaticFirmwareUpdate);
  prefs.putUChar("fwHour", firmwareCheckHour);
  prefs.putBool("medSound", meditationSoundEnabled);
  prefs.putUShort("medP1", meditationPresetMinutes[0]);
  prefs.putUShort("medP2", meditationPresetMinutes[1]);
  prefs.putUChar("medStartS", meditationStartSound);
  prefs.putUChar("medEndS", meditationEndSound);
  prefs.putUChar("medStartV", meditationStartVolume);
  prefs.putUChar("medEndV", meditationEndVolume);
  prefs.putBool("medLight", meditationLightEnabled);
  prefs.putBool("medNoise", meditationNoiseEnabled);
  prefs.putUChar("medNoiseS", meditationNoise);
  prefs.putUChar("medNoiseV", meditationNoiseVolume);
  prefs.putBool("alarmLight", alarmLightEnabled);
  prefs.putUInt("alarmColor", alarmLightColor);
  prefs.putUChar("alarmLBri", alarmLightBrightness);
  prefs.putUChar("alarmLMode", alarmLightMode);
  prefs.putBool("nightLight", nightLightEnabled);
  prefs.putUInt("nightColor", nightLightColor);
  prefs.putUChar("nightBri", nightLightBrightness);
  prefs.putUChar("nightMode", nightLightMode);
  prefs.putUShort("nightSecs", nightLightSeconds);
  prefs.putBool("mqttOn", mqttEnabled);
  prefs.putString("mqttHost", mqttHost);
  prefs.putUShort("mqttPort", mqttPort);
  prefs.putString("mqttUser", mqttUsername);
  prefs.putString("mqttPass", mqttPassword);
  prefs.putString("mqttTopic", mqttBaseTopic);
  for (int i = 0; i < COMPANION_PAGE_COUNT; ++i) {
    String hostKey = "compH" + String(i), portKey = "compP" + String(i);
    prefs.putString(hostKey.c_str(), companionHosts[i]);
    prefs.putUShort(portKey.c_str(), companionPorts[i]);
    String remoteKey = "compR" + String(i);
    prefs.putString(remoteKey.c_str(), companionInternetUrls[i]);
    String nameKey = "compN" + String(i);
    prefs.putString(nameKey.c_str(), companionNames[i]);
  }
  for (int i = 0; i < SAVED_WIFI_COUNT; ++i) {
    String ssidKey = "wifiS" + String(i), passwordKey = "wifiP" + String(i);
    prefs.putString(ssidKey.c_str(), savedWifiSsids[i]);
    prefs.putString(passwordKey.c_str(), savedWifiPasswords[i]);
  }
  prefs.putBytes("alarms", alarms, sizeof(alarms));
  prefs.putUChar("alarmRev", ALARM_SCHEMA_VERSION);
  prefs.end();
  mqttSettingsDirty = true;
  mqttDiscoveryDirty = true;
}

void loadSettings() {
  prefs.begin("spaceclock", true);
  timeZoneIndex = prefs.getUChar("tzCity", 18);
  if (timeZoneIndex >= TIME_ZONE_COUNT) timeZoneIndex = 18;
  uint8_t savedFace = prefs.getUChar("face", 0);
  clockFace = savedFace <= static_cast<uint8_t>(ClockFace::Matrix) ? static_cast<ClockFace>(savedFace) : ClockFace::Space;
  matrixRainSpeed = constrain((int)prefs.getUChar("matrixSpd", 25), 10, 100);
  matrixRainDensity = constrain((int)prefs.getUChar("matrixDen", 35), 10, 100);
  matrixGlyphScale = constrain((int)prefs.getUChar("matrixSize", 1), 1, 2);
  matrixRainColor = prefs.getUInt("matrixColor", 0x00E86B) & 0xFFFFFF;
  matrixGlassOpacity = constrain((int)prefs.getUChar("matrixGlass", 58), 15, 90);
  adaptiveBrightness = prefs.getBool("autoBright", true);
  dayBrightness = constrain((int)prefs.getUChar("dayBright", 80), 10, 100);
  nightBrightness = constrain((int)prefs.getUChar("nightBright", 20), 5, 100);
  alarmVolume = constrain((int)prefs.getUChar("alarmVol", 80), 10, 100);
  alarmSound = constrain((int)prefs.getUChar("alarmSound", 0), 0, 3);
  use24HourTime = prefs.getBool("time24", true);
  flatVirtualButtonsEnabled = prefs.getBool("flatBtns", false);
  screenOffSeconds = prefs.getUShort("screenOff", 300);
  automaticFirmwareUpdate = prefs.getBool("fwAuto", false);
  firmwareCheckHour = constrain((int)prefs.getUChar("fwHour", 3), 0, 23);
  meditationSoundEnabled = prefs.getBool("medSound", true);
  meditationPresetMinutes[0] = constrain((int)prefs.getUShort("medP1", 5), 1, 180);
  meditationPresetMinutes[1] = constrain((int)prefs.getUShort("medP2", 15), 1, 180);
  meditationStartSound = constrain((int)prefs.getUChar("medStartS", 1), 0, 3);
  meditationEndSound = constrain((int)prefs.getUChar("medEndS", 1), 0, 3);
  meditationStartVolume = constrain((int)prefs.getUChar("medStartV", 55), 5, 100);
  meditationEndVolume = constrain((int)prefs.getUChar("medEndV", 70), 5, 100);
  meditationLightEnabled = prefs.getBool("medLight", true);
  meditationNoiseEnabled = prefs.getBool("medNoise", false);
  meditationNoise = constrain((int)prefs.getUChar("medNoiseS", 0), 0, 2);
  meditationNoiseVolume = constrain((int)prefs.getUChar("medNoiseV", 25), 5, 80);
  alarmLightEnabled = prefs.getBool("alarmLight", true);
  alarmLightColor = prefs.getUInt("alarmColor", 0xFFFFFF) & 0xFFFFFF;
  alarmLightBrightness = constrain((int)prefs.getUChar("alarmLBri", 35), 1, 100);
  alarmLightMode = constrain((int)prefs.getUChar("alarmLMode", 0), 0, 3);
  nightLightEnabled = prefs.getBool("nightLight", false);
  nightLightColor = prefs.getUInt("nightColor", 0xFFF0C8) & 0xFFFFFF;
  nightLightBrightness = constrain((int)prefs.getUChar("nightBri", 18), 1, 100);
  nightLightMode = constrain((int)prefs.getUChar("nightMode", 0), 0, 2);
  nightLightSeconds = constrain((int)prefs.getUShort("nightSecs", 60), 5, 3600);
  mqttEnabled = prefs.getBool("mqttOn", false);
  mqttHost = prefs.getString("mqttHost", "");
  mqttPort = prefs.getUShort("mqttPort", 1883);
  mqttUsername = prefs.getString("mqttUser", "");
  mqttPassword = prefs.getString("mqttPass", "");
  mqttBaseTopic = prefs.getString("mqttTopic", "spaceclock/core2");
  String legacyCompanionHost = prefs.getString("compHost", "");
  uint16_t legacyCompanionPort = prefs.getUShort("compPort", 16622);
  for (int i = 0; i < COMPANION_PAGE_COUNT; ++i) {
    String hostKey = "compH" + String(i), portKey = "compP" + String(i);
    companionHosts[i] = prefs.getString(hostKey.c_str(), i == 0 ? legacyCompanionHost.c_str() : "");
    companionPorts[i] = prefs.getUShort(portKey.c_str(), i == 0 ? legacyCompanionPort : 16622);
    String remoteKey = "compR" + String(i);
    companionInternetUrls[i] = prefs.getString(remoteKey.c_str(), "");
    String nameKey = "compN" + String(i);
    companionNames[i] = prefs.getString(nameKey.c_str(), ("Page " + String(i + 1)).c_str());
    if (!companionInternetUrls[i].length() && (companionHosts[i].startsWith("http://") || companionHosts[i].startsWith("https://") || companionHosts[i].startsWith("ws://") || companionHosts[i].startsWith("wss://"))) {
      companionInternetUrls[i] = companionHosts[i];
      companionHosts[i] = "";
    }
  }
  for (int i = 0; i < SAVED_WIFI_COUNT; ++i) {
    String ssidKey = "wifiS" + String(i), passwordKey = "wifiP" + String(i);
    savedWifiSsids[i] = prefs.getString(ssidKey.c_str(), "");
    savedWifiPasswords[i] = prefs.getString(passwordKey.c_str(), "");
  }
  size_t alarmBytes = prefs.getBytesLength("alarms");
  uint8_t alarmRevision = prefs.getUChar("alarmRev", 0);
  if (alarmBytes == sizeof(alarms)) {
    prefs.getBytes("alarms", alarms, sizeof(alarms));
  } else if (alarmBytes == sizeof(Alarm) * 4) {
    // Preserve the four alarms saved by earlier native builds.
    prefs.getBytes("alarms", alarms, sizeof(Alarm) * 4);
  }
  prefs.end();
  if (alarmRevision < ALARM_SCHEMA_VERSION) {
    for (int i = 0; i < ALARM_COUNT; ++i) alarms[i].lastDay = -1;
    saveSettings();
  }
}

bool connectSavedWifi(uint32_t perNetworkTimeoutMs = 6000) {
  struct Candidate { int slot; int rssi; } candidates[SAVED_WIFI_COUNT];
  int candidateCount = 0;
  int found = WiFi.scanNetworks(false, true);
  for (int slot = 0; slot < SAVED_WIFI_COUNT; ++slot) {
    if (!savedWifiSsids[slot].length()) continue;
    int bestRssi = -1000;
    for (int network = 0; network < found; ++network) {
      if (WiFi.SSID(network) == savedWifiSsids[slot]) bestRssi = max(bestRssi, (int)WiFi.RSSI(network));
    }
    if (bestRssi > -1000) candidates[candidateCount++] = {slot, bestRssi};
  }
  WiFi.scanDelete();
  for (int i = 0; i < candidateCount; ++i) {
    for (int j = i + 1; j < candidateCount; ++j) {
      if (candidates[j].rssi > candidates[i].rssi) { Candidate swap = candidates[i]; candidates[i] = candidates[j]; candidates[j] = swap; }
    }
  }
  for (int i = 0; i < candidateCount; ++i) {
    int slot = candidates[i].slot;
    WiFi.begin(savedWifiSsids[slot].c_str(), savedWifiPasswords[slot].c_str());
    if (WiFi.waitForConnectResult(perNetworkTimeoutMs) == WL_CONNECTED) return true;
  }
  return false;
}

void maintainSavedWifi(uint32_t nowMs) {
  if (WiFi.status() == WL_CONNECTED || nowMs - lastWifiReconnectAttempt < 20000UL) return;
  lastWifiReconnectAttempt = nowMs;
  connectSavedWifi(3500);
  if (WiFi.status() == WL_CONNECTED && !settingsServerReady) { setupSettingsServer(); syncTime(); }
}

void syncTime() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTzTime(TIME_ZONES[timeZoneIndex].rule, "pool.ntp.org", "time.cloudflare.com");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    m5::rtc_datetime_t dt;
    dt.date.year = t.tm_year + 1900;
    dt.date.month = t.tm_mon + 1;
    dt.date.date = t.tm_mday;
    dt.date.weekDay = t.tm_wday;
    dt.time.hours = t.tm_hour;
    dt.time.minutes = t.tm_min;
    dt.time.seconds = t.tm_sec;
    M5.Rtc.setDateTime(&dt);
  }
}

void getClockDateTime(m5::rtc_datetime_t* dt) {
  struct tm t;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&t, 20)) {
    dt->date.year = t.tm_year + 1900;
    dt->date.month = t.tm_mon + 1;
    dt->date.date = t.tm_mday;
    dt->date.weekDay = t.tm_wday;
    dt->time.hours = t.tm_hour;
    dt->time.minutes = t.tm_min;
    dt->time.seconds = t.tm_sec;
  } else {
    M5.Rtc.getDateTime(dt);
  }
}

void applyDisplayBrightness(const m5::rtc_datetime_t& dt) {
  if (screenSleeping) return;
  uint8_t percent = dayBrightness;
  if (adaptiveBrightness && (dt.time.hours < 7 || dt.time.hours >= 21)) percent = nightBrightness;
  M5.Display.setBrightness((uint8_t)(percent * 255 / 100));
}

void wakeDisplay() {
  if (!screenSleeping) return;
  screenSleeping = false;
  lastUserActivity = millis();
  motionBaselineReady = false;
  m5::rtc_datetime_t wakeTime;
  getClockDateTime(&wakeTime);
  applyDisplayBrightness(wakeTime);
}

void checkMotionWake(uint32_t nowMs) {
  if (!screenSleeping || !M5.Imu.isEnabled() || nowMs - lastMotionSample < 100) return;
  lastMotionSample = nowMs;
  M5.Imu.update();
  float ax, ay, az, gx, gy, gz;
  if (!M5.Imu.getAccel(&ax, &ay, &az) || !M5.Imu.getGyro(&gx, &gy, &gz)) return;
  if (!motionBaselineReady) {
    previousAccelX = ax; previousAccelY = ay; previousAccelZ = az;
    motionBaselineReady = true;
    return;
  }
  float accelerationChange = fabsf(ax - previousAccelX) + fabsf(ay - previousAccelY) + fabsf(az - previousAccelZ);
  float rotationSpeed = fabsf(gx) + fabsf(gy) + fabsf(gz);
  previousAccelX = ax; previousAccelY = ay; previousAccelZ = az;
  // Ignore the first half-second after dimming and normal sensor noise, while
  // still reacting quickly when the Core2 is picked up or tilted.
  if (nowMs - screenSleepStarted >= 500 && (accelerationChange >= 0.10f || rotationSpeed >= 24.0f)) wakeDisplay();
}

void drawClockStatus(uint16_t background) {
  bool transparentMatrix = clockFace == ClockFace::Matrix && background == TFT_BLACK;
  if (!transparentMatrix) M5.Display.fillRect(0, 0, 320, 29, background);
  int level = constrain(M5.Power.getBatteryLevel(), 0, 100);
  bool charging = M5.Power.isCharging();
  uint16_t themeColor = matrixColor(100);
  uint16_t batteryColor = level <= 20 ? TFT_RED : (clockFace == ClockFace::Matrix ? themeColor : (charging ? TFT_GREEN : TFT_WHITE));
  M5.Display.setTextDatum(top_left);
  useUIFont(1);
  if (transparentMatrix) M5.Display.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED);
  else M5.Display.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, background);
  String label = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi offline";
  M5.Display.drawString(label, 5, 7);
  M5.Display.setTextDatum(top_right);
  useUIFont(1);
  if (transparentMatrix) M5.Display.setTextColor(batteryColor);
  else M5.Display.setTextColor(batteryColor, background);
  M5.Display.drawString(String(level) + "%", 278, 9);
  M5.Display.drawRoundRect(282, 5, 32, 19, 4, batteryColor);
  M5.Display.fillRoundRect(285, 8, max(2, level * 25 / 100), 13, 2, batteryColor);
  M5.Display.fillRect(314, 10, 4, 9, batteryColor);
  if (charging) {
    uint16_t boltColor = clockFace == ClockFace::Matrix ? themeColor : TFT_YELLOW;
    M5.Display.fillTriangle(299, 6, 293, 15, 299, 15, boltColor);
    M5.Display.fillTriangle(299, 13, 305, 13, 297, 23, boltColor);
  }
  M5.Display.setTextDatum(top_left);
}

void drawGearNavigationIcon(int cx, int cy) {
  M5.Display.drawCircle(cx, cy, 8, ACCENT);
  M5.Display.drawCircle(cx, cy, 7, ACCENT);
  M5.Display.fillCircle(cx, cy, 3, ACCENT);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4.0f;
    int x1 = cx + lroundf(cosf(a) * 8), y1 = cy + lroundf(sinf(a) * 8);
    int x2 = cx + lroundf(cosf(a) * 11), y2 = cy + lroundf(sinf(a) * 11);
    M5.Display.drawLine(x1, y1, x2, y2, ACCENT);
    M5.Display.fillCircle(x2, y2, 1, ACCENT);
  }
}

void drawClockNavigationIcon(int cx, int cy) {
  M5.Display.drawCircle(cx, cy, 10, TFT_WHITE);
  M5.Display.drawCircle(cx, cy, 9, TFT_WHITE);
  M5.Display.drawLine(cx, cy, cx, cy - 6, TFT_WHITE);
  M5.Display.drawLine(cx, cy, cx + 5, cy + 3, TFT_WHITE);
  M5.Display.fillCircle(cx, cy, 2, ACCENT);
}

void drawClockNavigationIcons() {
  if (clockFace != ClockFace::Matrix) M5.Display.fillRect(0, 215, 320, 25, BG);
  // Both supplied bitmaps are normalized to a 24 × 24 visible canvas.
  M5.Display.drawPng(nav_companion_png, nav_companion_png_len, 41, 215);
  M5.Display.drawPng(nav_meditation_png, nav_meditation_png_len, 148, 215);
  // Vector icons remain crisp at this small size and match the 22 px visual weight.
  drawGearNavigationIcon(267, 227);
}

void resetMatrixRain() {
  const int glyphHeight = 8 * matrixGlyphScale;
  const int usableColumns = min((int)MATRIX_COLUMNS, 320 / glyphHeight);
  const int rows = min((int)MATRIX_MAX_ROWS, 240 / glyphHeight + 2);
  for (int i = 0; i < MATRIX_COLUMNS; ++i) {
    matrixActive[i] = i < usableColumns && (esp_random() % 100) < matrixRainDensity;
    matrixHead[i] = -((float)(esp_random() % rows));
    // Match the CM4 renderer: independent columns, with a broad speed range
    // and a visibly different trail length for each stream.
    matrixSpeed[i] = 1.0f + (esp_random() % 100) / 100.0f * 2.5f;
    matrixLength[i] = 5 + (esp_random() % max(2, rows - 4));
    for (int row = 0; row < MATRIX_MAX_ROWS; ++row) matrixGlyphs[i][row] = esp_random() % MATRIX_GLYPH_COUNT;
  }
  // Keep a few streams at the lowest density so the background never dies.
  for (int i = 0; i < min(3, usableColumns); ++i) matrixActive[i] = true;
  lastMatrixFrame = 0;
}

uint16_t matrixColor(uint8_t strength) {
  uint8_t r = ((matrixRainColor >> 16) & 255) * strength / 100;
  uint8_t g = ((matrixRainColor >> 8) & 255) * strength / 100;
  uint8_t b = (matrixRainColor & 255) * strength / 100;
  return M5.Display.color565(r, g, b);
}

uint16_t blendMatrixGlass(uint16_t base, uint8_t opacity) {
  // Blend each 565 component with a very dark, cool phosphor glass colour.
  // Unlike the old binary dither, every slider position is visibly distinct.
  uint8_t r = (base >> 11) & 0x1F;
  uint8_t g = (base >> 5) & 0x3F;
  uint8_t b = base & 0x1F;
  uint8_t keep = 100 - opacity;
  r = (r * keep + 2 * opacity) / 100;
  g = (g * keep + 8 * opacity) / 100;
  b = (b * keep + 5 * opacity) / 100;
  return (r << 11) | (g << 5) | b;
}

void drawMatrixGlassPanel(M5Canvas& canvas) {
  const int x0 = 42, y0 = 72, width = 236, height = 113;
  // Blend the actual already-drawn rain, in coarse 2x2 samples. Sampling a
  // neighbouring pixel gives the surface its diffuse/frosted texture; the
  // opacity slider therefore directly controls how much rain is visible.
  for (int y = y0 + 5; y < y0 + height - 5; y += 2) {
    for (int x = x0 + 5; x < x0 + width - 5; x += 2) {
      int sx = min(x0 + width - 6, x + (((x + y) & 2) ? 2 : 0));
      int sy = min(y0 + height - 6, y + (((x ^ y) & 2) ? 2 : 0));
      canvas.fillRect(x, y, 2, 2, blendMatrixGlass(canvas.readPixel(sx, sy), matrixGlassOpacity));
    }
  }
  canvas.drawRoundRect(x0, y0, width, height, 10, matrixColor(72));
  canvas.drawRoundRect(x0 + 2, y0 + 2, width - 4, height - 4, 8, matrixColor(25));
}

void drawMatrixClockPanel(M5Canvas& canvas) {
  m5::rtc_datetime_t dt; getClockDateTime(&dt);
  char buf[24];
  drawMatrixGlassPanel(canvas);
  int shownHour = use24HourTime ? dt.time.hours : (dt.time.hours % 12 ? dt.time.hours % 12 : 12);
  snprintf(buf, sizeof(buf), "%02d:%02d", shownHour, dt.time.minutes);
  // The reference look uses a heavy white time with a quiet phosphor shadow,
  // while secondary information stays in the selected rain colour.
  canvas.setFont(&SourceHanSansTC_Medium28pt7b); canvas.setTextSize(1);
  canvas.setTextDatum(middle_center); canvas.setTextColor(matrixColor(38));
  canvas.drawString(buf, 162, 114);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(buf, 160, 112);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
  canvas.setTextColor(matrixColor(92)); canvas.setFont(&SourceHanSansTC_UI14pt8b); canvas.setTextSize(1);
  canvas.drawString(buf, 160, 160);
  canvas.setTextDatum(top_left);
}

void drawMatrixStatus(M5Canvas& canvas) {
  int level = constrain(M5.Power.getBatteryLevel(), 0, 100);
  bool charging = M5.Power.isCharging();
  uint16_t theme = matrixColor(100);
  uint16_t battery = level <= 20 ? TFT_RED : theme;
  canvas.setTextDatum(top_left); canvas.setFont(&SourceHanSansTC_UI8pt8b); canvas.setTextSize(1);
  canvas.setTextColor(WiFi.status() == WL_CONNECTED ? theme : TFT_RED);
  canvas.drawString(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi offline", 5, 7);
  canvas.setTextDatum(top_right); canvas.setTextColor(battery);
  canvas.drawString(String(level) + "%", 278, 9);
  canvas.drawRoundRect(282, 5, 32, 19, 4, battery);
  canvas.fillRoundRect(285, 8, max(2, level * 25 / 100), 13, 2, battery);
  canvas.fillRect(314, 10, 4, 9, battery);
  if (charging) {
    canvas.fillTriangle(299, 6, 293, 15, 299, 15, theme);
    canvas.fillTriangle(299, 13, 305, 13, 297, 23, theme);
  }
  canvas.setTextDatum(top_left);
}

void drawMatrixGearNavigationIcon(M5Canvas& canvas, int cx, int cy) {
  uint16_t theme = matrixColor(100);
  canvas.drawCircle(cx, cy, 8, theme); canvas.drawCircle(cx, cy, 7, theme); canvas.fillCircle(cx, cy, 3, theme);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4.0f;
    int x1 = cx + lroundf(cosf(a) * 8), y1 = cy + lroundf(sinf(a) * 8);
    int x2 = cx + lroundf(cosf(a) * 11), y2 = cy + lroundf(sinf(a) * 11);
    canvas.drawLine(x1, y1, x2, y2, theme); canvas.fillCircle(x2, y2, 1, theme);
  }
}

void drawMatrixNavigationIcons(M5Canvas& canvas) {
  canvas.drawPng(nav_companion_png, nav_companion_png_len, 41, 215);
  canvas.drawPng(nav_meditation_png, nav_meditation_png_len, 148, 215);
  drawMatrixGearNavigationIcon(canvas, 267, 227);
}

void drawMatrixRainFrame(uint32_t nowMs) {
  if (clockFace != ClockFace::Matrix || screenNow != Screen::Clock || alarmActive >= 0) return;
  uint32_t frameInterval = 100;  // CM4 uses 10 fps for the live rain layer.
  if (nowMs - lastMatrixFrame < frameInterval) return;
  float dt = lastMatrixFrame ? (nowMs - lastMatrixFrame) / 1000.0f : frameInterval / 1000.0f;
  if (dt > 0.25f) dt = 0.25f;
  lastMatrixFrame = nowMs;
  const int glyphSize = 8 * matrixGlyphScale;
  const int rows = min((int)MATRIX_MAX_ROWS, 240 / glyphSize + 2);
  matrixCanvas.fillSprite(TFT_BLACK);
  matrixCanvas.setTextDatum(top_left);
  matrixCanvas.setFont(&SourceHanSansTC_UI8pt8b);
  matrixCanvas.setTextSize(matrixGlyphScale);
  for (int i = 0; i < MATRIX_COLUMNS; ++i) {
    if (!matrixActive[i]) {
      // Dormant lanes periodically return from above the screen. This makes
      // density a living distribution rather than a one-time random choice.
      if ((esp_random() % 1000) < matrixRainDensity * 3) {
        matrixActive[i] = true;
        matrixHead[i] = -((float)(esp_random() % max(1, rows / 2)));
      } else continue;
    }
    int x = i * glyphSize;
    int before = (int)matrixHead[i];
    // User speed maps to 1.5–11 cells/s: calm by default, never a blur.
    matrixHead[i] += matrixSpeed[i] * (0.7f + matrixRainSpeed * 0.095f) * dt;
    int head = (int)matrixHead[i];
    if (head != before && head >= 0) matrixGlyphs[i][head % MATRIX_MAX_ROWS] = esp_random() % MATRIX_GLYPH_COUNT;
    // A little character flicker inside a tail gives the rain its living look.
    if ((esp_random() % 100) < 12) matrixGlyphs[i][esp_random() % rows] = esp_random() % MATRIX_GLYPH_COUNT;
    for (int trail = 0; trail < matrixLength[i]; ++trail) {
      int row = head - trail;
      if (row < 0 || row >= rows) continue;
      uint8_t strength = trail == 0 ? 100 : max(7, 78 - (trail * 72 / max(1, (int)matrixLength[i] - 1)));
      uint16_t color = trail == 0 ? M5.Display.color565(205, 255, 215) : matrixColor(strength);
      matrixCanvas.setTextColor(color);
      matrixCanvas.drawString(MATRIX_GLYPH_SET[matrixGlyphs[i][row % MATRIX_MAX_ROWS]], x, row * glyphSize);
    }
    if (matrixHead[i] - matrixLength[i] > rows) {
      matrixHead[i] = -((float)(esp_random() % max(1, rows / 2)));
      matrixSpeed[i] = 1.0f + (esp_random() % 100) / 100.0f * 2.5f;
      matrixLength[i] = 5 + (esp_random() % max(2, rows - 4));
      // Permanent seed streams prevent the display becoming empty after a
      // full cycle; other columns continue to enter and leave at the chosen
      // density, just like the reference CodeRain implementation.
      matrixActive[i] = i < 3 || (esp_random() % 100) < matrixRainDensity;
    }
  }
  drawMatrixClockPanel(matrixCanvas);
  drawMatrixStatus(matrixCanvas);
  drawMatrixNavigationIcons(matrixCanvas);
  matrixCanvas.pushSprite(0, 0);
}

void drawMeditationNavigationIcons() {
  M5.Display.fillRect(0, 215, 320, 25, BG);
  M5.Display.drawPng(nav_companion_png, nav_companion_png_len, 41, 215);
  drawClockNavigationIcon(160, 227);
  drawGearNavigationIcon(267, 227);
}

void drawClockStatic() {
  M5.Display.fillScreen(BG);
  if (clockFace == ClockFace::Space) {
    for (int i = 0; i < 14; ++i) {
      int x = (47 * i + 19) % 315, y = 31 + ((71 * i + 13) % 176);
      uint16_t c = (i % 3 == 0) ? TFT_CYAN : ((i % 3 == 1) ? TFT_WHITE : 0xA51F);
      M5.Display.fillCircle(x, y, i % 4 == 0 ? 2 : 1, c);
      if (i % 5 == 0) { M5.Display.drawFastHLine(x - 3, y, 7, c); M5.Display.drawFastVLine(x, y - 3, 7, c); }
    }
    M5.Display.drawPng(background_png, background_png_len, 120, 0);
    drawClockStatus(BG);
  } else {
    M5.Display.fillRect(0, 0, 320, 215, TFT_BLACK);
    drawClockStatus(TFT_BLACK);
    if (clockFace == ClockFace::Matrix) resetMatrixRain();
  }
  drawClockNavigationIcons();
}

void drawFlipCard(int x, int value) {
  const int y = 57, w = 126, h = 123;
  M5.Display.fillRoundRect(x, y, w, h, 16, 0x2124);
  M5.Display.drawFastHLine(x + 2, y + h / 2, w - 4, TFT_BLACK);
  M5.Display.drawFastHLine(x + 2, y + h / 2 + 2, w - 4, 0x1082);
  char digits[3]; snprintf(digits, sizeof(digits), "%02d", value);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, 0x2124);
  useUILargeFont();
  M5.Display.drawString(digits, x + w / 2, y + h / 2 - 1);
  // Repaint the hinge over the digits for the split-flap appearance.
  M5.Display.drawFastHLine(x + 1, y + h / 2, w - 2, TFT_BLACK);
  M5.Display.drawCircle(x + 4, y + h / 2, 2, 0x4208);
  M5.Display.drawCircle(x + w - 5, y + h / 2, 2, 0x4208);
}

void animateFlipCards(int hour12, int minute) {
  for (int band = 3; band <= 15; band += 3) {
    drawFlipCard(27, hour12); drawFlipCard(167, minute);
    M5.Display.fillRect(29, 118 - band / 2, 122, band, 0x1082);
    M5.Display.fillRect(169, 118 - band / 2, 122, band, 0x1082);
    delay(18);
  }
  for (int band = 12; band >= 3; band -= 3) {
    drawFlipCard(27, hour12); drawFlipCard(167, minute);
    M5.Display.fillRect(29, 118 - band / 2, 122, band, 0x1082);
    M5.Display.fillRect(169, 118 - band / 2, 122, band, 0x1082);
    delay(18);
  }
  drawFlipCard(27, hour12); drawFlipCard(167, minute);
}

void drawClock(bool full = false) {
  if (full) drawClockStatic();
  m5::rtc_datetime_t dt;
  getClockDateTime(&dt);
  char buf[24];
  if (clockFace == ClockFace::Space) {
    // The clock owns x >= 116; the astronaut owns x <= 112. Keeping these
    // regions disjoint prevents the minute repaint from cutting the sprite.
    M5.Display.fillRect(116, 116, 204, 92, BG);
    M5.Display.setTextColor(alarmActive >= 0 ? TFT_RED : FG, BG);
    M5.Display.setTextDatum(top_left);
    useUILargeFont();
    int shownHour = use24HourTime ? dt.time.hours : (dt.time.hours % 12 ? dt.time.hours % 12 : 12);
    snprintf(buf, sizeof(buf), "%02d:%02d", shownHour, dt.time.minutes);
    M5.Display.drawString(buf, 120, 116);
    useUIMediumFont();
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
    M5.Display.drawString(buf, 120, 174);
  } else if (clockFace == ClockFace::Matrix) {
    // Matrix is always composed as one complete sprite frame. This avoids
    // partial redraws that show up as flicker on the Core2 TFT.
    lastMatrixFrame = millis() - 100;
    drawMatrixRainFrame(millis());
  } else {
    static int shownMinute = -1, shownHour = -1, shownDay = -1;
    if (full) shownMinute = shownHour = shownDay = -1;
    int hour12 = use24HourTime ? dt.time.hours : dt.time.hours % 12; if (!use24HourTime && !hour12) hour12 = 12;
    bool hourChanged = shownHour != dt.time.hours;
    bool timeChanged = shownMinute != dt.time.minutes || hourChanged;
    if (timeChanged) {
      if (!full && shownMinute >= 0) animateFlipCards(hour12, dt.time.minutes);
      else { drawFlipCard(27, hour12); drawFlipCard(167, dt.time.minutes); }
      shownMinute = dt.time.minutes; shownHour = dt.time.hours;
      drawClockStatus(TFT_BLACK);
    }
    if (full || shownDay != dt.date.date || hourChanged) {
      const char* days[] = {"星期日 (SUN)", "星期一 (MON)", "星期二 (TUE)", "星期三 (WED)", "星期四 (THU)", "星期五 (FRI)", "星期六 (SAT)"};
      M5.Display.fillRect(0, 29, 320, 28, TFT_BLACK);
      M5.Display.setTextDatum(middle_center); useUIFont(1); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Display.drawString(days[dt.date.weekDay], 160, 41);
      M5.Display.fillRoundRect(3, 151, 22, 27, 5, use24HourTime ? TFT_BLACK : 0x2124);
      M5.Display.setTextDatum(middle_center); useUIFont(1);
      if (!use24HourTime) M5.Display.drawString(dt.time.hours >= 12 ? "PM" : "AM", 14, 164);
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
      M5.Display.fillRect(0, 181, 320, 34, TFT_BLACK);
      M5.Display.setTextDatum(middle_center); useUIMediumFont(); M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      M5.Display.drawString(buf, 160, 197);
      shownDay = dt.date.date;
    }
  }
}

void drawAstronaut() {
  if (alarmActive >= 0) {
    M5.Display.fillRect(0, 0, 320, 215, BG);
    int sx = 0, sy = satelliteTop ? 0 : 143;
    const Asset& sat = satelliteAssets[satelliteTop ? 0 : 1];
    M5.Display.drawPng(sat.data, sat.size, sx, sy);
    M5.Display.drawPng(cosmonaut_1_png, cosmonaut_1_png_len, astronautX, astronautY);
    M5.Display.setTextColor(TFT_RED, BG);
    useUIFont(1);
    M5.Display.drawCentreString("Drag astronaut to satellite to dismiss", 160, 196, 1);
    drawBottomBar("", "Snooze 5m", "");
  } else if (clockFace == ClockFace::Space) {
    // Compose the complete moving region off-screen, then transfer it in one
    // operation. This removes the visible clear/draw flash seen in the video.
    astronautCanvas.fillSprite(BG);
    for (int i = 0; i < 14; ++i) {
      int sx = (47 * i + 19) % 315, sy = 31 + ((71 * i + 13) % 176);
      if (sx >= 5 && sx < 110 && sy >= 85 && sy < 215) {
        uint16_t c = (i % 3 == 0) ? TFT_CYAN : ((i % 3 == 1) ? TFT_WHITE : 0xA51F);
        astronautCanvas.fillCircle(sx - 5, sy - 85, i % 4 == 0 ? 2 : 1, c);
      }
    }
    astronautCanvas.drawPng(cosmonaut_0_png, cosmonaut_0_png_len, astronautX - 5, astronautY - 85);
    astronautCanvas.pushSprite(5, 85);
  }
}

void showMenu() {
  screenNow = Screen::Menu;
  title("Settings");
  useUIFont(1);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  const char* rows[] = {"Wi-Fi & Companion", "Clock faces", "Clock settings", "Alarms", "Meditation settings", "Firmware update"};
  for (int i = 0; i < 6; ++i) {
    M5.Display.fillRoundRect(12, 38 + i * 29, 296, 25, 7, i & 1 ? 0xDEFB : 0xEF7D);
    M5.Display.drawString(rows[i], 25, 42 + i * 29);
  }
  drawBottomBar("", "", "Close");
}

uint16_t rgbHexTo565(const String& value) {
  int p = value.indexOf('#');
  if (p < 0 || value.length() < p + 7) return 0x2945;
  uint32_t rgb = strtoul(value.substring(p + 1, p + 7).c_str(), nullptr, 16);
  return M5.Display.color565((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

String commandValue(const String& line, const char* key) {
  String marker = String(key) + "=";
  int p = line.indexOf(marker);
  if (p < 0) return "";
  p += marker.length();
  bool quoted = p < (int)line.length() && line[p] == '"';
  if (quoted) ++p;
  int e = quoted ? line.indexOf('"', p) : line.indexOf(' ', p);
  if (e < 0) e = line.length();
  return line.substring(p, e);
}

String decodeBase64Text(const String& encoded) {
  if (!encoded.length()) return "";
  size_t required = 0;
  mbedtls_base64_decode(nullptr, 0, &required, (const uint8_t*)encoded.c_str(), encoded.length());
  if (!required || required > 256) return "";
  uint8_t decoded[257] = {0}; size_t written = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &written, (const uint8_t*)encoded.c_str(), encoded.length()) != 0) return "";
  decoded[written] = 0;
  return String((char*)decoded);
}

bool storeCompanionPng(int key, const String& dataUrl) {
  int comma = dataUrl.indexOf(',');
  String encoded = comma >= 0 ? dataUrl.substring(comma + 1) : dataUrl;
  size_t required = 0;
  mbedtls_base64_decode(nullptr, 0, &required, (const uint8_t*)encoded.c_str(), encoded.length());
  if (!required || required > 65536) return false;
  uint8_t* image = (uint8_t*)ps_malloc(required);
  if (!image) image = (uint8_t*)malloc(required);
  if (!image) return false;
  size_t written = 0;
  if (mbedtls_base64_decode(image, required, &written, (const uint8_t*)encoded.c_str(), encoded.length()) != 0) {
    free(image); return false;
  }
  free(companionImages[key].data);
  companionImages[key].data = image;
  companionImages[key].size = written;
  return true;
}

void drawCompanionButton(int key) {
  if (key < 0 || key >= COMPANION_KEYS) return;
  int x = (key % COMPANION_COLS) * 106 + 4, y = (key / COMPANION_COLS) * 104 + 4;
  M5.Display.startWrite();
  M5.Display.fillRect(x, y, 96, 96, BG);
  companionButtonCanvas.fillSprite(BG);
  if (companionImages[key].data && companionImages[key].size) {
      companionButtonCanvas.drawPng(companionImages[key].data, companionImages[key].size,
                                    0, 0, 96, 96, 0, 0, 1.0f, 1.0f);
      // Only soften the extreme corner pixels. Companion supplies its own border,
      // so a larger mask would cut active/pressed outlines off again.
      companionButtonCanvas.drawPixel(0, 0, BG);
      companionButtonCanvas.drawPixel(1, 0, BG);
      companionButtonCanvas.drawPixel(0, 1, BG);
      companionButtonCanvas.drawPixel(95, 0, BG);
      companionButtonCanvas.drawPixel(94, 0, BG);
      companionButtonCanvas.drawPixel(95, 1, BG);
      companionButtonCanvas.drawPixel(0, 95, BG);
      companionButtonCanvas.drawPixel(1, 95, BG);
      companionButtonCanvas.drawPixel(0, 94, BG);
      companionButtonCanvas.drawPixel(95, 95, BG);
      companionButtonCanvas.drawPixel(94, 95, BG);
      companionButtonCanvas.drawPixel(95, 94, BG);
  } else {
    companionButtonCanvas.fillRoundRect(0, 0, 96, 96, 15, companionColors[key]);
    companionButtonCanvas.drawRoundRect(0, 0, 96, 96, 15, 0x632C);
    companionButtonCanvas.setTextDatum(middle_center); companionButtonCanvas.setTextColor(TFT_WHITE);
    companionButtonCanvas.setFont(&SourceHanSansTC_UI8pt8b); companionButtonCanvas.setTextSize(1);
    String label = companionLabels[key]; if (label.length() > 24) label = label.substring(0, 24);
    companionButtonCanvas.drawString(label, 48, 48);
  }
  companionButtonCanvas.pushSprite(x, y);
  M5.Display.endWrite();
}

void drawCompanionPageBar();

void drawCompanionButtons() {
  M5.Display.fillScreen(BG);
  for (int i = 0; i < COMPANION_KEYS; ++i) drawCompanionButton(i);
  drawCompanionPageBar();
}

void drawCompanionPageBar() {
  M5.Display.fillRect(0, 215, 320, 25, BG);
  M5.Display.fillTriangle(45, 227, 59, 219, 59, 235, ACCENT);
  M5.Display.fillTriangle(275, 227, 261, 219, 261, 235, ACCENT);
  String name = companionNames[companionPage];
  if (!name.length()) name = "Page " + String(companionPage + 1);
  if (name.length() > 16) name = name.substring(0, 16);
  useUIFont(1); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(ACCENT, BG);
  M5.Display.drawString(name, 160, 228);
}

void clearCompanionPageData() {
  for (int i = 0; i < COMPANION_KEYS; ++i) {
    companionLabels[i] = "Button " + String(i + 1);
    companionColors[i] = 0x2945;
    free(companionImages[i].data);
    companionImages[i] = {};
  }
}

bool companionConnected() {
  return companionWebSocketMode ? companionWebSocketConnected : companionClient.connected();
}

void sendCompanionMessage(const String& message) {
  if (companionWebSocketMode) {
    if (companionWebSocketConnected) { String payload = message; companionWebSocket.sendTXT(payload); }
  } else if (companionClient.connected()) {
    companionClient.print(message);
  }
}

void stopCompanion() {
  companionClient.stop();
  companionWebSocket.disconnect();
  companionWebSocketConnected = false;
  companionWebSocketMode = false;
  companionUsingInternet = false;
  companionRegistered = false;
}

void processCompanionLine(String line) {
  line.trim();
  if (line.startsWith("PING ")) { sendCompanionMessage("PONG " + line.substring(5) + "\n"); return; }
  if (line.startsWith("BEGIN ")) {
    uint64_t chip = ESP.getEfuseMac();
    char id[17]; snprintf(id, sizeof(id), "%08lx-p%u", (unsigned long)(chip & 0xffffffff), companionPage + 1);
    companionDeviceId = id;
    String add = "ADD-DEVICE DEVICEID=" + String(id) + " SERIAL=core2:" + String(id) +
      " PRODUCT_NAME=\"M5Stack Core2 Page " + String(companionPage + 1) +
      "\" KEYS_TOTAL=6 KEYS_PER_ROW=3 BITMAPS=96 BITMAP_FORMAT=png COLORS=hex TEXT=true TEXT_STYLE=true BRIGHTNESS=false\n";
    sendCompanionMessage(add);
    companionRegistered = true;
    if (screenNow == Screen::Companion) drawCompanionPageBar();
    return;
  }
  if (line.startsWith("KEYS-CLEAR")) {
    for (int i = 0; i < COMPANION_KEYS; ++i) {
      companionLabels[i] = ""; companionColors[i] = TFT_BLACK;
      free(companionImages[i].data); companionImages[i] = {};
    }
    if (screenNow == Screen::Companion) drawCompanionButtons();
    return;
  }
  if (line.startsWith("KEY-STATE")) {
    int key = commandValue(line, "KEY").toInt();
    if (key >= 0 && key < COMPANION_KEYS) {
      String text64 = commandValue(line, "TEXT");
      if (text64.length()) companionLabels[key] = decodeBase64Text(text64);
      String color = commandValue(line, "COLOR");
      if (color.length()) companionColors[key] = rgbHexTo565(color);
      String bitmap = commandValue(line, "BITMAP");
      if (bitmap.length()) storeCompanionPng(key, bitmap);
      if (screenNow == Screen::Companion) drawCompanionButton(key);
    }
  }
}

void onCompanionWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    companionWebSocketConnected = true;
    companionRegistered = false;
    if (screenNow == Screen::Companion) drawCompanionPageBar();
  } else if (type == WStype_DISCONNECTED) {
    companionWebSocketConnected = false;
    companionRegistered = false;
    if (screenNow == Screen::Companion) drawCompanionPageBar();
  } else if (type == WStype_TEXT && payload && length) {
    String incoming((char*)payload, length);
    int start = 0;
    while (start < (int)incoming.length()) {
      int end = incoming.indexOf('\n', start);
      if (end < 0) end = incoming.length();
      if (end > start) processCompanionLine(incoming.substring(start, end));
      start = end + 1;
    }
  }
}

void beginCompanionTarget(String target, bool isInternet) {
  companionUsingInternet = isInternet;
  target.trim();
  bool secure = target.startsWith("https://") || target.startsWith("wss://");
  bool websocket = secure || target.startsWith("http://") || target.startsWith("ws://");
  companionWebSocketMode = websocket;
  if (websocket) {
    int schemeEnd = target.indexOf("://");
    String address = schemeEnd >= 0 ? target.substring(schemeEnd + 3) : target;
    int slash = address.indexOf('/');
    String host = slash >= 0 ? address.substring(0, slash) : address;
    String path = slash >= 0 ? address.substring(slash) : "/satellite";
    if (!path.length() || path == "/") path = "/satellite";
    int colon = host.lastIndexOf(':');
    uint16_t port = secure ? 443 : companionPorts[companionPage];
    if (colon > 0) { port = host.substring(colon + 1).toInt(); host = host.substring(0, colon); }
    companionWebSocket.onEvent(onCompanionWebSocketEvent);
    companionWebSocket.setReconnectInterval(3000);
    companionWebSocket.enableHeartbeat(2000, 1500, 2);
    if (secure) companionWebSocket.beginSSL(host.c_str(), port, path.c_str());
    else companionWebSocket.begin(host.c_str(), port ? port : 80, path.c_str());
  } else {
    while (target.endsWith("/")) target.remove(target.length() - 1);
    companionClient.connect(target.c_str(), companionPorts[companionPage]);
  }
}

void connectCompanion() {
  if (WiFi.status() != WL_CONNECTED || companionConnected()) return;
  if (!companionHosts[companionPage].length() && !companionInternetUrls[companionPage].length()) return;
  stopCompanion();
  companionRegistered = false;
  String local = companionHosts[companionPage]; local.trim();
  if (local.length()) {
    while (local.endsWith("/")) local.remove(local.length() - 1);
    companionWebSocketMode = false;
    companionUsingInternet = false;
    if (companionClient.connect(local.c_str(), companionPorts[companionPage])) return;
  }
  if (companionInternetUrls[companionPage].length()) beginCompanionTarget(companionInternetUrls[companionPage], true);
}

void probeAndPreferLocalCompanion(uint32_t nowMs) {
  if (!companionUsingInternet || !companionWebSocketConnected || !companionHosts[companionPage].length() || nowMs - lastCompanionLocalProbe < 30000UL) return;
  lastCompanionLocalProbe = nowMs;
  WiFiClient probe;
  String local = companionHosts[companionPage]; local.trim();
  bool reachable = probe.connect(local.c_str(), companionPorts[companionPage], 500);
  probe.stop();
  if (reachable) { stopCompanion(); connectCompanion(); }
}

void showCompanion() {
  screenNow = Screen::Companion;
  connectCompanion();
  drawCompanionButtons();
}

void changeCompanionPage(int direction) {
  stopCompanion();
  companionPage = (companionPage + COMPANION_PAGE_COUNT + direction) % COMPANION_PAGE_COUNT;
  clearCompanionPageData();
  drawCompanionButtons();
  connectCompanion();
}

const char* meditationSoundName(uint8_t choice) {
  static const char* names[] = {"Da Ban", "Chime", "Stream", "Water drop"};
  return names[min((int)choice, 3)];
}

void playSoundChoice(uint8_t choice, uint8_t volume, uint32_t repeat = 1) {
  const int16_t* data = nullptr;
  size_t samples = 0;
  switch (choice) {
    case 1:
      data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_chime_sound_raw);
      samples = SpaceClockNative_generated_chime_sound_raw_len / sizeof(int16_t); break;
    case 2:
      data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_stream_sound_raw);
      samples = SpaceClockNative_generated_stream_sound_raw_len / sizeof(int16_t); break;
    case 3:
      data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_drop_sound_raw);
      samples = SpaceClockNative_generated_drop_sound_raw_len / sizeof(int16_t); break;
    default:
      data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_alarm_sound_raw);
      samples = SpaceClockNative_generated_alarm_sound_raw_len / sizeof(int16_t); break;
  }
  M5.Speaker.stop();
  M5.Speaker.setVolume((uint8_t)(constrain((int)volume, 5, 100) * 255 / 100));
  M5.Speaker.playRaw(data, samples, 16000, false, repeat, 0, true);
}

void playMeditationSound(uint8_t choice, uint8_t volume) {
  if (!meditationSoundEnabled) return;
  playSoundChoice(choice, volume, 1);
}

void playMeditationAmbient(uint32_t repeat = UINT32_MAX) {
  if (!meditationNoiseEnabled || meditationState != MeditationState::Running) return;
  const int16_t* data;
  size_t samples;
  if (meditationNoise == 1) {
    data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_rain_sound_raw);
    samples = SpaceClockNative_generated_rain_sound_raw_len / sizeof(int16_t);
  } else if (meditationNoise == 2) {
    data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_insects_sound_raw);
    samples = SpaceClockNative_generated_insects_sound_raw_len / sizeof(int16_t);
  } else {
    data = reinterpret_cast<const int16_t*>(SpaceClockNative_generated_stream_sound_raw);
    samples = SpaceClockNative_generated_stream_sound_raw_len / sizeof(int16_t);
  }
  M5.Speaker.setVolume((uint8_t)(meditationNoiseVolume * 255 / 100));
  M5.Speaker.playRaw(data, samples, 16000, false, repeat, 1, true);
}

uint32_t meditationElapsedSeconds() {
  uint32_t elapsed = meditationElapsedBeforeRun;
  if (meditationState == MeditationState::Running) elapsed += (millis() - meditationRunStarted) / 1000UL;
  return min(elapsed, meditationDurationSeconds);
}

String durationText(uint32_t seconds) {
  char value[16];
  if (seconds >= 3600) snprintf(value, sizeof(value), "%02lu:%02lu:%02lu", (unsigned long)(seconds / 3600), (unsigned long)((seconds / 60) % 60), (unsigned long)(seconds % 60));
  else snprintf(value, sizeof(value), "%02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
  return value;
}

void drawMeditationCard(int index, const String& heading, const String& value, uint16_t color) {
  int x = (index % 3) * 106 + 4, y = (index / 3) * 104 + 4;
  meditationCardCanvas.fillSprite(BG);
  meditationCardCanvas.fillRoundRect(0, 0, 98, 96, 15, color);
  meditationCardCanvas.drawRoundRect(0, 0, 98, 96, 15, 0x632C);
  meditationCardCanvas.setTextDatum(middle_center);
  meditationCardCanvas.setTextColor(TFT_WHITE, color);
  if (value.length()) {
    meditationCardCanvas.setFont(&SourceHanSansTC_UI8pt8b); meditationCardCanvas.setTextSize(1);
    meditationCardCanvas.drawString(heading, 49, 25);
    meditationCardCanvas.setFont(&SourceHanSansTC_UI14pt8b); meditationCardCanvas.setTextSize(1);
    meditationCardCanvas.drawString(value, 49, 62);
  } else {
    meditationCardCanvas.setFont(&SourceHanSansTC_UI8pt8b); meditationCardCanvas.setTextSize(1);
    meditationCardCanvas.drawString(heading, 49, 49);
  }
  meditationCardCanvas.pushSprite(x, y);
}

void drawMeditation(bool full = true) {
  if (screenNow != Screen::Meditation) return;
  M5.Display.startWrite();
  if (full) M5.Display.fillScreen(BG);
  m5::rtc_datetime_t now; getClockDateTime(&now);
  int shownHour = use24HourTime ? now.time.hours : (now.time.hours % 12 ? now.time.hours % 12 : 12);
  char timeText[6]; snprintf(timeText, sizeof(timeText), "%02u:%02u", shownHour, now.time.minutes);
  uint32_t elapsed = meditationElapsedSeconds();
  uint32_t remaining = meditationDurationSeconds > elapsed ? meditationDurationSeconds - elapsed : 0;
  const char* action = meditationState == MeditationState::Running ? "暫停" : (meditationState == MeditationState::Paused ? "繼續" : "開始");
  drawMeditationCard(0, String(timeText), action, meditationState == MeditationState::Running ? 0x03E8 : 0x2124);
  drawMeditationCard(1, "經過多久", durationText(elapsed), 0x2124);
  drawMeditationCard(2, "剩下多久", durationText(remaining), meditationState == MeditationState::Done ? 0xC986 : 0x2124);
  if (full) {
    drawMeditationCard(3, "重新開始", "", 0x2124);
    drawMeditationCard(4, "預設時間1", String(meditationPresetMinutes[0]) + "分", 0x2945);
    drawMeditationCard(5, "預設時間2", String(meditationPresetMinutes[1]) + "分", 0x2945);
    drawMeditationNavigationIcons();
  }
  M5.Display.endWrite();
}

void beginMeditation(uint16_t minutes) {
  meditationDurationSeconds = max(1, (int)minutes) * 60UL;
  meditationElapsedBeforeRun = 0;
  meditationRunStarted = millis();
  meditationLightEventStarted = millis();
  meditationAmbientPendingAt = millis() + 1000UL;
  meditationState = MeditationState::Running;
  playMeditationSound(meditationStartSound, meditationStartVolume);
  drawMeditation();
}

void toggleMeditation() {
  if (meditationState == MeditationState::Ready || meditationState == MeditationState::Done) {
    beginMeditation(max(1, (int)(meditationDurationSeconds / 60UL)));
  } else if (meditationState == MeditationState::Running) {
    meditationElapsedBeforeRun = meditationElapsedSeconds();
    meditationState = MeditationState::Paused;
    M5.Speaker.stop(1);
  } else {
    meditationRunStarted = millis();
    meditationState = MeditationState::Running;
    meditationAmbientPendingAt = millis() + 100UL;
  }
  drawMeditation();
}

void resetMeditation() {
  M5.Speaker.stop();
  meditationAmbientPendingAt = 0;
  meditationElapsedBeforeRun = 0;
  meditationDurationSeconds = meditationPresetMinutes[0] * 60UL;
  meditationState = MeditationState::Ready;
  meditationLightEventStarted = millis();
  drawMeditation();
}

void showMeditation() {
  screenNow = Screen::Meditation;
  if (meditationState == MeditationState::Ready) meditationDurationSeconds = meditationPresetMinutes[0] * 60UL;
  drawMeditation();
}

uint8_t meditationSettingsPage = 0;
void showMeditationSettings() {
  screenNow = Screen::MeditationSettings;
  title("Meditation settings");
  useUIFont(1); M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  String rows[4], values[4];
  if (meditationSettingsPage == 0) {
    rows[0]="Preset time 1"; values[0]=String(meditationPresetMinutes[0])+" min";
    rows[1]="Preset time 2"; values[1]=String(meditationPresetMinutes[1])+" min";
    rows[2]="Sound reminder"; values[2]=meditationSoundEnabled?"ON":"OFF";
    rows[3]="Light effects"; values[3]=meditationLightEnabled?"ON":"OFF";
  } else if (meditationSettingsPage == 1) {
    rows[0]="Start sound"; values[0]=meditationSoundName(meditationStartSound);
    rows[1]="Start volume"; values[1]=String(meditationStartVolume)+"%";
    rows[2]="End sound"; values[2]=meditationSoundName(meditationEndSound);
    rows[3]="End volume"; values[3]=String(meditationEndVolume)+"%";
  } else {
    const char* noiseNames[] = {"Stream", "Rain", "Summer insects"};
    rows[0]="Background sound"; values[0]=meditationNoiseEnabled?"ON":"OFF";
    rows[1]="Soundscape"; values[1]=noiseNames[meditationNoise];
    rows[2]="Background volume"; values[2]=String(meditationNoiseVolume)+"%";
    rows[3]="Preview"; values[3]="Play";
  }
  for (int i=0;i<4;++i) {
    int y=47+i*39; M5.Display.fillRoundRect(10,y,300,32,8,i&1?0xDEFB:0xEF7D);
    M5.Display.setTextDatum(middle_left); M5.Display.drawString(rows[i],20,y+16);
    M5.Display.setTextDatum(middle_right); M5.Display.drawString(values[i],300,y+16);
  }
  drawBottomBar(meditationSettingsPage?"Previous":"", meditationSettingsPage<2?"Next":"", "Close");
}

/* AgentDeck support was intentionally removed. */
#if 0
void drawAgentRobot(int x, int y) {
  M5.Display.fillRoundRect(x + 6, y + 8, 34, 28, 5, 0xFBA0);
  M5.Display.fillRect(x + 2, y + 13, 6, 15, 0xFFE0);
  M5.Display.fillRect(x + 40, y + 13, 6, 15, 0xFFE0);
  M5.Display.fillRoundRect(x + 14, y + 2, 18, 8, 3, TFT_WHITE);
  M5.Display.fillCircle(x + 17, y + 20, 3, 0x2310);
  M5.Display.fillCircle(x + 30, y + 20, 3, 0x2310);
  M5.Display.fillRect(x + 16, y + 31, 4, 9, 0x5DDF);
  M5.Display.fillRect(x + 28, y + 31, 4, 9, 0x5DDF);
}

void drawAgentOctopus(int x, int y) {
  M5.Display.fillCircle(x + 13, y + 12, 12, 0xFBA0);
  M5.Display.fillCircle(x + 7, y + 24, 6, 0xFBA0);
  M5.Display.fillCircle(x + 18, y + 24, 6, 0xFBA0);
  M5.Display.fillCircle(x + 28, y + 21, 5, 0xFBA0);
  M5.Display.fillCircle(x + 9, y + 10, 2, TFT_WHITE);
  M5.Display.fillCircle(x + 18, y + 10, 2, TFT_WHITE);
  M5.Display.drawPixel(x + 9, y + 10, TFT_BLACK);
  M5.Display.drawPixel(x + 18, y + 10, TFT_BLACK);
}

void drawAgentDeck() {
  // Compact terrarium dashboard inspired by AgentDeck's square ESP32 panel.
  M5.Display.fillRect(0, 0, 320, 55, 0x0439);
  M5.Display.fillRect(0, 55, 320, 65, 0x03FB);
  M5.Display.fillRect(0, 120, 320, 55, 0x14D5);
  M5.Display.fillRect(0, 175, 320, 40, 0x7B86);
  for (int x = 0; x < 320; x += 32) M5.Display.drawFastHLine(x, 54 + ((x / 32) & 1) * 2, 24, TFT_CYAN);
  // Sand, distant rocks, sea grass and bubbles.
  M5.Display.fillTriangle(198, 175, 226, 137, 250, 175, 0x52AA);
  M5.Display.fillTriangle(225, 175, 257, 146, 285, 175, 0x4A69);
  M5.Display.drawLine(18, 184, 14, 130, TFT_GREEN); M5.Display.drawLine(14, 153, 7, 144, TFT_GREEN);
  M5.Display.drawLine(300, 185, 305, 125, 0x07EF); M5.Display.drawLine(304, 150, 313, 139, 0x07EF);
  const uint8_t bubbles[][2] = {{55,91},{72,145},{105,105},{181,117},{286,87},{268,131},{42,119}};
  for (auto &b : bubbles) M5.Display.drawCircle(b[0], b[1], 2, TFT_CYAN);

  uint16_t stateColor = agentState == "processing" ? TFT_CYAN :
                        agentState.startsWith("awaiting") ? TFT_ORANGE :
                        agentDeckConnected ? TFT_GREEN : TFT_RED;
  M5.Display.fillRoundRect(7, 7, 132, 67, 8, 0x01CC);
  M5.Display.drawRoundRect(7, 7, 132, 67, 8, TFT_CYAN);
  M5.Display.setTextDatum(top_left); M5.Display.setTextColor(TFT_WHITE); useUIFont(1);
  M5.Display.drawString("AgentDeck", 14, 10);
  M5.Display.setTextColor(stateColor); M5.Display.drawString(agentState, 14, 29);
  String project = agentProject; if (project.length() > 18) project = project.substring(0, 18);
  M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString(project, 14, 47);

  M5.Display.fillRoundRect(226, 7, 87, 67, 8, 0x01CC);
  M5.Display.drawRoundRect(226, 7, 87, 67, 8, TFT_CYAN);
  M5.Display.setTextColor(TFT_WHITE); useUIFont(1);
  M5.Display.drawString("TASK STATUS", 232, 12);
  M5.Display.setTextColor(stateColor); useUIFont(1);
  M5.Display.drawString(agentDeckConnected ? "ONLINE" : "OFFLINE", 232, 28);
  String usage = agentUsage5h >= 0 ? "5h " + String(agentUsage5h) + "%" : "5h --";
  M5.Display.setTextColor(TFT_YELLOW); M5.Display.drawString(usage, 232, 49);

  String task = agentTool.length() ? agentTool : agentProject;
  if (task.length() > 26) task = task.substring(0, 26);
  M5.Display.fillRoundRect(91, 81, 139, 20, 7, 0x2310);
  M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(TFT_WHITE); useUIFont(1);
  M5.Display.drawString(task, 160, 91);
  drawAgentRobot(118, 111);
  drawAgentOctopus(257, 137);
  M5.Display.fillRoundRect(77, 135, 30, 21, 9, 0x5DDF);
  M5.Display.setTextColor(0x0439); M5.Display.drawString("AI", 92, 145);

  const char* labels[] = {"ALLOW", "DENY", "STOP", "REFRESH"};
  const uint16_t colors[] = {0x2646, 0xA145, 0xB800, 0x2310};
  for (int i = 0; i < 4; ++i) {
    int x = i * 79 + 2;
    M5.Display.fillRoundRect(x, 190, 76, 21, 6, colors[i]);
    M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(TFT_WHITE); useUIFont(1);
    M5.Display.drawString(labels[i], x + 38, 200);
  }
  drawBottomBar("", agentDeckConnected ? "Connected" : "Offline", "Close");
}

uint16_t agentWaterColor(int y) {
  return y < 120 ? 0x03FB : (y < 175 ? 0x14D5 : 0x7B86);
}

void animateAgentDeck() {
  static const uint8_t bubbles[][2] = {{50,104},{70,138},{184,128},{290,92},{280,132},{40,122}};
  uint8_t oldStep = agentBubbleStep;
  agentBubbleStep = (agentBubbleStep + 1) % 12;
  for (auto &b : bubbles) {
    int oldY = b[1] - oldStep;
    int newY = b[1] - agentBubbleStep;
    M5.Display.drawCircle(b[0], oldY, 2, agentWaterColor(oldY));
    M5.Display.drawCircle(b[0], newY, 2, TFT_CYAN);
  }
  // A subtle blink makes the central agent feel alive without repainting the page.
  if ((agentBubbleStep % 8) == 0) {
    M5.Display.drawFastHLine(135, 131, 5, 0x2310);
    M5.Display.drawFastHLine(148, 131, 5, 0x2310);
  } else {
    M5.Display.fillCircle(135, 131, 3, 0x2310);
    M5.Display.fillCircle(148, 131, 3, 0x2310);
  }
}

void onAgentDeckEvent(WStype_t type, uint8_t* payload, size_t length) {
  bool changed = false;
  if (type == WStype_CONNECTED) {
    changed = !agentDeckConnected || agentState != "connected";
    agentDeckConnected = true;
    agentState = "connected";
    agentDeckWs.sendTXT("{\"type\":\"query_usage\"}");
    agentDeckWs.sendTXT("{\"type\":\"device_info\",\"board\":\"m5stack-core2\",\"version\":\"space-clock-native\",\"wifiConnected\":true}");
  } else if (type == WStype_DISCONNECTED) {
    changed = agentDeckConnected || agentState != "offline";
    agentDeckConnected = false; agentState = "offline";
  } else if (type == WStype_TEXT) {
    JsonDocument doc;
    if (!deserializeJson(doc, payload, length)) {
      const char* typeName = doc["type"] | "";
      if (!strcmp(typeName, "state_update")) {
        String nextState = String((const char*)(doc["state"] | "idle"));
        String nextProject = String((const char*)(doc["projectName"] | "Agent session"));
        String nextTool = String((const char*)(doc["currentTool"] | ""));
        changed = nextState != agentState || nextProject != agentProject || nextTool != agentTool;
        agentState = nextState; agentProject = nextProject; agentTool = nextTool;
      } else if (!strcmp(typeName, "sessions_list") && doc["sessions"].size() > 0) {
        JsonObject first = doc["sessions"][0];
        String nextProject = String((const char*)(first["project"] | first["projectName"] | "Agent session"));
        String nextState = String((const char*)(first["state"] | "idle"));
        changed = nextProject != agentProject || nextState != agentState;
        agentProject = nextProject; agentState = nextState;
      } else if (!strcmp(typeName, "usage_update")) {
        int next5h = doc["fiveHourPercent"].is<float>() ? constrain((int)round(doc["fiveHourPercent"].as<float>()), 0, 100) : -1;
        int next7d = doc["sevenDayPercent"].is<float>() ? constrain((int)round(doc["sevenDayPercent"].as<float>()), 0, 100) : -1;
        changed = next5h != agentUsage5h || next7d != agentUsage7d;
        agentUsage5h = next5h; agentUsage7d = next7d;
      }
    }
  }
  if (changed && screenNow == Screen::AgentDeck) drawAgentDeck();
}

void connectAgentDeck() {
  // begin() starts an asynchronous connection and installs its own reconnect
  // timer. Calling begin() again while the handshake is in progress aborts the
  // previous attempt and makes the status flash between connected/offline.
  if (!agentDeckHost.length() || WiFi.status() != WL_CONNECTED || agentDeckStarted) return;
  agentDeckToken.trim();
  String path = "/?token=" + agentDeckToken + "&clientType=esp32&board=m5stack_core2";
  agentDeckWs.begin(agentDeckHost.c_str(), agentDeckPort, path.c_str());
  agentDeckWs.onEvent(onAgentDeckEvent);
  agentDeckWs.setReconnectInterval(3000);
  agentDeckWs.enableHeartbeat(10000, 3000, 2);
  agentDeckStarted = true;
  agentState = "connecting";
}

void showAgentDeck() {
  screenNow = Screen::AgentDeck;
  connectAgentDeck();
  drawAgentDeck();
}
#endif

void showFaces() {
  screenNow = Screen::Faces;
  title("Clock faces");
  const char* names[] = {"Space", "Flip clock", "Matrix rain"};
  useUIFont(1);
  for (int i = 0; i < 3; ++i) {
    bool selected = i == static_cast<int>(clockFace);
    M5.Display.fillRoundRect(18, 52 + i * 48, 284, 36, 7, selected ? UI_BLUE : 0xE71C);
    M5.Display.setTextColor(selected ? TFT_WHITE : TFT_BLACK);
    M5.Display.drawString(names[i], 34, 62 + i * 48);
    if (selected) M5.Display.drawString("Selected", 225, 62 + i * 48);
  }
  drawBottomBar("", "", "Close");
}

String weekdaysText(uint8_t mask) {
  if (!mask) return "once";
  const char* n[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
  String s;
  for (int i = 0; i < 7; ++i) if (mask & (1 << i)) { if (s.length()) s += " "; s += n[i]; }
  return s;
}

void showAlarms() {
  screenNow = Screen::Alarms;
  char heading[24];
  snprintf(heading, sizeof(heading), "Alarms %u/%u", alarmPage + 1, ALARM_PAGE_COUNT);
  title(heading);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  for (int row = 0; row < ALARMS_PER_PAGE; ++row) {
    int i = alarmPage * ALARMS_PER_PAGE + row;
    int y = 43 + row * 42;
    M5.Display.drawFastHLine(5, y + 37, 310, 0xBDF7);
    char b[16]; snprintf(b, sizeof(b), "%02d:%02d", alarms[i].hour, alarms[i].minute);
    useUIMediumFont(); M5.Display.drawString(b, 14, y);
    useUIFont(1); M5.Display.drawString(weekdaysText(alarms[i].weekdays), 160, y + 7);
    M5.Display.fillRoundRect(250, y + 3, 50, 25, 12, alarms[i].enabled ? TFT_GREEN : 0xAD55);
    M5.Display.setTextColor(TFT_WHITE); M5.Display.drawCentreString(alarms[i].enabled ? "ON" : "OFF", 275, y + 10, 1);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  }
  drawBottomBar(alarmPage ? "< Previous" : "", alarmPage + 1 < ALARM_PAGE_COUNT ? "Next >" : "", "Close");
}

String screenOffText() {
  if (!screenOffSeconds) return "Never";
  if (screenOffSeconds < 60) return String(screenOffSeconds) + " sec";
  return String(screenOffSeconds / 60) + " min";
}

void cycleScreenOffTime() {
  static constexpr uint16_t choices[] = {0, 30, 60, 300, 600, 1800};
  for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
    if (screenOffSeconds == choices[i]) {
      screenOffSeconds = choices[(i + 1) % (sizeof(choices) / sizeof(choices[0]))];
      return;
    }
  }
  screenOffSeconds = 300;
}

uint8_t clockSettingsPage = 0;
void showSettings() {
  screenNow = Screen::Settings;
  title("Settings");
  useUIFont(1); M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  if (!clockSettingsPage) {
    M5.Display.drawString("Time zone", 14, 42); M5.Display.drawString(TIME_ZONES[timeZoneIndex].city, 174, 42);
    M5.Display.drawString("Auto brightness", 14, 70); M5.Display.drawString(adaptiveBrightness ? "ON" : "OFF", 244, 70);
    M5.Display.drawString("Day brightness", 14, 98); M5.Display.drawString(String(dayBrightness) + "%", 238, 98);
    M5.Display.drawString("Night brightness", 14, 126); M5.Display.drawString(String(nightBrightness) + "%", 238, 126);
    M5.Display.drawString("Alarm volume", 14, 154); M5.Display.drawString(String(alarmVolume) + "%", 238, 154);
    M5.Display.drawString("Screen off", 14, 182); M5.Display.drawString(screenOffText(), 218, 182);
    drawBottomBar("NTP sync", "Next", "Close");
  } else {
    M5.Display.drawString("Time format", 14, 42); M5.Display.drawString(use24HourTime?"24 hour":"12 hour", 220, 42);
    M5.Display.drawString("Flat virtual buttons", 14, 70); M5.Display.drawString(flatVirtualButtonsEnabled?"ON":"OFF", 244, 70);
    M5.Display.drawString("Night light", 14, 98); M5.Display.drawString(nightLightEnabled ? "ON" : "OFF", 244, 98);
    M5.Display.drawString("Color", 14, 126); M5.Display.fillRoundRect(250, 122, 45, 22, 5, M5.Display.color565((nightLightColor>>16)&255,(nightLightColor>>8)&255,nightLightColor&255));
    M5.Display.drawString("LED brightness", 14, 154); M5.Display.drawString(String(nightLightBrightness)+"%", 238, 154);
    const char* modes[] = {"Stay on", "Timed off", "Timed fade"};
    M5.Display.drawString("Mode", 14, 182); M5.Display.drawString(modes[nightLightMode], 205, 182);
    drawBottomBar("Previous", "Save", "Close");
  }
}

int compareFirmwareVersions(const String& left, const String& right) {
  int li = 0, ri = 0;
  for (int part = 0; part < 3; ++part) {
    int le = left.indexOf('.', li); if (le < 0) le = left.length();
    int re = right.indexOf('.', ri); if (re < 0) re = right.length();
    int lv = left.substring(li, le).toInt();
    int rv = right.substring(ri, re).toInt();
    if (lv != rv) return lv < rv ? -1 : 1;
    li = le + 1; ri = re + 1;
  }
  return 0;
}

void showFirmwareUpdate() {
  screenNow = Screen::FirmwareUpdate;
  title("Firmware update");
  useUIFont(1); M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.drawString("Current", 14, 45); M5.Display.drawString(SPACE_CLOCK_VERSION, 190, 45);
  M5.Display.drawString("Latest", 14, 75); M5.Display.drawString(latestFirmwareVersion.length() ? latestFirmwareVersion : "Not checked", 190, 75);
  M5.Display.drawString("Automatic update", 14, 105); M5.Display.drawString(automaticFirmwareUpdate ? "ON" : "OFF", 250, 105);
  char checkTime[8]; snprintf(checkTime, sizeof(checkTime), "%02u:00", firmwareCheckHour);
  M5.Display.drawString("Daily check", 14, 135); M5.Display.drawString(checkTime, 238, 135);
  M5.Display.setTextColor(firmwareUpdateAvailable ? 0x0400 : 0x4208, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  String line1 = firmwareUpdateMessage, line2;
  if (line1.length() > 43) { int split = line1.lastIndexOf(' ', 43); if (split < 15) split = 43; line2 = line1.substring(split + 1); line1 = line1.substring(0, split); }
  M5.Display.drawString(line1, 14, 165); if (line2.length()) M5.Display.drawString(line2, 14, 184);
  drawBottomBar("Check", firmwareUpdateAvailable ? "Install" : "", "Close");
}

bool readFirmwareManifest(bool redraw = true) {
  if (WiFi.status() != WL_CONNECTED) {
    firmwareUpdateMessage = "Wi-Fi is not connected.";
    firmwareUpdateAvailable = false;
    if (redraw) showFirmwareUpdate();
    return false;
  }
  firmwareUpdateMessage = "Checking GitHub...";
  if (redraw) showFirmwareUpdate();
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  if (!http.begin(secure, SPACE_CLOCK_MANIFEST_URL)) {
    firmwareUpdateMessage = "Could not open update service.";
    if (redraw) showFirmwareUpdate();
    return false;
  }
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    firmwareUpdateMessage = "GitHub check failed (" + String(status) + ").";
    http.end();
    if (redraw) showFirmwareUpdate();
    return false;
  }
  DynamicJsonDocument doc(1536);
  DeserializationError error = deserializeJson(doc, http.getString());
  http.end();
  if (error || !doc["version"].is<const char*>() || !doc["url"].is<const char*>()) {
    firmwareUpdateMessage = "Update information is invalid.";
    if (redraw) showFirmwareUpdate();
    return false;
  }
  latestFirmwareVersion = doc["version"].as<String>();
  latestFirmwareUrl = doc["url"].as<String>();
  firmwareUpdateAvailable = compareFirmwareVersions(SPACE_CLOCK_VERSION, latestFirmwareVersion) < 0;
  firmwareUpdateMessage = firmwareUpdateAvailable ? "New firmware is ready. Tap Install." : "This firmware is up to date.";
  if (redraw) showFirmwareUpdate();
  return true;
}

bool installLatestFirmware(bool redraw = true) {
  if (!firmwareUpdateAvailable || !latestFirmwareUrl.length()) return false;
  if (redraw) {
    firmwareUpdateMessage = "Downloading. Keep USB power connected...";
    showFirmwareUpdate();
  }
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(secure, latestFirmwareUrl)) return false;
  int status = http.GET();
  int size = http.getSize();
  if (status != HTTP_CODE_OK || size <= 0 || !Update.begin(size, U_FLASH)) {
    firmwareUpdateMessage = "Download could not start.";
    http.end();
    if (redraw) showFirmwareUpdate();
    return false;
  }
  size_t written = Update.writeStream(http.getStream());
  bool success = written == (size_t)size && Update.end(true);
  http.end();
  if (!success) {
    firmwareUpdateMessage = "Update failed; current firmware is safe.";
    if (redraw) showFirmwareUpdate();
    return false;
  }
  if (redraw) {
    firmwareUpdateMessage = "Installed. Restarting...";
    showFirmwareUpdate();
  }
  delay(800);
  ESP.restart();
  return true;
}

void checkAutomaticFirmwareUpdate(const m5::rtc_datetime_t& now) {
  if (!automaticFirmwareUpdate || WiFi.status() != WL_CONNECTED || now.time.hours != firmwareCheckHour) return;
  int32_t day = now.date.year * 512 + now.date.month * 32 + now.date.date;
  if (day == lastAutomaticUpdateDay) return;
  lastAutomaticUpdateDay = day;
  if (readFirmwareManifest(false) && firmwareUpdateAvailable) installLatestFirmware(false);
}

void showAbout() {
  screenNow = Screen::About;
  title("Space Clock");
  M5.Display.drawPng(cosmonaut_0_png, cosmonaut_0_png_len, 122, 42);
  M5.Display.setTextColor(UI_BLUE, TFT_WHITE); useUIFont(1);
  M5.Display.drawCentreString(SPACE_CLOCK_VERSION, 160, 150, 2);
  M5.Display.drawCentreString("Native firmware for M5Stack Core2", 160, 177, 2);
  drawBottomBar("", "", "Close");
}

void runWifiPortal() {
  if (settingsServerReady) settingsServer.stop();
  title("Wi-Fi setup");
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE); useUIFont(1);
  M5.Display.drawCentreString("Connect to SpaceClock-Setup", 160, 90, 2);
  M5.Display.drawCentreString("and open the captive portal", 160, 115, 2);
  WiFiManager wm;
  char compHost[40] = {0}, compPort[8] = {0};
  companionHosts[0].substring(0, 39).toCharArray(compHost, sizeof(compHost));
  snprintf(compPort, sizeof(compPort), "%u", companionPorts[0]);
  WiFiManagerParameter companionHostField("companion_host", "Companion page 1 host/IP", compHost, 39);
  WiFiManagerParameter companionPortField("companion_port", "Companion page 1 port", compPort, 7);
  wm.addParameter(&companionHostField);
  wm.addParameter(&companionPortField);
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal(SPACE_CLOCK_WIFI_AP);
  companionHosts[0] = companionHostField.getValue();
  companionPorts[0] = max(1, atoi(companionPortField.getValue()));
  saveSettings();
  if (WiFi.status() == WL_CONNECTED) {
    if (settingsServerReady) settingsServer.begin(); else setupSettingsServer();
    syncTime();
  }
  showMenu();
}

void dismissAlarm() {
  m5::rtc_datetime_t now; getClockDateTime(&now);
  alarms[alarmActive].lastDay = now.date.year * 512 + now.date.month * 32 + now.date.date;
  if (!alarms[alarmActive].weekdays) alarms[alarmActive].enabled = false;
  alarmActive = -1;
  astronautDragging = false;
  M5.Speaker.stop();
  astronautX = 6; astronautY = 105;
  astronautDX = 1; astronautDY = 1;
  lastMinute = -1;
  saveSettings();
  drawClock(true); drawAstronaut();
}

void updateAlarmBaseLights(uint32_t nowMs) {
  if (nowMs - lastAlarmLedUpdate < 30) return;
  lastAlarmLedUpdate = nowMs;
  uint32_t color = 0;
  uint8_t strength = 0;
  if (alarmActive >= 0 && alarmLightEnabled) {
    uint32_t age = nowMs - alarmLightEventStarted;
    if (alarmLightMode == 0 || alarmLightMode == 2) {
      if (alarmLightMode == 0 || age < 9000UL) {
        float phase = (age % 3000UL) * (2.0f * PI / 3000.0f);
        strength = 8 + (uint8_t)((1.0f - cosf(phase)) * (alarmLightBrightness - min((int)alarmLightBrightness, 8)) / 2.0f);
      }
    } else if (alarmLightMode == 1 || age < 1800UL) {
      strength = ((age / 180UL) & 1) ? 0 : alarmLightBrightness;
    }
    color = alarmLightColor;
  } else if (meditationLightEnabled && (screenNow == Screen::Meditation || meditationState == MeditationState::Running || meditationState == MeditationState::Paused || meditationState == MeditationState::Done)) {
    uint32_t age = nowMs - meditationLightEventStarted;
    if (meditationState == MeditationState::Ready || meditationState == MeditationState::Paused) {
      color = 0xFFE2A8; strength = 20;
    } else if (meditationState == MeditationState::Running && age < 650UL) {
      color = 0xFFFFFF; strength = age < 320 ? 55 : 0;
    } else if (meditationState == MeditationState::Running) {
      float phase = (nowMs % 4000UL) * (2.0f * PI / 4000.0f);
      color = 0xFFFFFF; strength = 5 + (uint8_t)((1.0f - cosf(phase)) * 17.0f);
    } else if (meditationState == MeditationState::Done && age < 3000UL) {
      color = 0xFFF1D2; strength = ((age / 300UL) & 1) ? 0 : 50;
    }
  } else if (screenSleeping && nightLightEnabled) {
    uint32_t age = nowMs - screenSleepStarted;
    color = nightLightColor;
    if (nightLightMode == 0) strength = nightLightBrightness;
    else if (age < (uint32_t)nightLightSeconds * 1000UL) strength = nightLightBrightness;
    else if (nightLightMode == 2 && age < (uint32_t)nightLightSeconds * 1000UL + 5000UL)
      strength = nightLightBrightness * ((uint32_t)nightLightSeconds * 1000UL + 5000UL - age) / 5000UL;
  }
  uint8_t r = ((color >> 16) & 255) * strength / 100;
  uint8_t g = ((color >> 8) & 255) * strength / 100;
  uint8_t b = (color & 255) * strength / 100;
  for (int i = 0; i < BOTTOM_LED_COUNT; ++i) bottomLeds.setPixelColor(i, r, g, b);
  bottomLeds.show();
  alarmLedsOn = strength > 0;
}

void startAlarm(int index) {
  if (index < 0 || index >= ALARM_COUNT) return;
  wakeDisplay();
  lastUserActivity = millis();
  alarmActive = index;
  alarmLightEventStarted = millis();
  screenNow = Screen::Clock;
  satelliteTop = esp_random() & 1;
  astronautX = 260; astronautY = 100;
  astronautDragging = false;
  playSoundChoice(alarmSound, alarmVolume, UINT32_MAX);
  drawAstronaut();
}

void snoozeAlarm() {
  if (alarmActive < 0) return;
  m5::rtc_datetime_t now; getClockDateTime(&now);
  alarms[alarmActive].lastDay = now.date.year * 512 + now.date.month * 32 + now.date.date;
  snoozedAlarm = alarmActive;
  snoozeStarted = millis();
  alarmActive = -1;
  astronautDragging = false;
  M5.Speaker.stop();
  astronautX = 6; astronautY = 105;
  saveSettings();
  drawClock(true); drawAstronaut();
}

void checkAlarms(const m5::rtc_datetime_t& dt) {
  int32_t day = dt.date.year * 512 + dt.date.month * 32 + dt.date.date;
  for (int i = 0; i < ALARM_COUNT; ++i) {
    if (!alarms[i].enabled || alarms[i].hour != dt.time.hours || alarms[i].minute != dt.time.minutes || alarms[i].lastDay == day) continue;
    if (alarms[i].weekdays && !(alarms[i].weekdays & (1 << dt.date.weekDay))) continue;
    startAlarm(i); return;
  }
}

void handleClockTouch(const m5::touch_detail_t& t) {
  if (alarmActive >= 0) {
    if (t.wasReleased() && t.y >= 215 && t.x >= 107 && t.x < 214) { snoozeAlarm(); return; }
    if (t.wasPressed() && t.x >= astronautX - 8 && t.x <= astronautX + 52 && t.y >= astronautY - 8 && t.y <= astronautY + 52) {
      astronautDragging = true;
      lastAlarmDragDraw = 0;
    }
    if (astronautDragging && t.isPressed()) {
      int nextX = constrain(t.x - 21, 2, 276), nextY = constrain(t.y - 21, 2, 195);
      // PNG decoding the full alarm scene on every touch sample starves the
      // touch controller. Limit visual repainting to a smooth 25 fps while
      // keeping the final hit-test at the user's latest finger position.
      bool moved = abs(nextX - astronautX) >= 3 || abs(nextY - astronautY) >= 3;
      astronautX = nextX; astronautY = nextY;
      if (moved && millis() - lastAlarmDragDraw >= 40UL) {
        lastAlarmDragDraw = millis();
        drawAstronaut();
      }
    }
    if (astronautDragging && t.wasReleased()) {
      astronautDragging = false;
      drawAstronaut();
      if (astronautX < 55 && ((satelliteTop && astronautY < 65) || (!satelliteTop && astronautY > 135))) dismissAlarm();
    }
    return;
  }
  if (!t.wasReleased() || t.y < 210) return;
  haptic();
  if (t.x < 107) showCompanion(); else if (t.x < 214) showMeditation(); else showMenu();
}

bool deviceIsFlat() {
  if (!M5.Imu.isEnabled()) return false;
  M5.Imu.update();
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  return fabsf(az) > 0.82f && fabsf(ax) < 0.42f && fabsf(ay) < 0.42f;
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (screenSleeping) {
    if (t.wasPressed() || t.isPressed()) {
      wakeDisplay();
      wakeTouchConsumed = true;
    }
    return;
  }
  if (wakeTouchConsumed) {
    if (t.wasReleased()) wakeTouchConsumed = false;
    return;
  }
  if (!flatVirtualButtonsEnabled && t.y >= 210 && (t.wasPressed() || t.isPressed() || t.wasReleased()) && deviceIsFlat()) return;
  if (t.wasPressed() || t.isPressed() || t.wasReleased()) lastUserActivity = millis();
  if (screenNow == Screen::Clock) { handleClockTouch(t); return; }
  if (screenNow == Screen::Companion && t.y >= 210) {
    if (t.wasPressed() && t.x >= 107 && t.x < 214) companionCenterPressedAt = millis();
    if (t.wasReleased()) {
      haptic(15);
      if (t.x < 107) {
        changeCompanionPage(-1);
      } else if (t.x >= 214) {
        changeCompanionPage(1);
      } else {
        bool longPress = companionCenterPressedAt && millis() - companionCenterPressedAt >= 700;
        companionCenterPressedAt = 0;
        if (longPress) {
          stopCompanion();
          screenNow = Screen::Clock;
          drawClock(true); drawAstronaut();
        } else if (companionPage != 0) {
          stopCompanion();
          companionPage = 0;
          clearCompanionPageData();
          drawCompanionButtons();
          connectCompanion();
        }
      }
    }
    return;
  }
  if (screenNow == Screen::Companion && t.y < 210 && (t.wasPressed() || t.wasReleased())) {
    int key = (t.y >= 105 ? 1 : 0) * COMPANION_COLS + min((int)COMPANION_COLS - 1, t.x / 106);
    if (companionConnected()) sendCompanionMessage("KEY-PRESS DEVICEID=" + companionDeviceId + " KEY=" + String(key) + " PRESSED=" + (t.wasPressed() ? "true\n" : "false\n"));
    if (t.wasPressed()) haptic(12);
    return;
  }
  if (screenNow == Screen::Meditation && t.wasReleased()) {
    haptic(15);
    if (t.y >= 210) {
      if (t.x < 107) showCompanion();
      else if (t.x < 214) { screenNow = Screen::Clock; drawClock(true); drawAstronaut(); }
      else { meditationSettingsPage = 0; showMeditationSettings(); }
    } else {
      int key = (t.y >= 104 ? 1 : 0) * 3 + min(2, t.x / 106);
      if (key == 0) toggleMeditation();
      else if (key == 3) resetMeditation();
      else if (key == 4) beginMeditation(meditationPresetMinutes[0]);
      else if (key == 5) beginMeditation(meditationPresetMinutes[1]);
    }
    return;
  }
  if (screenNow == Screen::MeditationSettings && t.wasReleased()) {
    haptic(15);
    if (t.y >= 210) {
      if (t.x < 107 && meditationSettingsPage) { --meditationSettingsPage; showMeditationSettings(); }
      else if (t.x < 214 && meditationSettingsPage < 2) { ++meditationSettingsPage; showMeditationSettings(); }
      else if (t.x >= 214) showMeditation();
      return;
    }
    if (t.y >= 47 && t.y < 203) {
      int row = constrain((t.y - 47) / 39, 0, 3);
      if (!meditationSettingsPage) {
        if (row == 0) meditationPresetMinutes[0] = meditationPresetMinutes[0] >= 60 ? 1 : meditationPresetMinutes[0] + 1;
        else if (row == 1) meditationPresetMinutes[1] = meditationPresetMinutes[1] >= 120 ? 1 : meditationPresetMinutes[1] + 5;
        else if (row == 2) meditationSoundEnabled = !meditationSoundEnabled;
        else meditationLightEnabled = !meditationLightEnabled;
      } else if (meditationSettingsPage == 1) {
        if (row == 0) { meditationStartSound = (meditationStartSound + 1) % 4; playMeditationSound(meditationStartSound, meditationStartVolume); }
        else if (row == 1) { meditationStartVolume = meditationStartVolume >= 100 ? 10 : meditationStartVolume + 10; playMeditationSound(meditationStartSound, meditationStartVolume); }
        else if (row == 2) { meditationEndSound = (meditationEndSound + 1) % 4; playMeditationSound(meditationEndSound, meditationEndVolume); }
        else { meditationEndVolume = meditationEndVolume >= 100 ? 10 : meditationEndVolume + 10; playMeditationSound(meditationEndSound, meditationEndVolume); }
      } else {
        if (row == 0) meditationNoiseEnabled = !meditationNoiseEnabled;
        else if (row == 1) meditationNoise = (meditationNoise + 1) % 3;
        else if (row == 2) meditationNoiseVolume = meditationNoiseVolume >= 80 ? 5 : meditationNoiseVolume + 5;
        else { M5.Speaker.stop(1); bool wasOn=meditationNoiseEnabled; MeditationState wasState=meditationState; meditationNoiseEnabled=true; meditationState=MeditationState::Running; playMeditationAmbient(1); meditationNoiseEnabled=wasOn; meditationState=wasState; }
      }
      saveSettings(); showMeditationSettings();
    }
    return;
  }
  if (!t.wasReleased()) return;
  haptic();
  if (t.y >= 210 && t.x >= 214) { screenNow = Screen::Clock; drawClock(true); drawAstronaut(); return; }
  if (screenNow == Screen::Menu) {
    if (t.y >= 38 && t.y < 67) runWifiPortal();
    else if (t.y < 96) showFaces();
    else if (t.y < 125) { clockSettingsPage = 0; showSettings(); }
    else if (t.y < 154) showAlarms();
    else if (t.y < 183) { meditationSettingsPage = 0; showMeditationSettings(); }
    else if (t.y < 212) showFirmwareUpdate();
  } else if (screenNow == Screen::Faces) {
    if (t.y >= 52 && t.y < 196) {
      int selected = (t.y - 52) / 48;
      if (selected >= 0 && selected <= 2) {
        clockFace = static_cast<ClockFace>(selected);
        saveSettings(); showFaces();
      }
    }
  } else if (screenNow == Screen::Alarms) {
    if (t.y >= 43 && t.y < 211) {
      int row = constrain((t.y - 43) / 42, 0, (int)ALARMS_PER_PAGE - 1);
      int i = alarmPage * ALARMS_PER_PAGE + row;
      if (t.x > 235) alarms[i].enabled = !alarms[i].enabled;
      else if (t.x < 90) alarms[i].hour = (alarms[i].hour + 1) % 24;
      else if (t.x < 155) alarms[i].minute = (alarms[i].minute + 5) % 60;
      else {
        // once -> weekdays -> every day -> once
        alarms[i].weekdays = alarms[i].weekdays == 0 ? 0x3E : (alarms[i].weekdays == 0x3E ? 0x7F : 0);
      }
      // Editing or re-enabling an alarm creates a new schedule. It must not
      // inherit today's "already handled" marker from an older time.
      alarms[i].lastDay = -1;
      saveSettings(); showAlarms();
    } else if (t.y >= 210 && t.x < 107 && alarmPage > 0) {
      --alarmPage; showAlarms();
    } else if (t.y >= 210 && t.x >= 107 && t.x < 214 && alarmPage + 1 < ALARM_PAGE_COUNT) {
      ++alarmPage; showAlarms();
    }
  } else if (screenNow == Screen::Settings) {
    if (t.y >= 210) {
      if (t.x < 107) {
        if (clockSettingsPage) clockSettingsPage = 0; else syncTime();
      } else if (t.x < 214) {
        if (!clockSettingsPage) clockSettingsPage = 1; else saveSettings();
      }
    } else if (!clockSettingsPage) {
      if (t.y >= 36 && t.y < 62) { timeZoneIndex = (timeZoneIndex + 1) % TIME_ZONE_COUNT; syncTime(); }
      else if (t.y < 90) adaptiveBrightness = !adaptiveBrightness;
      else if (t.y < 118) dayBrightness = dayBrightness >= 100 ? 20 : dayBrightness + 10;
      else if (t.y < 146) nightBrightness = nightBrightness >= 100 ? 5 : nightBrightness + 5;
      else if (t.y < 174) alarmVolume = alarmVolume >= 100 ? 10 : alarmVolume + 10;
      else if (t.y < 210) cycleScreenOffTime();
    } else {
      if (t.y >= 36 && t.y < 62) use24HourTime = !use24HourTime;
      else if (t.y < 90) flatVirtualButtonsEnabled = !flatVirtualButtonsEnabled;
      else if (t.y < 118) nightLightEnabled = !nightLightEnabled;
      else if (t.y < 146) {
        const uint32_t colors[] = {0xFFF0C8, 0xFFFFFF, 0xFFD080, 0x80B8FF, 0xFF9090};
        int next = 0; for (int i=0;i<5;++i) if (nightLightColor==colors[i]) next=(i+1)%5;
        nightLightColor=colors[next];
      } else if (t.y < 174) nightLightBrightness = nightLightBrightness >= 100 ? 5 : nightLightBrightness + 5;
      else if (t.y < 210) nightLightMode = (nightLightMode + 1) % 3;
    }
    saveSettings();
    m5::rtc_datetime_t brightnessNow; getClockDateTime(&brightnessNow); applyDisplayBrightness(brightnessNow);
    showSettings();
  } else if (screenNow == Screen::FirmwareUpdate) {
    if (t.y >= 96 && t.y < 127) {
      automaticFirmwareUpdate = !automaticFirmwareUpdate;
      saveSettings(); showFirmwareUpdate();
    } else if (t.y >= 127 && t.y < 158) {
      firmwareCheckHour = (firmwareCheckHour + 1) % 24;
      saveSettings(); showFirmwareUpdate();
    } else if (t.y >= 210 && t.x < 107) {
      readFirmwareManifest(true);
    } else if (t.y >= 210 && t.x < 214 && firmwareUpdateAvailable) {
      installLatestFirmware(true);
    }
  }
}

void handleSerialConfig() {
  while (Serial.available()) Serial.read();
}

String htmlEscape(String value) {
  value.replace("&", "&amp;"); value.replace("\"", "&quot;");
  value.replace("<", "&lt;"); value.replace(">", "&gt;");
  return value;
}

String colorHex(uint32_t color) {
  char value[8]; snprintf(value, sizeof(value), "#%06lX", (unsigned long)(color & 0xFFFFFF));
  return value;
}

uint32_t parseWebColor(const String& value, uint32_t fallback) {
  if (value.length() != 7 || value[0] != '#') return fallback;
  return strtoul(value.substring(1).c_str(), nullptr, 16) & 0xFFFFFF;
}

uint32_t parseJsonColor(JsonVariantConst value, uint32_t fallback) {
  if (value.is<const char*>()) return parseWebColor(String(value.as<const char*>()), fallback);
  if (value.is<uint32_t>()) return value.as<uint32_t>() & 0xFFFFFF;
  return fallback;
}

const char* meditationStateName() {
  switch (meditationState) {
    case MeditationState::Running: return "running";
    case MeditationState::Paused: return "paused";
    case MeditationState::Done: return "done";
    default: return "ready";
  }
}

String homeAssistantDeviceId() {
  return "spaceclock_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

void addHomeAssistantDevice(JsonDocument& doc) {
  JsonObject device = doc["device"].to<JsonObject>();
  JsonArray identifiers = device["identifiers"].to<JsonArray>();
  identifiers.add(homeAssistantDeviceId());
  device["name"] = "Space Clock Core2";
  device["manufacturer"] = "M5Stack";
  device["model"] = "Core2";
  device["sw_version"] = SPACE_CLOCK_VERSION;
}

void publishHomeAssistantConfig(const char* component, const char* objectId, JsonDocument& doc) {
  if (!mqttClient.connected()) return;
  addHomeAssistantDevice(doc);
  String topic = "homeassistant/" + String(component) + "/" + homeAssistantDeviceId() + "/" + objectId + "/config";
  String payload; serializeJson(doc, payload);
  mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

void publishHomeAssistantDiscovery() {
  if (!mqttClient.connected()) return;
  const String stateTopic = mqttBaseTopic + "/state";
  const String settingsTopic = mqttBaseTopic + "/settings";
  const String setTopic = mqttBaseTopic + "/set";
  const String commandTopic = mqttBaseTopic + "/command/";
  JsonDocument doc;

  doc["name"] = "Battery"; doc["unique_id"] = homeAssistantDeviceId() + "_battery";
  doc["state_topic"] = stateTopic; doc["value_template"] = "{{ value_json.device.battery_percent }}";
  doc["unit_of_measurement"] = "%"; doc["device_class"] = "battery"; doc["state_class"] = "measurement";
  publishHomeAssistantConfig("sensor", "battery", doc); doc.clear();

  doc["name"] = "Ambient light"; doc["unique_id"] = homeAssistantDeviceId() + "_ambient_light";
  doc["state_topic"] = stateTopic; doc["value_template"] = "{{ value_json.device.ambient_light_lux }}";
  doc["availability_topic"] = stateTopic; doc["availability_template"] = "{{ value_json.device.ambient_light_available }}";
  doc["payload_available"] = "true"; doc["payload_not_available"] = "false";
  doc["unit_of_measurement"] = "lx"; doc["device_class"] = "illuminance"; doc["state_class"] = "measurement";
  publishHomeAssistantConfig("sensor", "ambient_light", doc); doc.clear();

  doc["name"] = "Meditation remaining"; doc["unique_id"] = homeAssistantDeviceId() + "_meditation_remaining";
  doc["state_topic"] = stateTopic; doc["value_template"] = "{{ value_json.meditation.remaining_seconds }}";
  doc["unit_of_measurement"] = "s"; doc["device_class"] = "duration"; doc["state_class"] = "measurement";
  publishHomeAssistantConfig("sensor", "meditation_remaining", doc); doc.clear();

  doc["name"] = "Charging"; doc["unique_id"] = homeAssistantDeviceId() + "_charging";
  doc["state_topic"] = stateTopic; doc["value_template"] = "{{ value_json.device.charging }}";
  doc["payload_on"] = "true"; doc["payload_off"] = "false"; doc["device_class"] = "battery_charging";
  publishHomeAssistantConfig("binary_sensor", "charging", doc); doc.clear();

  doc["name"] = "Screen"; doc["unique_id"] = homeAssistantDeviceId() + "_screen";
  doc["state_topic"] = stateTopic; doc["value_template"] = "{{ value_json.device.screen_on }}";
  doc["command_topic"] = commandTopic + "screen"; doc["payload_on"] = "wake"; doc["payload_off"] = "off";
  publishHomeAssistantConfig("switch", "screen", doc); doc.clear();

  doc["name"] = "Adaptive brightness"; doc["unique_id"] = homeAssistantDeviceId() + "_adaptive_brightness";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ value_json.adaptive_brightness }}";
  doc["command_topic"] = setTopic; doc["payload_on"] = "{\"adaptive_brightness\":true}"; doc["payload_off"] = "{\"adaptive_brightness\":false}";
  publishHomeAssistantConfig("switch", "adaptive_brightness", doc); doc.clear();

  doc["name"] = "Night light"; doc["unique_id"] = homeAssistantDeviceId() + "_night_light";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ value_json.night_light.enabled }}";
  doc["command_topic"] = setTopic; doc["payload_on"] = "{\"night_light\":{\"enabled\":true}}"; doc["payload_off"] = "{\"night_light\":{\"enabled\":false}}";
  publishHomeAssistantConfig("switch", "night_light", doc); doc.clear();

  doc["name"] = "Alarm light"; doc["unique_id"] = homeAssistantDeviceId() + "_alarm_light";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ value_json.alarm_light.enabled }}";
  doc["command_topic"] = setTopic; doc["payload_on"] = "{\"alarm_light\":{\"enabled\":true}}"; doc["payload_off"] = "{\"alarm_light\":{\"enabled\":false}}";
  publishHomeAssistantConfig("switch", "alarm_light", doc); doc.clear();

  doc["name"] = "Day brightness"; doc["unique_id"] = homeAssistantDeviceId() + "_day_brightness";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ value_json.day_brightness }}"; doc["command_topic"] = setTopic;
  doc["command_template"] = "{\"day_brightness\":{{ value | int }}}"; doc["min"] = 10; doc["max"] = 100; doc["step"] = 5; doc["unit_of_measurement"] = "%";
  publishHomeAssistantConfig("number", "day_brightness", doc); doc.clear();

  doc["name"] = "Alarm volume"; doc["unique_id"] = homeAssistantDeviceId() + "_alarm_volume";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ value_json.alarm_volume }}"; doc["command_topic"] = setTopic;
  doc["command_template"] = "{\"alarm_volume\":{{ value | int }}}"; doc["min"] = 10; doc["max"] = 100; doc["step"] = 5; doc["unit_of_measurement"] = "%";
  publishHomeAssistantConfig("number", "alarm_volume", doc); doc.clear();

  doc["name"] = "Screen timeout"; doc["unique_id"] = homeAssistantDeviceId() + "_screen_timeout";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ value_json.screen_off_seconds }}"; doc["command_topic"] = setTopic;
  doc["command_template"] = "{\"screen_off_seconds\":{{ value | int }}}"; doc["min"] = 0; doc["max"] = 1800; doc["step"] = 5; doc["unit_of_measurement"] = "s";
  publishHomeAssistantConfig("number", "screen_timeout", doc); doc.clear();

  doc["name"] = "Clock face"; doc["unique_id"] = homeAssistantDeviceId() + "_clock_face";
  doc["state_topic"] = settingsTopic; doc["value_template"] = "{{ ['Space', 'Flip clock', 'Matrix rain'][value_json.clock_face] }}"; doc["command_topic"] = setTopic;
  JsonArray faceOptions = doc["options"].to<JsonArray>(); faceOptions.add("Space"); faceOptions.add("Flip clock"); faceOptions.add("Matrix rain");
  doc["command_template"] = "{\"clock_face\":{{ {'Space':0,'Flip clock':1,'Matrix rain':2}[value] }}}";
  publishHomeAssistantConfig("select", "clock_face", doc); doc.clear();

  doc["name"] = "Start meditation preset 1"; doc["unique_id"] = homeAssistantDeviceId() + "_meditation_start_1";
  doc["command_topic"] = commandTopic + "meditation"; doc["payload_press"] = "start1";
  publishHomeAssistantConfig("button", "meditation_start_1", doc); doc.clear();
  doc["name"] = "Dismiss alarm"; doc["unique_id"] = homeAssistantDeviceId() + "_dismiss_alarm";
  doc["command_topic"] = commandTopic + "alarm"; doc["payload_press"] = "dismiss";
  publishHomeAssistantConfig("button", "dismiss_alarm", doc);
  mqttDiscoveryDirty = false;
}

void publishMqttState() {
  if (!mqttClient.connected()) return;
  m5::rtc_datetime_t now; getClockDateTime(&now);
  char localTime[24];
  snprintf(localTime, sizeof(localTime), "%04u-%02u-%02uT%02u:%02u:%02u",
           now.date.year, now.date.month, now.date.date,
           now.time.hours, now.time.minutes, now.time.seconds);
  char dateText[11], clockTime[16];
  snprintf(dateText, sizeof(dateText), "%04u-%02u-%02u", now.date.year, now.date.month, now.date.date);
  if (use24HourTime) {
    snprintf(clockTime, sizeof(clockTime), "%02u:%02u:%02u", now.time.hours, now.time.minutes, now.time.seconds);
  } else {
    uint8_t hour12 = now.time.hours % 12; if (!hour12) hour12 = 12;
    snprintf(clockTime, sizeof(clockTime), "%02u:%02u:%02u %s", hour12, now.time.minutes, now.time.seconds, now.time.hours >= 12 ? "PM" : "AM");
  }
  const char* weekdayText[] = {"星期日 (SUN)", "星期一 (MON)", "星期二 (TUE)", "星期三 (WED)", "星期四 (THU)", "星期五 (FRI)", "星期六 (SAT)"};
  uint32_t elapsed = meditationElapsedSeconds();
  uint32_t remaining = meditationDurationSeconds > elapsed ? meditationDurationSeconds - elapsed : 0;
  JsonDocument doc;
  doc["time"] = localTime;
  doc["date"] = dateText;
  doc["weekday"] = weekdayText[now.date.weekDay];
  doc["clock_time"] = clockTime;
  doc["timezone"] = TIME_ZONES[timeZoneIndex].city;
  doc["time_format"] = use24HourTime ? 24 : 12;
  JsonObject meditation = doc["meditation"].to<JsonObject>();
  meditation["state"] = meditationStateName();
  meditation["duration_seconds"] = meditationDurationSeconds;
  meditation["duration_text"] = durationText(meditationDurationSeconds);
  meditation["elapsed_seconds"] = elapsed;
  meditation["elapsed_text"] = durationText(elapsed);
  meditation["remaining_seconds"] = remaining;
  meditation["remaining_text"] = durationText(remaining);
  JsonObject device = doc["device"].to<JsonObject>();
  device["ip"] = WiFi.localIP().toString();
  device["battery_percent"] = M5.Power.getBatteryLevel();
  device["charging"] = M5.Power.isCharging();
  device["screen_on"] = !screenSleeping;
  // Core2 has no built-in ambient-light sensor. The Discovery entity is
  // intentionally marked unavailable until an external light unit is added.
  device["ambient_light_available"] = false;
  device["ambient_light_lux"] = 0;
  String payload; serializeJson(doc, payload);
  mqttClient.publish((mqttBaseTopic + "/state").c_str(), payload.c_str(), true);
  // Keep the original topic alive for existing integrations.
  mqttClient.publish((mqttBaseTopic + "/status").c_str(), payload.c_str(), true);
}

void publishMqttSettings() {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["timezone_index"] = timeZoneIndex;
  doc["timezone_city"] = TIME_ZONES[timeZoneIndex].city;
  doc["clock_face"] = static_cast<uint8_t>(clockFace);
  JsonObject matrix = doc["matrix"].to<JsonObject>();
  matrix["speed"] = matrixRainSpeed; matrix["density"] = matrixRainDensity;
  matrix["glyph_scale"] = matrixGlyphScale; matrix["color"] = colorHex(matrixRainColor);
  matrix["glass_opacity"] = matrixGlassOpacity;
  doc["time_format"] = use24HourTime ? 24 : 12;
  doc["flat_virtual_buttons"] = flatVirtualButtonsEnabled;
  doc["adaptive_brightness"] = adaptiveBrightness;
  doc["day_brightness"] = dayBrightness;
  doc["night_brightness"] = nightBrightness;
  doc["screen_off_seconds"] = screenOffSeconds;
  doc["alarm_volume"] = alarmVolume;
  doc["alarm_sound"] = alarmSound;
  JsonObject night = doc["night_light"].to<JsonObject>();
  night["enabled"] = nightLightEnabled; night["color"] = colorHex(nightLightColor);
  night["brightness"] = nightLightBrightness; night["mode"] = nightLightMode; night["seconds"] = nightLightSeconds;
  JsonObject alarmLight = doc["alarm_light"].to<JsonObject>();
  alarmLight["enabled"] = alarmLightEnabled; alarmLight["color"] = colorHex(alarmLightColor);
  alarmLight["brightness"] = alarmLightBrightness; alarmLight["mode"] = alarmLightMode;
  JsonObject meditation = doc["meditation"].to<JsonObject>();
  JsonArray presets = meditation["preset_minutes"].to<JsonArray>(); presets.add(meditationPresetMinutes[0]); presets.add(meditationPresetMinutes[1]);
  meditation["sound_enabled"] = meditationSoundEnabled;
  meditation["start_sound"] = meditationStartSound; meditation["start_volume"] = meditationStartVolume;
  meditation["end_sound"] = meditationEndSound; meditation["end_volume"] = meditationEndVolume;
  meditation["light_enabled"] = meditationLightEnabled;
  meditation["noise_enabled"] = meditationNoiseEnabled; meditation["noise"] = meditationNoise; meditation["noise_volume"] = meditationNoiseVolume;
  JsonArray alarmList = doc["alarms"].to<JsonArray>();
  for (int i = 0; i < ALARM_COUNT; ++i) {
    JsonObject a = alarmList.add<JsonObject>();
    a["index"] = i; a["hour"] = alarms[i].hour; a["minute"] = alarms[i].minute;
    a["enabled"] = alarms[i].enabled; a["weekdays"] = alarms[i].weekdays;
  }
  JsonArray companion = doc["companion"].to<JsonArray>();
  for (int i = 0; i < COMPANION_PAGE_COUNT; ++i) {
    JsonObject page = companion.add<JsonObject>(); page["page"] = i + 1; page["name"] = companionNames[i]; page["host"] = companionHosts[i]; page["port"] = companionPorts[i]; page["internet_url"] = companionInternetUrls[i];
  }
  JsonObject mqtt = doc["mqtt"].to<JsonObject>();
  mqtt["enabled"] = mqttEnabled; mqtt["host"] = mqttHost; mqtt["port"] = mqttPort;
  mqtt["username"] = mqttUsername; mqtt["base_topic"] = mqttBaseTopic;
  JsonObject wifi = doc["wifi"].to<JsonObject>(); wifi["ssid"] = WiFi.SSID();
  String payload; serializeJson(doc, payload);
  mqttClient.publish((mqttBaseTopic + "/settings").c_str(), payload.c_str(), true);
  mqttSettingsDirty = false;
}

bool applyMqttSettings(const String& payload, String& error) {
  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, payload);
  if (jsonError || !doc.is<JsonObject>()) { error = jsonError ? jsonError.c_str() : "object required"; return false; }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root["timezone_index"].is<int>()) timeZoneIndex = constrain(root["timezone_index"].as<int>(), 0, (int)TIME_ZONE_COUNT - 1);
  if (root["clock_face"].is<int>()) clockFace = static_cast<ClockFace>(constrain(root["clock_face"].as<int>(), 0, 2));
  JsonObjectConst matrix = root["matrix"];
  if (!matrix.isNull()) {
    if (matrix["speed"].is<int>()) matrixRainSpeed = constrain(matrix["speed"].as<int>(), 10, 100);
    if (matrix["density"].is<int>()) matrixRainDensity = constrain(matrix["density"].as<int>(), 10, 100);
    if (matrix["glyph_scale"].is<int>()) matrixGlyphScale = constrain(matrix["glyph_scale"].as<int>(), 1, 2);
    if (!matrix["color"].isNull()) matrixRainColor = parseJsonColor(matrix["color"], matrixRainColor);
    if (matrix["glass_opacity"].is<int>()) matrixGlassOpacity = constrain(matrix["glass_opacity"].as<int>(), 15, 90);
    resetMatrixRain();
  }
  if (root["time_format"].is<int>()) use24HourTime = root["time_format"].as<int>() != 12;
  if (root["flat_virtual_buttons"].is<bool>()) flatVirtualButtonsEnabled = root["flat_virtual_buttons"].as<bool>();
  if (root["adaptive_brightness"].is<bool>()) adaptiveBrightness = root["adaptive_brightness"].as<bool>();
  if (root["day_brightness"].is<int>()) dayBrightness = constrain(root["day_brightness"].as<int>(), 10, 100);
  if (root["night_brightness"].is<int>()) nightBrightness = constrain(root["night_brightness"].as<int>(), 5, 100);
  if (root["screen_off_seconds"].is<int>()) screenOffSeconds = constrain(root["screen_off_seconds"].as<int>(), 0, 1800);
  if (root["alarm_volume"].is<int>()) alarmVolume = constrain(root["alarm_volume"].as<int>(), 10, 100);
  if (root["alarm_sound"].is<int>()) alarmSound = constrain(root["alarm_sound"].as<int>(), 0, 3);
  JsonObjectConst night = root["night_light"];
  if (!night.isNull()) {
    if (night["enabled"].is<bool>()) nightLightEnabled = night["enabled"];
    if (!night["color"].isNull()) nightLightColor = parseJsonColor(night["color"], nightLightColor);
    if (night["brightness"].is<int>()) nightLightBrightness = constrain(night["brightness"].as<int>(), 1, 100);
    if (night["mode"].is<int>()) nightLightMode = constrain(night["mode"].as<int>(), 0, 2);
    if (night["seconds"].is<int>()) nightLightSeconds = constrain(night["seconds"].as<int>(), 5, 3600);
  }
  JsonObjectConst alarmLight = root["alarm_light"];
  if (!alarmLight.isNull()) {
    if (alarmLight["enabled"].is<bool>()) alarmLightEnabled = alarmLight["enabled"];
    if (!alarmLight["color"].isNull()) alarmLightColor = parseJsonColor(alarmLight["color"], alarmLightColor);
    if (alarmLight["brightness"].is<int>()) alarmLightBrightness = constrain(alarmLight["brightness"].as<int>(), 1, 100);
    if (alarmLight["mode"].is<int>()) alarmLightMode = constrain(alarmLight["mode"].as<int>(), 0, 3);
  }
  JsonObjectConst meditation = root["meditation"];
  if (!meditation.isNull()) {
    JsonArrayConst presets = meditation["preset_minutes"];
    if (presets.size() > 0) meditationPresetMinutes[0] = constrain(presets[0].as<int>(), 1, 180);
    if (presets.size() > 1) meditationPresetMinutes[1] = constrain(presets[1].as<int>(), 1, 180);
    if (meditation["sound_enabled"].is<bool>()) meditationSoundEnabled = meditation["sound_enabled"];
    if (meditation["start_sound"].is<int>()) meditationStartSound = constrain(meditation["start_sound"].as<int>(), 0, 3);
    if (meditation["start_volume"].is<int>()) meditationStartVolume = constrain(meditation["start_volume"].as<int>(), 5, 100);
    if (meditation["end_sound"].is<int>()) meditationEndSound = constrain(meditation["end_sound"].as<int>(), 0, 3);
    if (meditation["end_volume"].is<int>()) meditationEndVolume = constrain(meditation["end_volume"].as<int>(), 5, 100);
    if (meditation["light_enabled"].is<bool>()) meditationLightEnabled = meditation["light_enabled"];
    if (meditation["noise_enabled"].is<bool>()) meditationNoiseEnabled = meditation["noise_enabled"];
    if (meditation["noise"].is<int>()) meditationNoise = constrain(meditation["noise"].as<int>(), 0, 2);
    if (meditation["noise_volume"].is<int>()) meditationNoiseVolume = constrain(meditation["noise_volume"].as<int>(), 5, 80);
  }
  JsonArrayConst alarmList = root["alarms"];
  for (JsonObjectConst item : alarmList) {
    int i = item["index"] | -1; if (i < 0 || i >= ALARM_COUNT) continue;
    if (item["hour"].is<int>()) alarms[i].hour = constrain(item["hour"].as<int>(), 0, 23);
    if (item["minute"].is<int>()) alarms[i].minute = constrain(item["minute"].as<int>(), 0, 59);
    if (item["enabled"].is<bool>()) alarms[i].enabled = item["enabled"];
    if (item["weekdays"].is<int>()) alarms[i].weekdays = constrain(item["weekdays"].as<int>(), 0, 127);
    alarms[i].lastDay = -1;
  }
  bool reconnectCompanion = false;
  JsonArrayConst companion = root["companion"];
  for (JsonObjectConst item : companion) {
    int i = (item["page"] | 0) - 1; if (i < 0 || i >= COMPANION_PAGE_COUNT) continue;
    if (item["name"].is<const char*>()) companionNames[i] = item["name"].as<const char*>();
    if (item["host"].is<const char*>()) { String host = item["host"].as<const char*>(); reconnectCompanion |= host != companionHosts[i]; companionHosts[i] = host; }
    if (item["port"].is<int>()) { uint16_t port = constrain(item["port"].as<int>(), 1, 65535); reconnectCompanion |= port != companionPorts[i]; companionPorts[i] = port; }
    if (item["internet_url"].is<const char*>()) { String url = item["internet_url"].as<const char*>(); reconnectCompanion |= url != companionInternetUrls[i]; companionInternetUrls[i] = url; }
  }
  JsonObjectConst mqtt = root["mqtt"];
  bool reconnectMqtt = false;
  if (!mqtt.isNull()) {
    if (mqtt["enabled"].is<bool>()) mqttEnabled = mqtt["enabled"];
    if (mqtt["host"].is<const char*>()) { mqttHost = mqtt["host"].as<const char*>(); reconnectMqtt = true; }
    if (mqtt["port"].is<int>()) { mqttPort = constrain(mqtt["port"].as<int>(), 1, 65535); reconnectMqtt = true; }
    if (mqtt["username"].is<const char*>()) { mqttUsername = mqtt["username"].as<const char*>(); reconnectMqtt = true; }
    if (mqtt["password"].is<const char*>()) { mqttPassword = mqtt["password"].as<const char*>(); reconnectMqtt = true; }
    if (mqtt["base_topic"].is<const char*>()) { mqttBaseTopic = mqtt["base_topic"].as<const char*>(); mqttBaseTopic.trim(); while (mqttBaseTopic.endsWith("/")) mqttBaseTopic.remove(mqttBaseTopic.length()-1); reconnectMqtt = true; }
  }
  JsonObjectConst wifi = root["wifi"];
  String nextSsid, nextPassword;
  if (!wifi.isNull() && wifi["ssid"].is<const char*>()) nextSsid = wifi["ssid"].as<const char*>();
  if (!wifi.isNull() && wifi["password"].is<const char*>()) nextPassword = wifi["password"].as<const char*>();
  if (reconnectCompanion) { stopCompanion(); clearCompanionPageData(); }
  saveSettings(); mqttSettingsDirty = true;
  syncTime();
  m5::rtc_datetime_t brightnessNow; getClockDateTime(&brightnessNow); applyDisplayBrightness(brightnessNow);
  if (!screenSleeping) { drawClock(true); drawAstronaut(); }
  if (nextSsid.length() && (nextSsid != WiFi.SSID() || nextPassword.length())) { WiFi.disconnect(); WiFi.begin(nextSsid.c_str(), nextPassword.c_str()); }
  if (reconnectMqtt) mqttReconnectRequested = true;
  return true;
}

void mqttMessage(char* topicChars, byte* payload, unsigned int length) {
  String topic(topicChars), value;
  for (unsigned int i=0;i<length;++i) value += (char)payload[i];
  value.trim();
  if (topic == mqttBaseTopic + "/set") {
    String replyBase = mqttBaseTopic;
    String error;
    bool ok = applyMqttSettings(value, error);
    if (mqttClient.connected()) {
      String ack = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"" + error + "\"}";
      mqttClient.publish((replyBase + "/ack").c_str(), ack.c_str(), false);
      if (ok && !mqttReconnectRequested) { publishMqttSettings(); publishMqttState(); }
    }
    if (mqttReconnectRequested) { mqttReconnectRequested = false; mqttClient.disconnect(); }
    return;
  }
  value.toLowerCase();
  String commandRoot = mqttBaseTopic + "/command/";
  if (!topic.startsWith(commandRoot)) return;
  String command = topic.substring(commandRoot.length());
  if (command == "screen") {
    if (value == "on" || value == "wake") wakeDisplay();
    else if (value == "off") { screenSleeping=true; screenSleepStarted=millis(); M5.Display.setBrightness(0); }
  } else if (command == "brightness") {
    adaptiveBrightness=false; dayBrightness=constrain(value.toInt(),10,100); M5.Display.setBrightness(dayBrightness*255/100); saveSettings();
  } else if (command == "page") {
    if (value == "clock") { screenNow=Screen::Clock; drawClock(true); drawAstronaut(); }
    else if (value == "meditation") showMeditation();
    else if (value.startsWith("companion")) { companionPage=constrain(value.substring(9).toInt()-1,0,3); showCompanion(); }
  } else if (command == "meditation") {
    if (value == "start1") beginMeditation(meditationPresetMinutes[0]);
    else if (value == "start2") beginMeditation(meditationPresetMinutes[1]);
    else if (value == "pause" || value == "resume") toggleMeditation();
    else if (value == "reset" || value == "stop") resetMeditation();
  } else if (command == "alarm" && alarmActive >= 0) {
    if (value == "stop" || value == "dismiss") dismissAlarm();
    else if (value == "snooze") snoozeAlarm();
  } else if (command == "settings" && value == "get") publishMqttSettings();
}

void maintainMqtt(uint32_t nowMs) {
  if (!mqttEnabled || !mqttHost.length() || WiFi.status()!=WL_CONNECTED) { if(mqttClient.connected()) mqttClient.disconnect(); return; }
  if (!mqttClient.connected()) {
    if (nowMs-lastMqttReconnect < 5000UL) return;
    lastMqttReconnect=nowMs;
    mqttClient.setServer(mqttHost.c_str(), mqttPort);
    mqttClient.setCallback(mqttMessage);
    mqttClient.setBufferSize(8192);
    String clientId="SpaceClock-"+String((uint32_t)ESP.getEfuseMac(),HEX);
    bool ok = mqttUsername.length() ? mqttClient.connect(clientId.c_str(),mqttUsername.c_str(),mqttPassword.c_str()) : mqttClient.connect(clientId.c_str());
    if(ok) {
      mqttClient.subscribe((mqttBaseTopic+"/command/#").c_str());
      mqttClient.subscribe((mqttBaseTopic+"/set").c_str());
      mqttSettingsDirty = true;
      mqttDiscoveryDirty = true;
    }
  }
  if (!mqttClient.connected()) return;
  mqttClient.loop();
  if (mqttDiscoveryDirty) publishHomeAssistantDiscovery();
  if (mqttSettingsDirty) publishMqttSettings();
  if(nowMs-lastMqttPublish >= 1000UL) {
    lastMqttPublish=nowMs;
    publishMqttState();
  }
}

void sendSettingsPage(const String& message = "") {
  String page;
  page.reserve(28000);
  page = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Space Clock</title><style>body{font-family:system-ui;background:#08111f;color:#eef4ff;max-width:620px;margin:auto;padding:20px}"
         "h1{color:#65b9ff}h2{margin-top:30px}.field{display:block;margin-top:14px}.field input,.field select{box-sizing:border-box;width:100%;padding:11px;border-radius:8px;border:1px solid #52657a;background:#142236;color:white}"
         ".alarm{background:#101d2e;border:1px solid #344b63;border-radius:10px;padding:12px;margin:10px 0}.alarm summary{cursor:pointer;font-weight:700}.days{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px}.days label{white-space:nowrap}"
         "button,.button{box-sizing:border-box;display:block;width:100%;padding:13px;margin-top:22px;border:0;border-radius:9px;background:#1688e5;color:white;font-size:17px;text-align:center;text-decoration:none}.ok{color:#70e39a}</style></head><body>";
  page += "<h1>Space Clock settings</h1><p>Device IP: <b>" + WiFi.localIP().toString() + "</b></p>";
  m5::rtc_datetime_t webNow; getClockDateTime(&webNow);
  char webTime[24]; snprintf(webTime, sizeof(webTime), "%04d-%02d-%02d %02d:%02d:%02d", webNow.date.year, webNow.date.month, webNow.date.date, webNow.time.hours, webNow.time.minutes, webNow.time.seconds);
  page += "<p>Device time: <b>" + String(webTime) + "</b> (" + TIME_ZONES[timeZoneIndex].city + ")</p>";
  if (message.length()) page += "<p class='ok'>" + htmlEscape(message) + "</p>";
  page += "<form method='post' action='/save'>";
  page += "<h2>Saved Wi-Fi networks</h2><p>Up to 10 networks. Passwords are stored only on this Core2 and are never shown. Leave a password blank to keep it unchanged.</p>";
  for (int i = 0; i < SAVED_WIFI_COUNT; ++i) {
    page += "<div class='alarm'><b>Wi-Fi " + String(i + 1) + "</b>";
    page += "<label class='field'>SSID<input name='wifiS" + String(i) + "' value='" + htmlEscape(savedWifiSsids[i]) + "'></label>";
    page += "<label class='field'>New password<input type='password' name='wifiP" + String(i) + "' placeholder='Leave blank to keep current'></label>";
    page += "<label><input type='checkbox' name='wifiD" + String(i) + "'> Remove this network</label></div>";
  }
  page += "<label class='field'>Time zone (major city)<select name='timeZone'>";
  for (int i = 0; i < TIME_ZONE_COUNT; ++i) page += "<option value='" + String(i) + "'" + (i == timeZoneIndex ? " selected" : "") + ">" + TIME_ZONES[i].city + "</option>";
  page += "</select></label>";
  page += "<label class='field'>Clock face<select name='face'>";
  const char* faceNames[] = {"Space", "Flip clock", "Matrix rain"};
  for (int i = 0; i < 3; ++i) page += "<option value='" + String(i) + "'" + (i == (int)clockFace ? " selected" : "") + ">" + faceNames[i] + "</option>";
  page += "</select></label>";
  page += "<h2>Matrix rain appearance</h2><p>Applies when Matrix rain is selected. Lower speed and density create a calmer background. Glass opacity controls how much code is visible behind the clock.</p>";
  page += "<label class='field'>Rain speed: <output id='matrixSpeedOut'>" + String(matrixRainSpeed) + "</output><input type='range' min='10' max='100' step='5' name='matrixSpeed' value='" + String(matrixRainSpeed) + "' oninput='matrixSpeedOut.value=this.value'></label>";
  page += "<label class='field'>Rain density: <output id='matrixDensityOut'>" + String(matrixRainDensity) + "%</output><input type='range' min='10' max='100' step='5' name='matrixDensity' value='" + String(matrixRainDensity) + "' oninput='matrixDensityOut.value=this.value+\"%\"'></label>";
  page += "<label class='field'>Glyph size<select name='matrixSize'><option value='1'" + String(matrixGlyphScale == 1 ? " selected" : "") + ">Small</option><option value='2'" + String(matrixGlyphScale == 2 ? " selected" : "") + ">Large</option></select></label>";
  page += "<label class='field'>Rain color<input type='color' name='matrixColor' value='" + colorHex(matrixRainColor) + "'></label>";
  page += "<label class='field'>Clock glass opacity: <output id='matrixGlassOut'>" + String(matrixGlassOpacity) + "%</output><input type='range' min='15' max='90' step='5' name='matrixGlass' value='" + String(matrixGlassOpacity) + "' oninput='matrixGlassOut.value=this.value+\"%\"'></label>";
  page += "<label class='field'>Time format<select name='time24'><option value='1'"+String(use24HourTime?" selected":"")+">24-hour</option><option value='0'"+String(!use24HourTime?" selected":"")+">12-hour</option></select></label>";
  page += "<label class='field'><input type='checkbox' name='flatButtons'"+String(flatVirtualButtonsEnabled?" checked":"")+"> Enable the three virtual buttons while device is lying flat</label>";
  page += "<label class='field'><input type='checkbox' name='autoBrightness'" + String(adaptiveBrightness ? " checked" : "") + "> Automatic brightness (day 07:00–20:59)</label>";
  page += "<label class='field'>Day brightness: <output id='dayOut'>" + String(dayBrightness) + "%</output><input type='range' min='10' max='100' step='5' name='dayBrightness' value='" + String(dayBrightness) + "' oninput='dayOut.value=this.value+\"%\"'></label>";
  page += "<label class='field'>Night brightness: <output id='nightOut'>" + String(nightBrightness) + "%</output><input type='range' min='5' max='100' step='5' name='nightBrightness' value='" + String(nightBrightness) + "' oninput='nightOut.value=this.value+\"%\"'></label>";
  page += "<label class='field'>Alarm volume: <output id='volumeOut'>" + String(alarmVolume) + "%</output><input type='range' min='10' max='100' step='5' name='alarmVolume' value='" + String(alarmVolume) + "' oninput='volumeOut.value=this.value+\"%\"'></label>";
  const char* allSoundNames[] = {"Da Ban", "Chime", "Stream", "Water drop"};
  page += "<label class='field'>Alarm sound<select id='alarmSound' name='alarmSound'>";
  for(int i=0;i<4;++i) page += "<option value='"+String(i)+"'"+(alarmSound==i?" selected":"")+">"+allSoundNames[i]+"</option>";
  page += "</select></label><button type='button' onclick='fetch(\"/preview?sound=\"+document.getElementById(\"alarmSound\").value+\"&volume=\"+document.querySelector(\"[name=alarmVolume]\").value)'>Preview alarm sound on Core2</button>";
  page += "<label class='field'>Screen automatically turns off after<select name='screenOff'>";
  const uint16_t screenOffValues[] = {0, 30, 60, 300, 600, 1800};
  const char* screenOffNames[] = {"Never", "30 seconds", "1 minute", "5 minutes", "10 minutes", "30 minutes"};
  for (int i = 0; i < 6; ++i) page += "<option value='" + String(screenOffValues[i]) + "'" + (screenOffSeconds == screenOffValues[i] ? " selected" : "") + ">" + screenOffNames[i] + "</option>";
  page += "</select></label>";
  page += "<h2>Night light</h2>";
  page += "<label class='field'><input type='checkbox' name='nightLight'" + String(nightLightEnabled ? " checked" : "") + "> Enable Bottom2 night light when screen is off</label>";
  page += "<label class='field'>Color<input type='color' name='nightColor' value='" + colorHex(nightLightColor) + "'></label>";
  page += "<label class='field'>Brightness: <output id='nightLedOut'>" + String(nightLightBrightness) + "%</output><input type='range' min='1' max='100' name='nightLedBrightness' value='" + String(nightLightBrightness) + "' oninput='nightLedOut.value=this.value+\"%\"'></label>";
  page += "<label class='field'>Mode<select name='nightLightMode'><option value='0'" + String(nightLightMode==0?" selected":"") + ">Stay on while screen is off</option><option value='1'" + String(nightLightMode==1?" selected":"") + ">Turn off after set time</option><option value='2'" + String(nightLightMode==2?" selected":"") + ">Fade out after set time</option></select></label>";
  page += "<label class='field'>On time (seconds)<input type='number' min='5' max='3600' name='nightLightSeconds' value='" + String(nightLightSeconds) + "'></label>";
  page += "<h2>Alarm light reminder</h2>";
  page += "<label class='field'><input type='checkbox' name='alarmLight'" + String(alarmLightEnabled?" checked":"") + "> Enable Bottom2 alarm lighting</label>";
  page += "<label class='field'>Color<input type='color' name='alarmLightColor' value='" + colorHex(alarmLightColor) + "'></label>";
  page += "<label class='field'>Brightness: <output id='alarmLedOut'>" + String(alarmLightBrightness) + "%</output><input type='range' min='1' max='100' name='alarmLightBrightness' value='" + String(alarmLightBrightness) + "' oninput='alarmLedOut.value=this.value+\"%\"'></label>";
  const char* alarmModeNames[] = {"Continuous breathing", "Continuous fast flash", "Three breathing cycles", "Three fast-flash cycles"};
  page += "<label class='field'>Mode<select name='alarmLightMode'>";
  for(int i=0;i<4;++i) page += "<option value='"+String(i)+"'"+(alarmLightMode==i?" selected":"")+">"+alarmModeNames[i]+"</option>";
  page += "</select></label>";
  page += "<h2>Meditation timer</h2>";
  page += "<label class='field'>Preset time 1 (minutes)<input type='number' min='1' max='180' name='medPreset1' value='"+String(meditationPresetMinutes[0])+"'></label>";
  page += "<label class='field'>Preset time 2 (minutes)<input type='number' min='1' max='180' name='medPreset2' value='"+String(meditationPresetMinutes[1])+"'></label>";
  page += "<label class='field'><input type='checkbox' name='medSoundEnabled'"+String(meditationSoundEnabled?" checked":"")+"> Enable sound reminders</label>";
  const char* medSoundNames[] = {"Da Ban", "Chime", "Stream", "Water drop"};
  page += "<label class='field'>Start sound<select id='medStartSound' name='medStartSound'>";
  for(int i=0;i<4;++i) page += "<option value='"+String(i)+"'"+(meditationStartSound==i?" selected":"")+">"+medSoundNames[i]+"</option>";
  page += "</select></label><label class='field'>Start volume: <output id='medStartOut'>"+String(meditationStartVolume)+"%</output><input id='medStartVolume' type='range' min='5' max='100' step='5' name='medStartVolume' value='"+String(meditationStartVolume)+"' oninput='medStartOut.value=this.value+\"%\"'></label><button type='button' onclick='previewSound(\"start\")'>Preview start sound on Core2</button>";
  page += "<label class='field'>Time-up sound<select id='medEndSound' name='medEndSound'>";
  for(int i=0;i<4;++i) page += "<option value='"+String(i)+"'"+(meditationEndSound==i?" selected":"")+">"+medSoundNames[i]+"</option>";
  page += "</select></label><label class='field'>Time-up volume: <output id='medEndOut'>"+String(meditationEndVolume)+"%</output><input id='medEndVolume' type='range' min='5' max='100' step='5' name='medEndVolume' value='"+String(meditationEndVolume)+"' oninput='medEndOut.value=this.value+\"%\"'></label><button type='button' onclick='previewSound(\"end\")'>Preview time-up sound on Core2</button>";
  page += "<label class='field'><input type='checkbox' name='medLightEnabled'"+String(meditationLightEnabled?" checked":"")+"> Enable meditation lighting effects</label>";
  page += "<label class='field'><input type='checkbox' name='medNoiseEnabled'"+String(meditationNoiseEnabled?" checked":"")+"> Play background sound during countdown</label>";
  const char* noiseNames[] = {"Stream", "Rain (original)", "Summer night insects (original)"};
  page += "<label class='field'>Background sound<select id='medNoise' name='medNoise'>";
  for(int i=0;i<3;++i) page += "<option value='"+String(i)+"'"+(meditationNoise==i?" selected":"")+">"+noiseNames[i]+"</option>";
  page += "</select></label><label class='field'>Background volume: <output id='medNoiseOut'>"+String(meditationNoiseVolume)+"%</output><input id='medNoiseVolume' type='range' min='5' max='80' step='5' name='medNoiseVolume' value='"+String(meditationNoiseVolume)+"' oninput='medNoiseOut.value=this.value+\"%\"'></label><button type='button' onclick='fetch(\"/preview-ambient?sound=\"+document.getElementById(\"medNoise\").value+\"&volume=\"+document.getElementById(\"medNoiseVolume\").value)'>Preview background sound on Core2</button>";
  page += "<h2>Alarms</h2><p>Choose a time, enable the alarm, and select its repeat days. No selected day means one-time.</p>";
  const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int i = 0; i < ALARM_COUNT; ++i) {
    char timeValue[6]; snprintf(timeValue, sizeof(timeValue), "%02u:%02u", alarms[i].hour, alarms[i].minute);
    page += "<details class='alarm'" + String(i < 4 ? " open" : "") + "><summary>Alarm " + String(i + 1) + " — " + timeValue + (alarms[i].enabled ? " (ON)" : " (OFF)") + "</summary>";
    page += "<label class='field'>Time<input type='time' name='a" + String(i) + "_time' value='" + timeValue + "'></label>";
    page += "<label><input type='checkbox' name='a" + String(i) + "_on'" + String(alarms[i].enabled ? " checked" : "") + "> Enabled</label><div class='days'>";
    for (int d = 0; d < 7; ++d) page += "<label><input type='checkbox' name='a" + String(i) + "_d" + String(d) + "'" + String((alarms[i].weekdays & (1 << d)) ? " checked" : "") + ">" + dayNames[d] + "</label>";
    page += "</div></details>";
  }
  page += "<h2>MQTT</h2><p>Live time and meditation data: <b>base topic/state</b> (every second). Full settings: <b>base topic/settings</b>. Send partial JSON settings to <b>base topic/set</b>; result: <b>base topic/ack</b>. Existing commands remain at <b>base topic/command/#</b>.</p>";
  page += "<a class='button' href='/mqtt-guide'>Open complete MQTT guide</a>";
  page += "<label class='field'><input type='checkbox' name='mqttEnabled'"+String(mqttEnabled?" checked":"")+"> Enable MQTT</label>";
  page += "<label class='field'>Broker host/IP<input name='mqttHost' value='"+htmlEscape(mqttHost)+"'></label><label class='field'>Port<input type='number' min='1' max='65535' name='mqttPort' value='"+String(mqttPort)+"'></label>";
  page += "<label class='field'>Username<input name='mqttUsername' value='"+htmlEscape(mqttUsername)+"'></label><label class='field'>Password<input type='password' name='mqttPassword' placeholder='Leave blank to keep current'></label><label class='field'>Base topic<input name='mqttBaseTopic' value='"+htmlEscape(mqttBaseTopic)+"'></label>";
  page += "<h2>Companion pages</h2><p>The local host is preferred automatically. If unavailable, the Internet WebSocket URL is used. Either field may be blank.</p>";
  for (int i = 0; i < COMPANION_PAGE_COUNT; ++i) {
    page += "<div class='alarm'><b>Page " + String(i + 1) + "</b>";
    page += "<label class='field'>Host name<input name='compName" + String(i) + "' maxlength='24' value='" + htmlEscape(companionNames[i]) + "'></label>";
    page += "<label class='field'>Local host/IP<input name='compHost" + String(i) + "' placeholder='10.43.50.145' value='" + htmlEscape(companionHosts[i]) + "'></label>";
    page += "<label class='field'>Local TCP port<input type='number' min='1' max='65535' name='compPort" + String(i) + "' value='" + String(companionPorts[i]) + "'></label>";
    page += "<label class='field'>Internet WebSocket URL<input name='compRemote" + String(i) + "' placeholder='https://example.com/satellite' value='" + htmlEscape(companionInternetUrls[i]) + "'></label></div>";
  }
  page += "<h2>Automatic firmware update</h2><label class='field'><input type='checkbox' name='fwAuto'"+String(automaticFirmwareUpdate?" checked":"")+"> Automatically install new firmware from GitHub</label>";
  page += "<label class='field'>Daily check hour<select name='fwHour'>";
  for (int hour=0;hour<24;++hour) { char label[7]; snprintf(label,sizeof(label),"%02d:00",hour); page += "<option value='"+String(hour)+"'"+(firmwareCheckHour==hour?" selected":"")+">"+String(label)+"</option>"; }
  page += "</select></label>";
  page += "<button type='submit'>Save settings</button></form><script>function previewSound(k){const s=document.getElementById(k==='start'?'medStartSound':'medEndSound').value,v=document.getElementById(k==='start'?'medStartVolume':'medEndVolume').value;fetch('/preview?sound='+s+'&volume='+v);}</script>";
  page += "<h2>Firmware update</h2><p>Current version: <b>" SPACE_CLOCK_VERSION "</b>. Automatic update can also be configured on the Core2 under Settings → Firmware update.</p>";
  page += "<a class='button' href='/update'>Open wireless firmware update</a></body></html>";
  settingsServer.send(200, "text/html; charset=utf-8", page);
}

void sendFirmwareUpdatePage(const String& error = "") {
  String page;
  page.reserve(3500);
  page = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Firmware update</title><style>body{font-family:system-ui;background:#08111f;color:#eef4ff;max-width:620px;margin:auto;padding:20px}"
         "h1{color:#65b9ff}.box{background:#101d2e;border:1px solid #344b63;border-radius:10px;padding:16px}input{box-sizing:border-box;width:100%;padding:12px;margin-top:12px}"
         "button,a{box-sizing:border-box;display:block;width:100%;padding:13px;margin-top:18px;border:0;border-radius:9px;background:#1688e5;color:white;font-size:17px;text-align:center;text-decoration:none}.error{color:#ff7d7d}</style></head><body>"
         "<h1>Wireless firmware update</h1><div class='box'><p>Keep the Core2 powered and connected to Wi-Fi until it restarts.</p>";
  if (error.length()) page += "<p class='error'>" + htmlEscape(error) + "</p>";
  page += "<form method='post' action='/update' enctype='multipart/form-data' onsubmit='document.getElementById(\"status\").textContent=\"Uploading and installing... Do not turn off power.\"'>"
          "<input type='file' name='firmware' accept='.bin,application/octet-stream' required><button type='submit'>Install firmware</button></form>"
          "<p id='status'></p></div><a href='/'>Back to settings</a></body></html>";
  settingsServer.send(200, "text/html; charset=utf-8", page);
}

void setupSettingsServer() {
  settingsServer.on("/", HTTP_GET, []() { sendSettingsPage(); });
  settingsServer.on("/mqtt-guide", HTTP_GET, []() {
    settingsServer.send_P(200, "text/html; charset=utf-8", MQTT_GUIDE_HTML);
  });
  settingsServer.on("/preview", HTTP_GET, []() {
    uint8_t sound = constrain(settingsServer.arg("sound").toInt(), 0, 3);
    uint8_t volume = constrain(settingsServer.arg("volume").toInt(), 5, 100);
    bool enabledBeforePreview = meditationSoundEnabled;
    meditationSoundEnabled = true;
    playMeditationSound(sound, volume);
    meditationSoundEnabled = enabledBeforePreview;
    settingsServer.send(200, "text/plain", "Playing on Core2");
  });
  settingsServer.on("/preview-ambient", HTTP_GET, []() {
    uint8_t oldSound=meditationNoise, oldVolume=meditationNoiseVolume; bool oldEnabled=meditationNoiseEnabled; MeditationState oldState=meditationState;
    meditationNoise=constrain(settingsServer.arg("sound").toInt(),0,2);
    meditationNoiseVolume=constrain(settingsServer.arg("volume").toInt(),5,80);
    meditationNoiseEnabled=true; meditationState=MeditationState::Running;
    M5.Speaker.stop(1); playMeditationAmbient(1);
    meditationNoise=oldSound; meditationNoiseVolume=oldVolume; meditationNoiseEnabled=oldEnabled; meditationState=oldState;
    settingsServer.send(200,"text/plain","Playing on Core2");
  });
  settingsServer.on("/update", HTTP_GET, []() { sendFirmwareUpdatePage(); });
  settingsServer.on("/update", HTTP_POST,
    []() {
      bool success = !Update.hasError();
      settingsServer.sendHeader("Connection", "close");
      if (success) {
        settingsServer.send(200, "text/html; charset=utf-8",
          "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'><style>body{font-family:system-ui;background:#08111f;color:#eef4ff;padding:30px}h1{color:#70e39a}</style><h1>Update complete</h1><p>Core2 is restarting. Reopen the device IP in about 15 seconds.</p>");
        delay(700);
        ESP.restart();
      } else {
        sendFirmwareUpdatePage("Update failed (error " + String(Update.getError()) + "). The existing firmware is still available; please check the file and try again.");
      }
    },
    []() {
      HTTPUpload& upload = settingsServer.upload();
      if (upload.status == UPLOAD_FILE_START) {
        lastUserActivity = millis();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        lastUserActivity = millis();
        if (!Update.hasError() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.hasError() && !Update.end(true)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
      }
    });
  settingsServer.on("/save", HTTP_POST, []() {
    bool wifiChanged = false;
    for (int i = 0; i < SAVED_WIFI_COUNT; ++i) {
      String nextSsid = settingsServer.arg("wifiS" + String(i)); nextSsid.trim();
      String nextPassword = settingsServer.arg("wifiP" + String(i));
      if (settingsServer.hasArg("wifiD" + String(i))) { nextSsid = ""; nextPassword = ""; }
      else if (!nextPassword.length() && nextSsid == savedWifiSsids[i]) nextPassword = savedWifiPasswords[i];
      wifiChanged |= nextSsid != savedWifiSsids[i] || nextPassword != savedWifiPasswords[i];
      savedWifiSsids[i] = nextSsid; savedWifiPasswords[i] = nextPassword;
    }
    timeZoneIndex = constrain(settingsServer.arg("timeZone").toInt(), 0, (int)TIME_ZONE_COUNT - 1);
    clockFace = static_cast<ClockFace>(constrain(settingsServer.arg("face").toInt(), 0, 2));
    matrixRainSpeed = constrain(settingsServer.arg("matrixSpeed").toInt(), 10, 100);
    matrixRainDensity = constrain(settingsServer.arg("matrixDensity").toInt(), 10, 100);
    matrixGlyphScale = constrain(settingsServer.arg("matrixSize").toInt(), 1, 2);
    matrixRainColor = parseWebColor(settingsServer.arg("matrixColor"), matrixRainColor);
    matrixGlassOpacity = constrain(settingsServer.arg("matrixGlass").toInt(), 15, 90);
    resetMatrixRain();
    use24HourTime = settingsServer.arg("time24").toInt() != 0;
    flatVirtualButtonsEnabled = settingsServer.hasArg("flatButtons");
    adaptiveBrightness = settingsServer.hasArg("autoBrightness");
    dayBrightness = constrain(settingsServer.arg("dayBrightness").toInt(), 10, 100);
    nightBrightness = constrain(settingsServer.arg("nightBrightness").toInt(), 5, 100);
    alarmVolume = constrain(settingsServer.arg("alarmVolume").toInt(), 10, 100);
    alarmSound = constrain(settingsServer.arg("alarmSound").toInt(), 0, 3);
    screenOffSeconds = constrain(settingsServer.arg("screenOff").toInt(), 0, 1800);
    automaticFirmwareUpdate = settingsServer.hasArg("fwAuto");
    firmwareCheckHour = constrain(settingsServer.arg("fwHour").toInt(), 0, 23);
    nightLightEnabled = settingsServer.hasArg("nightLight");
    nightLightColor = parseWebColor(settingsServer.arg("nightColor"), nightLightColor);
    nightLightBrightness = constrain(settingsServer.arg("nightLedBrightness").toInt(), 1, 100);
    nightLightMode = constrain(settingsServer.arg("nightLightMode").toInt(), 0, 2);
    nightLightSeconds = constrain(settingsServer.arg("nightLightSeconds").toInt(), 5, 3600);
    alarmLightEnabled = settingsServer.hasArg("alarmLight");
    alarmLightColor = parseWebColor(settingsServer.arg("alarmLightColor"), alarmLightColor);
    alarmLightBrightness = constrain(settingsServer.arg("alarmLightBrightness").toInt(), 1, 100);
    alarmLightMode = constrain(settingsServer.arg("alarmLightMode").toInt(), 0, 3);
    meditationPresetMinutes[0] = constrain(settingsServer.arg("medPreset1").toInt(), 1, 180);
    meditationPresetMinutes[1] = constrain(settingsServer.arg("medPreset2").toInt(), 1, 180);
    meditationSoundEnabled = settingsServer.hasArg("medSoundEnabled");
    meditationStartSound = constrain(settingsServer.arg("medStartSound").toInt(), 0, 3);
    meditationStartVolume = constrain(settingsServer.arg("medStartVolume").toInt(), 5, 100);
    meditationEndSound = constrain(settingsServer.arg("medEndSound").toInt(), 0, 3);
    meditationEndVolume = constrain(settingsServer.arg("medEndVolume").toInt(), 5, 100);
    meditationLightEnabled = settingsServer.hasArg("medLightEnabled");
    meditationNoiseEnabled = settingsServer.hasArg("medNoiseEnabled");
    meditationNoise = constrain(settingsServer.arg("medNoise").toInt(),0,2);
    meditationNoiseVolume = constrain(settingsServer.arg("medNoiseVolume").toInt(),5,80);
    mqttEnabled = settingsServer.hasArg("mqttEnabled");
    mqttHost = settingsServer.arg("mqttHost");
    mqttPort = constrain(settingsServer.arg("mqttPort").toInt(),1,65535);
    mqttUsername = settingsServer.arg("mqttUsername");
    if(settingsServer.arg("mqttPassword").length()) mqttPassword=settingsServer.arg("mqttPassword");
    mqttBaseTopic = settingsServer.arg("mqttBaseTopic"); mqttBaseTopic.trim();
    while(mqttBaseTopic.endsWith("/")) mqttBaseTopic.remove(mqttBaseTopic.length()-1);
    mqttClient.disconnect();
    for (int i = 0; i < ALARM_COUNT; ++i) {
      String prefix = "a" + String(i);
      uint8_t oldHour = alarms[i].hour, oldMinute = alarms[i].minute, oldWeekdays = alarms[i].weekdays;
      bool oldEnabled = alarms[i].enabled;
      String alarmTime = settingsServer.arg(prefix + "_time");
      if (alarmTime.length() >= 5) {
        alarms[i].hour = constrain(alarmTime.substring(0, 2).toInt(), 0, 23);
        alarms[i].minute = constrain(alarmTime.substring(3, 5).toInt(), 0, 59);
      }
      alarms[i].enabled = settingsServer.hasArg(prefix + "_on");
      alarms[i].weekdays = 0;
      for (int d = 0; d < 7; ++d) if (settingsServer.hasArg(prefix + "_d" + String(d))) alarms[i].weekdays |= 1 << d;
      if (oldHour != alarms[i].hour || oldMinute != alarms[i].minute || oldWeekdays != alarms[i].weekdays || (!oldEnabled && alarms[i].enabled)) {
        alarms[i].lastDay = -1;
      }
    }
    bool reconnectCompanion = false;
    for (int i = 0; i < COMPANION_PAGE_COUNT; ++i) {
      String nextHost = settingsServer.arg("compHost" + String(i));
      String nextRemote = settingsServer.arg("compRemote" + String(i));
      String nextName = settingsServer.arg("compName" + String(i)); nextName.trim();
      uint16_t nextPort = (uint16_t)constrain(settingsServer.arg("compPort" + String(i)).toInt(), 1, 65535);
      if (nextHost != companionHosts[i] || nextRemote != companionInternetUrls[i] || nextPort != companionPorts[i]) reconnectCompanion = true;
      companionNames[i] = nextName.length() ? nextName : "Page " + String(i + 1);
      companionHosts[i] = nextHost;
      companionInternetUrls[i] = nextRemote;
      companionPorts[i] = nextPort;
    }
    if (reconnectCompanion) {
      stopCompanion();
      clearCompanionPageData();
    }
    saveSettings();
    sendSettingsPage("Settings saved. The clock will apply them now.");
    if (wifiChanged) {
      delay(300); WiFi.disconnect(false); connectSavedWifi();
    } else {
      syncTime();
      m5::rtc_datetime_t brightnessNow; getClockDateTime(&brightnessNow); applyDisplayBrightness(brightnessNow);
      drawClock(true); drawAstronaut();
    }
  });
  settingsServer.onNotFound([]() { settingsServer.sendHeader("Location", "/"); settingsServer.send(302); });
  settingsServer.begin();
  settingsServerReady = true;
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true; cfg.internal_rtc = true; cfg.internal_imu = true;
  M5.begin(cfg);
  setCpuFrequencyMhz(160);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Display.setColorDepth(16);
  M5.Display.setBrightness(100);
  M5.setTouchButtonHeight(0);
  bottomLeds.begin();
  bottomLeds.clear();
  bottomLeds.show();
  astronautCanvas.setColorDepth(16);
  astronautCanvas.createSprite(105, 130);
  companionButtonCanvas.setColorDepth(16);
  companionButtonCanvas.createSprite(96, 96);
  meditationCardCanvas.setColorDepth(16);
  meditationCardCanvas.createSprite(98, 96);
  matrixCanvas.setColorDepth(16);
  matrixCanvas.createSprite(320, 240);
  loadSettings();
  lastUserActivity = millis();
  m5::rtc_datetime_t startupTime; getClockDateTime(&startupTime); applyDisplayBrightness(startupTime);
  WiFi.mode(WIFI_STA); WiFi.setSleep(true); WiFi.begin();
  drawClock(true); drawAstronaut();
  if (WiFi.waitForConnectResult(4000) != WL_CONNECTED) {
    if (!connectSavedWifi() && strlen(DEFAULT_WIFI_SSID)) {
      WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
      WiFi.waitForConnectResult(10000);
    }
  }
  if (WiFi.status() == WL_CONNECTED) { setupSettingsServer(); syncTime(); drawClock(true); drawAstronaut(); }
}

void loop() {
  M5.update();
  if (WiFi.status() == WL_CONNECTED) settingsServer.handleClient();
  if (companionWebSocketMode) { companionWebSocket.loop(); probeAndPreferLocalCompanion(millis()); }
  if (!companionWebSocketMode && companionClient.connected()) {
    while (companionClient.available()) processCompanionLine(companionClient.readStringUntil('\n'));
    static uint32_t lastCompanionPing = 0;
    if (millis() - lastCompanionPing > 2000) { lastCompanionPing = millis(); sendCompanionMessage("PING " + String(millis()) + "\n"); }
  } else if (!companionWebSocketMode && screenNow == Screen::Companion) connectCompanion();
  handleTouch();
  handleSerialConfig();
  uint32_t nowMs = millis();
  maintainSavedWifi(nowMs);
  maintainMqtt(nowMs);
  if (meditationAmbientPendingAt && (int32_t)(nowMs - meditationAmbientPendingAt) >= 0) {
    meditationAmbientPendingAt = 0;
    playMeditationAmbient();
  }
  if (meditationState == MeditationState::Running && meditationElapsedSeconds() >= meditationDurationSeconds) {
    M5.Speaker.stop(1);
    meditationElapsedBeforeRun = meditationDurationSeconds;
    meditationState = MeditationState::Done;
    meditationLightEventStarted = nowMs;
    playMeditationSound(meditationEndSound, meditationEndVolume);
    drawMeditation();
  }
  updateAlarmBaseLights(nowMs);
  if (!screenSleeping && alarmActive < 0 && screenOffSeconds > 0 && nowMs - lastUserActivity >= (uint32_t)screenOffSeconds * 1000UL) {
    screenSleeping = true;
    screenSleepStarted = nowMs;
    motionBaselineReady = false;
    M5.Display.setBrightness(0);
  }
  checkMotionWake(nowMs);
  static uint32_t lastAlarmCheck = 0;
  if (nowMs - lastAlarmCheck >= 1000) {
    lastAlarmCheck = nowMs;
    if (alarmActive < 0) {
      if (snoozedAlarm >= 0 && nowMs - snoozeStarted >= 300000UL) {
        int index = snoozedAlarm; snoozedAlarm = -1;
        if (alarms[index].enabled) startAlarm(index);
      } else if (snoozedAlarm < 0) {
        m5::rtc_datetime_t alarmNow; getClockDateTime(&alarmNow);
        checkAlarms(alarmNow);
        checkAutomaticFirmwareUpdate(alarmNow);
      }
    }
  }
  if (screenNow == Screen::Clock && nowMs - lastClockDraw >= 1000) {
    lastClockDraw = nowMs;
    m5::rtc_datetime_t dt; getClockDateTime(&dt);
    bool minuteChanged = dt.time.minutes != lastMinute;
    if (minuteChanged) {
      lastMinute = dt.time.minutes;
    }
    if (alarmActive < 0 && (minuteChanged || clockFace != ClockFace::Space)) drawClock(false);
  }
  drawMatrixRainFrame(nowMs);
  if (screenNow == Screen::Meditation && nowMs - lastClockDraw >= 1000) {
    lastClockDraw = nowMs;
    drawMeditation(false);
  }
  static uint32_t lastPowerStatusDraw = 0;
  if (screenNow == Screen::Clock && alarmActive < 0 && nowMs - lastPowerStatusDraw >= 5000UL) {
    lastPowerStatusDraw = nowMs;
    drawClockStatus(clockFace == ClockFace::Matrix ? TFT_BLACK : BG);
  }
  uint32_t astronautFrameMs = M5.Power.isCharging() ? 120UL : 220UL;
  if (screenNow == Screen::Clock && clockFace == ClockFace::Space && alarmActive < 0 && nowMs - lastAnim >= astronautFrameMs) {
    lastAnim = nowMs;
    astronautX += astronautDX; astronautY += astronautDY;
    if (astronautX < 5 || astronautX > 8) astronautDX = -astronautDX;
    if (astronautY < 88 || astronautY > 118) astronautDY = -astronautDY;
    drawAstronaut();
  }
  static uint32_t lastNtpRefresh = 0;
  if (WiFi.status() == WL_CONNECTED && nowMs - lastNtpRefresh >= 21600000UL) {
    lastNtpRefresh = nowMs;
    syncTime();
  }
  static uint32_t lastBrightnessUpdate = 0;
  if (nowMs - lastBrightnessUpdate >= 30000UL) {
    lastBrightnessUpdate = nowMs;
    m5::rtc_datetime_t brightnessNow; getClockDateTime(&brightnessNow); applyDisplayBrightness(brightnessNow);
  }
  delay(screenSleeping ? 20 : 5);
}
