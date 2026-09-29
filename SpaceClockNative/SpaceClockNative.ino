#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <Adafruit_NeoPixel.h>
#include <PubSubClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <mp3dec.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
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
#include "emotion_tls.h"
#if defined(SPACE_CLOCK_PUBLIC_BUILD)
// Public OTA releases must never include a developer's ignored local Wi-Fi
// credentials, even when wifi_secrets.h exists in the build directory.
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
#else
#include "wifi_secrets.h"
#endif
#include "mqtt_guide.h"

enum class Screen : uint8_t { Clock, Menu, Faces, Companion, Alarms, Settings, Meditation, MeditationSettings, EmotionObservation, EmotionRecords, EmotionSettings, EmotionReminder, HassAssist, FirmwareUpdate, About, NightLight, Calendar };
enum class ClockFace : uint8_t { Space, Minimal, Matrix };
enum class MeditationState : uint8_t { Ready, Running, Paused, Done };
// Calendar (iCal subscription) state; drawing and parsing live further down.
struct CalEvent {
  time_t start;
  time_t end;
  bool allDay;
  char title[72];
  char location[48];
};
enum class CalView : uint8_t { Day, Week, Month };
String calendarIcalUrl;
CalEvent* calEvents = nullptr;
int calEventCount = 0;
static constexpr int CAL_MAX_EVENTS = 400;
time_t calWindowStart = 0, calWindowEnd = 0;
uint32_t calFetchedAt = 0;
String calError;
CalView calView = CalView::Day;
time_t calViewDay = 0;  // local midnight of the selected day

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
bool alarmGearDragging = false;
float alarmGearProgress = 0.0f;
float alarmGearLastAngle = 0.0f;
bool alarmPillHolding = false;
uint32_t alarmPillHoldStarted = 0;
uint32_t lastAlarmChallengeDraw = 0;
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
// A long press can temporarily force the night light on or off without
// changing the automatic screen-off night-light preference.
bool manualNightLightOverride = false;
bool manualNightLightActive = false;
// True while the side LEDs are showing the night-light colour (manual night
// light, or the sleep night light). Double-tapping then turns the whole
// screen into a night light of the same colour and brightness.
bool nightLedShowing = false;
uint8_t screenNightBrightness = 0;   // 0 = follow LED brightness
uint32_t screenNightLabelUntil = 0;
String touchDebugLog;  // recent touch events, served at /debug
bool screenSleeping = false;
bool automaticFirmwareUpdate = false;
uint8_t firmwareCheckHour = 3;
int32_t lastAutomaticUpdateDay = -1;
String latestFirmwareVersion;
String latestFirmwareUrl;
String latestFirmwareSha256;
uint32_t latestFirmwareExpectedSize = 0;
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
uint32_t clockSettingsPressedAt = 0;
bool clockSettingsPressValid = false;
uint32_t clockMiddlePressedAt = 0;
bool clockMiddlePressValid = false;
WiFiClient companionClient;
WebSocketsClient companionWebSocket;
bool companionWebSocketMode = false;
bool companionWebSocketConnected = false;
bool companionUsingInternet = false;
uint32_t lastCompanionLocalProbe = 0;
// Home Assistant Assist uses the official WebSocket pipeline API. The token
// stays in Preferences and is never emitted to MQTT, the UI, or public builds.
bool hassAssistEnabled = false;
String hassAssistBaseUrl;
String hassAssistToken;
String hassAssistPipeline;
uint8_t hassAssistVolume = 70;
bool hassAssistWakeWordEnabled = false;
// 0 = tap to start / tap to send, 1 = hold to talk, 2 = always-on wake word.
enum : uint8_t { HASS_MODE_TAP = 0, HASS_MODE_HOLD = 1, HASS_MODE_WAKE = 2 };
uint8_t hassAssistVoiceMode = HASS_MODE_TAP;
uint32_t hassAssistWakeStageStartedAt = 0;
uint8_t hassAssistWakeEmptyStreak = 0;
uint32_t hassAssistTapStartedAt = 0;
uint32_t hassAssistLastEventAt = 0;
// Set when Assist was opened by a shortcut (hold on the clock, or a wake word
// heard on another screen); the clock returns once the reply has finished.
bool hassAssistReturnToClock = false;
// Set when the user interrupts a reply; late tts-end events are ignored.
bool hassAssistReplyInterrupted = false;
uint32_t hassAssistReturnAt = 0;
bool hassAssistWakeWordPaused = false;
static constexpr uint8_t HASS_ASSIST_MAX_PIPELINES = 10;
String hassAssistPipelineIds[HASS_ASSIST_MAX_PIPELINES];
String hassAssistPipelineNames[HASS_ASSIST_MAX_PIPELINES];
uint8_t hassAssistPipelineCount = 0;
String hassAssistPreferredPipeline;
String hassAssistDiscoveryError;
enum class HassAssistState : uint8_t { Disabled, Disconnected, Connecting, Authenticating, Ready, Paused, WaitingWakeWord, Starting, Listening, Processing, Downloading, Speaking, Error };
HassAssistState hassAssistState = HassAssistState::Disabled;
WebSocketsClient hassAssistWebSocket;
bool hassAssistSocketStarted = false;
bool hassAssistSocketConnected = false;
bool hassAssistAuthenticated = false;
uint32_t hassAssistCommandId = 400;
uint32_t hassAssistActiveCommandId = 0;
uint32_t hassAssistPipelineListCommandId = 0;
bool hassAssistPipelineActive = false;
bool hassAssistWakeSessionActive = false;
bool hassAssistWakeDetected = false;
uint32_t hassAssistRestartAt = 0;
int hassAssistAudioHandlerId = -1;
bool hassAssistHolding = false;
bool hassAssistStopRequested = false;
bool hassAssistMicRunning = false;
bool hassAssistToggleListen = false;
bool hassAssistTouchLongStarted = false;
uint32_t hassAssistTouchStartedAt = 0;
static constexpr uint32_t HASS_ASSIST_LONG_PRESS_MS = 600;
static constexpr uint32_t HASS_ASSIST_HOLD_START_MS = 200;
// 100 ms per chunk: two queued chunks give 200 ms of headroom so a display
// redraw or TLS write in loop() no longer drops PCM between buffers.
static constexpr size_t HASS_MIC_SAMPLES = 1600;
int16_t hassAssistMicBuffers[4][HASS_MIC_SAMPLES] = {};
uint8_t hassAssistMicQueueIndex = 0;
uint8_t hassAssistMicSendIndex = 0;
uint8_t hassAssistMicOutstanding = 0;
std::atomic<uint8_t> hassAssistMicReadyMask{0};
uint32_t hassAssistMicChunksSent = 0;
uint32_t hassAssistMicBytesSent = 0;
uint16_t hassAssistMicPeak = 0;
String hassAssistLastEvent;
uint32_t hassAssistListenStarted = 0;
String hassAssistTranscript;
String hassAssistReply;
String hassAssistError;
String hassAssistTtsUrl;
String hassAssistTtsMime;
bool hassAssistTtsPending = false;
uint8_t* hassAssistAudioData = nullptr;
size_t hassAssistAudioLength = 0;
HMP3Decoder hassAssistMp3Decoder = nullptr;
size_t hassAssistMp3Position = 0;
static constexpr size_t HASS_MP3_FRAME_SAMPLES = 1152;
int16_t hassAssistMp3DecodeBuffer[HASS_MP3_FRAME_SAMPLES * 2] = {};
int16_t hassAssistMp3Buffers[3][HASS_MP3_FRAME_SAMPLES] = {};
uint8_t hassAssistMp3BufferIndex = 0;
bool hassAssistAudioDecodeDone = false;
bool hassAssistTouchActive = false;
WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
bool mqttEnabled = false;
String mqttHost;
uint16_t mqttPort = 1883;
String mqttUsername;
String mqttPassword;
String mqttBaseTopic = "spaceclock/core2";
String deviceName = "Space Clock";
// Emotion observation API and reminder configuration. Passwords are never
// persisted; the short-lived login response token is kept on the device only.
uint8_t emotionReminderMode = 0; // 0 off, 1 scheduled interval, 2 fixed times
uint16_t emotionReminderIntervalMinutes = 60;
uint16_t emotionReminderWindowStart = 300;
uint16_t emotionReminderWindowEnd = 1410;
uint16_t emotionReminderTimes[3] = {600, 900, 0xFFFF};
bool emotionReminderVibration = true;
bool emotionReminderSound = false;
uint8_t emotionReminderDurationSeconds = 30;
uint8_t emotionReminderSoundChoice = 1;
uint8_t emotionReminderVolume = 60;
String emotionApiBase = "https://emotion.theoakhouse.org";
String emotionApiIdentity;
String emotionApiUserId;
String emotionApiToken;
enum class EmotionApiState : uint8_t { Unknown, Checking, Connected, Disconnected };
EmotionApiState emotionApiState = EmotionApiState::Unknown;
uint32_t emotionApiLastChecked = 0;
uint32_t emotionReminderEnd = 0;
uint32_t emotionReminderSnoozeUntil = 0;
uint32_t emotionLastVibrationToggle = 0;
bool emotionReminderVibrationOn = false;
int32_t emotionLastReminderMinuteKey = -1;
Screen emotionReminderReturnScreen = Screen::Clock;
uint8_t emotionFormPage = 0;
bool emotionSubmitArmed = false;
bool emotionSubmitCompleted = false;
String emotionSubmitMessage;
String emotionLastSubmittedId;
String emotionLastSubmittedPayload;
enum class EmotionRecordsState : uint8_t { Loading, Ready, Error };
EmotionRecordsState emotionRecordsState = EmotionRecordsState::Loading;
uint16_t emotionRecordsLearningDays = 0;
uint32_t emotionRecordsSheetCount = 0;
uint32_t emotionRecordsTodaySheets = 0;
String emotionRecordsAverage = "0.0";
String emotionRecordsTopBody = "-";
String emotionRecordsTopEmotion = "-";
String emotionRecordsStrongest = "-";
String emotionRecordsTopGrounding = "-";
String emotionRecordsError;
uint32_t emotionStatsCacheAt = 0;
uint8_t emotionPendingCount = 0;
uint32_t emotionLastQueueSyncAt = 0;
bool emotionStorageReady = false;
m5::rtc_datetime_t emotionFormTime;
bool emotionTriggers[8] = {};
uint8_t emotionHeartRate = 4, emotionBreathRate = 4, emotionSweating = 0;
int8_t emotionBodySignal = -1, emotionBodyPart = -1, emotionBehaviorCue = 0;
uint8_t emotionCategory = 0, emotionIndexPercent = 50;
int8_t emotionChoice = -1;
uint8_t emotionObserveCount = 1, emotionObserveMinutes = 1;
uint8_t emotionGroundingTiming = 1, emotionGroundingAction = 10;
uint32_t emotionCancelPressedAt = 0;
bool emotionCancelPressValid = false;
uint32_t emotionMiddlePressedAt = 0;  // long-press middle = force sync
uint8_t emotionSettingsPage = 0;
uint8_t emotionRecordsPage = 0;
uint32_t companionNavPressStarted = 0;
bool companionNavPressValid = false;
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
enum class WifiRecoveryPhase : uint8_t { Primary, ScanningProfiles, ConnectingProfiles, ConnectingFallback };
WifiRecoveryPhase wifiRecoveryPhase = WifiRecoveryPhase::Primary;
uint32_t wifiRecoveryPhaseStartedAt = 0;
int wifiRecoverySlots[SAVED_WIFI_COUNT];
int wifiRecoveryRssi[SAVED_WIFI_COUNT];
int wifiRecoveryCount = 0;
int wifiRecoveryIndex = 0;
bool wifiWasConnected = false;
bool wifiDefaultFallbackAttempted = false;
static constexpr uint32_t WIFI_PRIMARY_TIMEOUT_MS = 12000;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 15000;
static constexpr uint32_t WIFI_PROFILE_TIMEOUT_MS = 12000;
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
M5Canvas firmwareProgressCanvas(&M5.Display);
// Off-screen buffers so the clock can tick every second without clearing the
// panel first (clearing then drawing is what shows up as a black flash).
M5Canvas spaceTimeCanvas(&M5.Display);
M5Canvas clockSecondsCanvas(&M5.Display);
bool spaceTimeCanvasReady = false, clockSecondsCanvasReady = false;
bool matrixCanvasReady = false;
bool matrixMemoryErrorDrawn = false;
bool firmwareProgressCanvasReady = false;
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
uint8_t matrixGlint[MATRIX_COLUMNS][MATRIX_MAX_ROWS];
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
  "@", "#", "$", "%", "&", "*", "+", "-", "=", "!", "?", "/", "\\", "<", ">", "[", "]", "{", "}", "~", "^", "|", ":", ";",
  // Katakana give the classic Matrix look (glyphs are in the 8 pt UI font).
  "ア", "イ", "ウ", "エ", "オ", "カ", "キ", "ク", "ケ", "コ", "サ", "シ", "ス", "セ", "ソ", "タ", "チ", "ツ", "テ", "ト", "ナ", "ニ", "ヌ", "ネ", "ノ", "ハ", "ヒ", "フ", "ヘ", "ホ", "マ", "ミ", "ム", "メ", "モ", "ヤ", "ユ", "ヨ", "ラ", "リ", "ル", "レ", "ロ", "ワ", "ヲ", "ン"
};
static constexpr uint8_t MATRIX_GLYPH_COUNT = sizeof(MATRIX_GLYPH_SET) / sizeof(MATRIX_GLYPH_SET[0]);

static constexpr uint16_t BG = 0x0000;
static constexpr uint16_t FG = 0xF79E;
static constexpr uint16_t ACCENT = 0xD229;
static constexpr uint16_t UI_BLUE = 0x2310;
static constexpr uint16_t PANEL = 0x2124;
static constexpr uint16_t UI_PANEL_ALT = 0x18E3;
static constexpr uint16_t UI_BORDER = 0x3A2C;
static constexpr uint16_t UI_MUTED = 0xA534;

static const char* const EMOTION_PAGE_TITLES[] = {
  "時間", "觸發點", "身體反應", "情緒類別", "情緒指數", "觀察次數", "情緒落地", "本筆總覽"
};
static const char* const EMOTION_TRIGGERS[] = {
  "表情", "一句話", "語氣", "動作", "眼神", "無聲的壓力", "惡夢", "記憶點"
};
static const char* const EMOTION_HEART_RATES[] = {
  "不填", "觀察不出來", "很慢", "偏慢", "正常", "偏快", "很快", "心悸亂跳"
};
static const char* const EMOTION_BREATH_RATES[] = {
  "不填", "觀察不出來", "很慢", "偏慢", "正常", "偏快", "很快", "呼吸急促"
};
static const char* const EMOTION_SWEATING[] = {"未勾選", "已勾選"};
static const char* const EMOTION_BODY_SIGNALS[] = {
  "心悸/心口抽緊", "胸悶/胸口壓迫", "呼吸急促", "呼吸變淺", "嘆氣", "肩頸緊繃", "頭痛", "胃部不適",
  "手腳發冷", "身體發熱", "手抖", "冒汗", "想哭", "身體僵住", "坐立難安", "疲倦"
};
static const char* const EMOTION_BODY_PARTS[] = {
  "頭部/臉", "眼睛", "喉嚨", "胸口", "胃部", "腹部", "肩頸", "背部", "手臂", "手掌", "腿部", "腳部", "全身", "不確定"
};
static const char* const EMOTION_BEHAVIOR_CUES[] = {
  "不填", "沉默不語", "反覆確認", "逃避/離開", "提高音量", "哭泣", "發呆", "坐立不安",
  "尋求安慰", "過度解釋", "僵住不動", "急著完成", "退縮", "迎合他人", "衝動行動", "其他可觀察行為"
};
static const char* const EMOTION_CATEGORIES[] = {
  "憤怒", "受傷", "悲傷", "失落", "絕望", "焦慮", "壓力", "懷念", "自我否定", "失敗感"
};
static const uint8_t EMOTION_CHOICE_COUNTS[] = {14, 5, 4, 5, 6, 8, 5, 4, 8, 3};
static const char* const EMOTION_CHOICES[10][14] = {
  {"煩躁", "無奈", "懊惱", "悶悶不樂", "不喜歡", "厭煩", "厭倦", "生氣", "惱火", "焦躁", "憤世嫉俗", "狂怒", "怒不可遏", "悲憤"},
  {"被否定", "被忽視", "被誤解", "被背叛", "委屈"},
  {"難過", "沮喪", "憂鬱", "心碎"},
  {"寂寞", "孤單", "空虛", "無助", "失落"},
  {"後悔", "惋惜", "灰心", "絕望", "身心俱疲", "無力"},
  {"緊張", "焦慮", "不安", "忐忑", "心有餘悸", "害怕", "恐慌", "驚恐"},
  {"壓抑", "迷惘", "不知所措", "窒息", "恐懼"},
  {"懷念", "無常感", "擔心失去", "缺乏安全感"},
  {"自責", "羞愧", "羞恥", "覺得自己不好", "內疚", "虧欠感", "後悔", "毀滅"},
  {"挫敗", "無能感", "不配得感"}
};
static const char* const EMOTION_GROUNDING_TIMES[] = {"不填", "當下", "事後"};
static const char* const EMOTION_GROUNDING_ACTIONS[] = {
  "無操作", "躺下", "摸摸頭", "走路", "放鬆肩膀", "轉脖子", "拜佛", "背書", "念誦", "持咒", "深呼吸",
  "二次呼吸", "腹式呼吸", "拉筋/伸展", "甩手放鬆", "按壓胸口", "按壓手掌", "握拳再放鬆", "喝溫水", "洗臉/沖冷水",
  "踩地感受支撐", "看固定物30秒", "慢慢數呼吸"
};

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

bool drawBottomActionIcon(const char* label, int centerX, int top = 218) {
  if (!label || !label[0]) return false;
  const uint8_t* data = nullptr;
  size_t length = 0;
  if (!strcmp(label, "Previous") || !strcmp(label, "< Previous") || !strcmp(label, "上一頁")) {
    data = action_previous_png; length = action_previous_png_len;
  } else if (!strcmp(label, "Next") || !strcmp(label, "Next >") || !strcmp(label, "下一頁")) {
    data = action_next_png; length = action_next_png_len;
  } else if (!strcmp(label, "Close") || !strcmp(label, "取消") || !strcmp(label, "關閉") ||
             !strcmp(label, "取消送出")) {
    data = action_close_png; length = action_close_png_len;
  } else if (!strcmp(label, "Check") || !strcmp(label, "Save") || !strcmp(label, "完成") ||
             !strcmp(label, "送出") || !strcmp(label, "確認送出") || !strcmp(label, "已送出")) {
    data = action_check_png; length = action_check_png_len;
  } else if (!strcmp(label, "Install")) {
    data = action_install_png; length = action_install_png_len;
  } else if (!strcmp(label, "Clock") || !strcmp(label, "時鐘")) {
    data = action_clock_png; length = action_clock_png_len;
  }
  if (!data) return false;
  M5.Display.drawPng(data, length, centerX - 10, top);
  return true;
}

void drawBottomBar(const char* left, const char* middle, const char* right) {
  M5.Display.fillRect(0, 215, 320, 25, BG);
  useUIFont(1);
  M5.Display.setTextColor(ACCENT, BG);
  M5.Display.setTextDatum(middle_center);
  if (!drawBottomActionIcon(left, 53)) M5.Display.drawString(left, 53, 228);
  if (!drawBottomActionIcon(middle, 160)) M5.Display.drawString(middle, 160, 228);
  if (!drawBottomActionIcon(right, 267)) M5.Display.drawString(right, 267, 228);
}

void title(const char* text) {
  M5.Display.fillScreen(BG);
  M5.Display.setTextColor(0x65DF, BG);
  useUIMediumFont();
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(text, 10, 8);
  M5.Display.drawFastHLine(10, 34, 300, UI_BORDER);
}

// Device settings lists: four large rows per page, filling the screen
// between the title rule and the bottom bar.
static constexpr int SETTINGS_ROW_TOP = 39;
static constexpr int SETTINGS_ROW_PITCH = 44;
int settingsRowAt(int y) {
  if (y < SETTINGS_ROW_TOP || y >= SETTINGS_ROW_TOP + 4 * SETTINGS_ROW_PITCH || y >= 210) return -1;
  return (y - SETTINGS_ROW_TOP) / SETTINGS_ROW_PITCH;
}
void drawSettingsRow(uint8_t index, const String& label, const String& value, int32_t swatch = -1) {
  int y = SETTINGS_ROW_TOP + index * SETTINGS_ROW_PITCH;
  int h = SETTINGS_ROW_PITCH - 5;
  uint16_t fill = index & 1 ? UI_PANEL_ALT : PANEL;
  M5.Display.fillRoundRect(8, y, 304, h, 8, fill);
  M5.Display.drawRoundRect(8, y, 304, h, 8, UI_BORDER);
  useUIMediumFont();
  M5.Display.setTextColor(TFT_WHITE, fill);
  M5.Display.setTextDatum(middle_left);
  M5.Display.drawString(label, 18, y + h / 2);
  if (swatch >= 0) {
    M5.Display.fillRoundRect(248, y + 8, 50, h - 16, 5,
      M5.Display.color565((swatch >> 16) & 255, (swatch >> 8) & 255, swatch & 255));
  } else if (value.length()) {
    M5.Display.setTextColor(0x9EFF, fill);
    M5.Display.setTextDatum(middle_right);
    M5.Display.drawString(value, 302, y + h / 2);
  }
  M5.Display.setTextDatum(top_left);
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
  prefs.putString("deviceName", deviceName);
  prefs.putBool("hassOn", hassAssistEnabled);
  prefs.putString("hassUrl", hassAssistBaseUrl);
  prefs.putString("hassToken", hassAssistToken);
  prefs.putString("hassPipe", hassAssistPipeline);
  prefs.putUChar("hassVol", hassAssistVolume);
  prefs.putBool("hassWake", hassAssistWakeWordEnabled);
  prefs.putUChar("hassMode", hassAssistVoiceMode);
  prefs.putString("icalUrl", calendarIcalUrl);
  prefs.putUChar("emoMode", emotionReminderMode);
  prefs.putUShort("emoInterval", emotionReminderIntervalMinutes);
  prefs.putUShort("emoWinStart", emotionReminderWindowStart);
  prefs.putUShort("emoWinEnd", emotionReminderWindowEnd);
  for (int i = 0; i < 3; ++i) prefs.putUShort(("emoTime" + String(i)).c_str(), emotionReminderTimes[i]);
  prefs.putBool("emoVib", emotionReminderVibration);
  prefs.putBool("emoSound", emotionReminderSound);
  prefs.putUChar("emoDur", emotionReminderDurationSeconds);
  prefs.putUChar("emoSnd", emotionReminderSoundChoice);
  prefs.putUChar("emoVol", emotionReminderVolume);
  prefs.putString("emoApi", emotionApiBase);
  prefs.putString("emoIdent", emotionApiIdentity);
  prefs.putString("emoUser", emotionApiUserId);
  prefs.putString("emoToken", emotionApiToken);
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
  meditationPresetMinutes[0] = constrain((int)prefs.getUShort("medP1", 5), 1, 60);
  meditationPresetMinutes[1] = constrain((int)prefs.getUShort("medP2", 15), 1, 60);
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
  deviceName = prefs.getString("deviceName", "Space Clock");
  deviceName.trim();
  if (!deviceName.length()) deviceName = "Space Clock";
  hassAssistEnabled = prefs.getBool("hassOn", false);
  hassAssistBaseUrl = prefs.getString("hassUrl", "");
  hassAssistBaseUrl.trim();
  while (hassAssistBaseUrl.endsWith("/")) hassAssistBaseUrl.remove(hassAssistBaseUrl.length() - 1);
  hassAssistToken = prefs.getString("hassToken", "");
  hassAssistPipeline = prefs.getString("hassPipe", "");
  hassAssistPipeline.trim();
  hassAssistVolume = constrain((int)prefs.getUChar("hassVol", 70), 5, 100);
  hassAssistWakeWordEnabled = prefs.getBool("hassWake", false);
  hassAssistVoiceMode = constrain((int)prefs.getUChar("hassMode", hassAssistWakeWordEnabled ? HASS_MODE_WAKE : HASS_MODE_TAP), 0, 2);
  hassAssistWakeWordEnabled = hassAssistVoiceMode == HASS_MODE_WAKE;
  calendarIcalUrl = prefs.getString("icalUrl", "");
  hassAssistState = hassAssistEnabled ? HassAssistState::Disconnected : HassAssistState::Disabled;
  emotionReminderMode = constrain((int)prefs.getUChar("emoMode", 0), 0, 2);
  emotionReminderIntervalMinutes = prefs.getUShort("emoInterval", 60);
  const uint16_t validEmotionIntervals[] = {10, 15, 30, 60, 120, 180, 240};
  bool emotionIntervalValid = false;
  for (uint16_t interval : validEmotionIntervals) if (emotionReminderIntervalMinutes == interval) emotionIntervalValid = true;
  if (!emotionIntervalValid) emotionReminderIntervalMinutes = 60;
  emotionReminderWindowStart = min<uint16_t>(prefs.getUShort("emoWinStart", 300), 1439);
  emotionReminderWindowEnd = min<uint16_t>(prefs.getUShort("emoWinEnd", 1410), 1439);
  if (emotionReminderWindowEnd < emotionReminderWindowStart) emotionReminderWindowEnd = emotionReminderWindowStart;
  for (int i = 0; i < 3; ++i) emotionReminderTimes[i] = prefs.getUShort(("emoTime" + String(i)).c_str(), i == 0 ? 600 : (i == 1 ? 900 : 0xFFFF));
  emotionReminderVibration = prefs.getBool("emoVib", true);
  emotionReminderSound = prefs.getBool("emoSound", false);
  emotionReminderDurationSeconds = prefs.getUChar("emoDur", 30);
  if (emotionReminderDurationSeconds != 10 && emotionReminderDurationSeconds != 30 && emotionReminderDurationSeconds != 60 && emotionReminderDurationSeconds != 120) emotionReminderDurationSeconds = 30;
  emotionReminderSoundChoice = constrain((int)prefs.getUChar("emoSnd", 1), 0, 3);
  emotionReminderVolume = constrain((int)prefs.getUChar("emoVol", 60), 5, 100);
  emotionApiBase = prefs.getString("emoApi", "https://emotion.theoakhouse.org");
  emotionApiIdentity = prefs.getString("emoIdent", "");
  emotionApiUserId = prefs.getString("emoUser", "");
  emotionApiToken = prefs.getString("emoToken", "");
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

String networkHostname() {
  String prefix;
  prefix.reserve(22);
  for (size_t i = 0; i < deviceName.length() && prefix.length() < 22; ++i) {
    char c = deviceName[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (valid) prefix += c;
    else if (prefix.length() && prefix[prefix.length() - 1] != '-') prefix += '-';
  }
  while (prefix.endsWith("-")) prefix.remove(prefix.length() - 1);
  if (!prefix.length()) prefix = "space-clock";
  char uniqueId[9]; snprintf(uniqueId, sizeof(uniqueId), "%08lx", (unsigned long)(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  return prefix + "-" + uniqueId;
}

void applyNetworkHostname() {
  String hostname = networkHostname();
  WiFi.setHostname(hostname.c_str());
}

void drawClock(bool full);
void runWifiPortal(bool automatic = false);
void drawAstronaut();
bool verifyEmotionApiConnection(bool force = false);
void showEmotionRecords(bool refresh = true);
void showEmotionSettings();

bool hasSavedWifiProfiles() {
  for (int slot = 0; slot < SAVED_WIFI_COUNT; ++slot) {
    if (savedWifiSsids[slot].length()) return true;
  }
  return false;
}

void prepareSavedWifiCandidates(int found) {
  wifiRecoveryCount = 0;
  for (int slot = 0; slot < SAVED_WIFI_COUNT; ++slot) {
    if (!savedWifiSsids[slot].length()) continue;
    int bestRssi = -1000;
    if (found >= 0) {
      for (int network = 0; network < found; ++network) {
        if (WiFi.SSID(network) == savedWifiSsids[slot]) bestRssi = max(bestRssi, (int)WiFi.RSSI(network));
      }
    }
    // Keep configured networks even when a scan misses them (hidden SSIDs,
    // scan failures, or an access point that is still starting up).
    wifiRecoverySlots[wifiRecoveryCount] = slot;
    wifiRecoveryRssi[wifiRecoveryCount++] = bestRssi;
  }

  for (int i = 0; i < wifiRecoveryCount; ++i) {
    for (int j = i + 1; j < wifiRecoveryCount; ++j) {
      if (wifiRecoveryRssi[j] > wifiRecoveryRssi[i]) {
        int slot = wifiRecoverySlots[i]; wifiRecoverySlots[i] = wifiRecoverySlots[j]; wifiRecoverySlots[j] = slot;
        int rssi = wifiRecoveryRssi[i]; wifiRecoveryRssi[i] = wifiRecoveryRssi[j]; wifiRecoveryRssi[j] = rssi;
      }
    }
  }
}

void beginSavedWifiAttempt(uint32_t nowMs) {
  int slot = wifiRecoverySlots[wifiRecoveryIndex];
  // The ten user-managed profiles already live in Preferences. Do not let a
  // fallback attempt overwrite the legacy ESP32 Wi-Fi credentials used by
  // WiFi.begin() with no arguments.
  WiFi.persistent(false);
  WiFi.disconnect(false, false);
  WiFi.begin(savedWifiSsids[slot].c_str(), savedWifiPasswords[slot].c_str());
  WiFi.persistent(true);
  wifiRecoveryPhase = WifiRecoveryPhase::ConnectingProfiles;
  wifiRecoveryPhaseStartedAt = nowMs;
}

void beginBuildWifiFallback(uint32_t nowMs) {
  wifiDefaultFallbackAttempted = true;
  WiFi.persistent(false);
  WiFi.disconnect(false, false);
  WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
  WiFi.persistent(true);
  wifiRecoveryPhase = WifiRecoveryPhase::ConnectingFallback;
  wifiRecoveryPhaseStartedAt = nowMs;
}

void beginLegacyWifiRetry(uint32_t nowMs) {
  WiFi.persistent(false);
  WiFi.disconnect(false, false);
  WiFi.begin();
  WiFi.persistent(true);
  wifiRecoveryPhase = WifiRecoveryPhase::Primary;
  wifiRecoveryPhaseStartedAt = nowMs;
}

void startWifiProfileScan(uint32_t nowMs) {
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  wifiRecoveryPhase = WifiRecoveryPhase::ScanningProfiles;
  wifiRecoveryPhaseStartedAt = nowMs;
}

void maintainSavedWifi(uint32_t nowMs) {
  static wl_status_t lastLoggedStatus = (wl_status_t)255;
  static uint8_t lastLoggedPhase = 255;
  if (WiFi.status() != lastLoggedStatus || (uint8_t)wifiRecoveryPhase != lastLoggedPhase) {
    lastLoggedStatus = WiFi.status();
    lastLoggedPhase = (uint8_t)wifiRecoveryPhase;
    Serial.printf("[wifi] status=%d phase=%u ssid='%s' saved-profiles=%d ip=%s\n", (int)lastLoggedStatus, lastLoggedPhase,
                  WiFi.SSID().c_str(), hasSavedWifiProfiles() ? 1 : 0, WiFi.localIP().toString().c_str());
  }
  if (WiFi.status() == WL_CONNECTED) {
    bool justConnected = !wifiWasConnected;
    wifiWasConnected = true;
    wifiDefaultFallbackAttempted = false;
    wifiRecoveryPhase = WifiRecoveryPhase::Primary;
    wifiRecoveryPhaseStartedAt = nowMs;
    if (!settingsServerReady) setupSettingsServer();
    if (justConnected) {
      syncTime();
      if (screenNow == Screen::Clock) { drawClock(true); drawAstronaut(); }
    }
    return;
  }
  wifiWasConnected = false;

  // A brand-new Core2 has no Wi-Fi anywhere (no web profiles, no build-time
  // default, nothing in the ESP32 Wi-Fi store). Open the setup hotspot by
  // itself instead of waiting forever; retry every 10 minutes if skipped.
  static uint32_t lastAutoPortalAt = 0;
  if (!hasSavedWifiProfiles() && !strlen(DEFAULT_WIFI_SSID) && !WiFi.SSID().length()
      && nowMs > 15000UL && (!lastAutoPortalAt || nowMs - lastAutoPortalAt > 600000UL)
      && alarmActive < 0 && screenNow == Screen::Clock) {
    Serial.println("[wifi] no saved networks; opening setup hotspot " SPACE_CLOCK_WIFI_AP);
    runWifiPortal(true);
    lastAutoPortalAt = millis();
    return;
  }

  if (wifiRecoveryPhase == WifiRecoveryPhase::Primary) {
    if (nowMs - wifiRecoveryPhaseStartedAt < WIFI_PRIMARY_TIMEOUT_MS) return;
    if (!hasSavedWifiProfiles()) {
      if (strlen(DEFAULT_WIFI_SSID) && !wifiDefaultFallbackAttempted) {
        beginBuildWifiFallback(nowMs);
      } else {
        // Retry the credentials saved by the ESP32 Wi-Fi stack. This is the
        // fast path used by earlier firmware and remains active even when
        // the newer multi-profile list is empty.
        WiFi.reconnect();
        wifiRecoveryPhaseStartedAt = nowMs;
      }
      return;
    }
    // Let the original association finish before scanning; an active scan can
    // interrupt a connection that is merely taking longer than expected.
    startWifiProfileScan(nowMs);
    return;
  }

  if (wifiRecoveryPhase == WifiRecoveryPhase::ScanningProfiles) {
    int found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING && nowMs - wifiRecoveryPhaseStartedAt < WIFI_SCAN_TIMEOUT_MS) return;
    if (found == WIFI_SCAN_RUNNING) { WiFi.scanDelete(); found = -1; }
    prepareSavedWifiCandidates(found);
    WiFi.scanDelete();
    if (wifiRecoveryCount == 0) {
      wifiRecoveryPhase = WifiRecoveryPhase::Primary;
      wifiRecoveryPhaseStartedAt = nowMs;
      WiFi.reconnect();
      return;
    }
    wifiRecoveryIndex = 0;
    beginSavedWifiAttempt(nowMs);
    return;
  }

  if (wifiRecoveryPhase == WifiRecoveryPhase::ConnectingProfiles &&
      nowMs - wifiRecoveryPhaseStartedAt >= WIFI_PROFILE_TIMEOUT_MS) {
    ++wifiRecoveryIndex;
    if (wifiRecoveryIndex < wifiRecoveryCount) {
      beginSavedWifiAttempt(nowMs);
    } else if (strlen(DEFAULT_WIFI_SSID) && !wifiDefaultFallbackAttempted) {
      beginBuildWifiFallback(nowMs);
    } else {
      // Re-arm the legacy saved connection, then give it a full window before
      // beginning another profile scan cycle.
      beginLegacyWifiRetry(nowMs);
    }
  }

  if (wifiRecoveryPhase == WifiRecoveryPhase::ConnectingFallback &&
      nowMs - wifiRecoveryPhaseStartedAt >= WIFI_PROFILE_TIMEOUT_MS) {
    wifiDefaultFallbackAttempted = false;
    beginLegacyWifiRetry(nowMs);
  }
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

// A new Core2's RTC holds garbage (e.g. weekDay 0xFF), which crashed the clock
// face on first boot. Reset any impossible value to the firmware build date;
// NTP corrects it once Wi-Fi connects.
void sanitizeRtcOnBoot() {
  m5::rtc_datetime_t dt;
  M5.Rtc.getDateTime(&dt);
  bool bad = dt.date.year < 2024 || dt.date.year > 2099 || dt.date.month < 1 || dt.date.month > 12
    || dt.date.date < 1 || dt.date.date > 31 || (uint8_t)dt.date.weekDay > 6
    || dt.time.hours > 23 || dt.time.minutes > 59 || dt.time.seconds > 59;
  if (!bad) return;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0}; int day = 1, year = 2026;
  sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
  int month = (int)((strstr(months, mon) - months) / 3) + 1;
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day; t.tm_hour = 12;
  mktime(&t);
  dt.date.year = year; dt.date.month = month; dt.date.date = day; dt.date.weekDay = t.tm_wday;
  dt.time.hours = 12; dt.time.minutes = 0; dt.time.seconds = 0;
  M5.Rtc.setDateTime(&dt);
  Serial.printf("[rtc] invalid RTC reset to build date %04d-%02d-%02d\n", year, month, day);
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
  if (screenNow == Screen::NightLight) {
    M5.Display.setBrightness((uint8_t)max(1, (screenNightBrightness ? screenNightBrightness : nightLightBrightness) * 255 / 100));
    return;
  }
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

// Device name in the status bar, centred in the gap between the IP address
// and the battery gauge so several Core2 units can be told apart at a glance.
template <typename Gfx>
void drawStatusDeviceName(Gfx& gfx, int ipRight, uint16_t color) {
  int left = ipRight + 8, right = 236;
  if (right - left < 24 || !deviceName.length()) return;
  String name = deviceName;
  while (name.length() > 1 && gfx.textWidth(name) > right - left) {
    int cut = name.length() - 1;
    while (cut > 0 && ((uint8_t)name[cut] & 0xC0) == 0x80) --cut;  // UTF-8 boundary
    name = name.substring(0, cut);
    if (gfx.textWidth(name + "…") <= right - left) { name += "…"; break; }
  }
  gfx.setTextDatum(top_center);
  gfx.setTextColor(color);
  gfx.drawString(name, (left + right) / 2, 7);
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
  drawStatusDeviceName(M5.Display, 5 + M5.Display.textWidth(label),
                       clockFace == ClockFace::Matrix ? matrixColor(100) : (uint16_t)0xBDF7);
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

void drawPairedGearNavigationIcon(int cx, int cy, uint16_t color) {
  M5.Display.drawCircle(cx, cy, 6, color);
  M5.Display.fillCircle(cx, cy, 2, color);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4.0f;
    int x1 = cx + lroundf(cosf(a) * 6), y1 = cy + lroundf(sinf(a) * 6);
    int x2 = cx + lroundf(cosf(a) * 8), y2 = cy + lroundf(sinf(a) * 8);
    M5.Display.drawLine(x1, y1, x2, y2, color);
  }
}

void drawClockNavigationIcons() {
  if (clockFace != ClockFace::Matrix) M5.Display.fillRect(0, 215, 320, 25, BG);
  // Each virtual button shows short press / long press actions from left to
  // right: Companion / Emotion, Meditation / HASS, Settings / Night light.
  M5.Display.drawPng(nav_companion_png, nav_companion_png_len, 29, 218);
  M5.Display.drawPng(nav_emotion_png, nav_emotion_png_len, 57, 218);
  M5.Display.drawPng(nav_meditation_png, nav_meditation_png_len, 136, 218);
  M5.Display.drawPng(nav_hass_png, nav_hass_png_len, 164, 218);
  M5.Display.drawPng(nav_settings_png, nav_settings_png_len, 243, 218);
  M5.Display.drawPng(nav_nightlight_png, nav_nightlight_png_len, 271, 218);
}

void resetMatrixRain() {
  const int scale = matrixGlyphScale == 1 ? 1 : 2;
  const int columnPitch = 16 * scale;
  const int rowPitch = 18 * scale;
  const int usableColumns = min((int)MATRIX_COLUMNS, 320 / columnPitch);
  const int rows = min((int)MATRIX_MAX_ROWS, 240 / rowPitch + 2);
  for (int i = 0; i < MATRIX_COLUMNS; ++i) {
    matrixActive[i] = i < usableColumns && (esp_random() % 100) < matrixRainDensity;
    matrixHead[i] = -((float)(esp_random() % rows));
    matrixSpeed[i] = 1.0f + (esp_random() % 100) / 100.0f * 2.5f;
    matrixLength[i] = 5 + (esp_random() % max(2, rows - 4));
    for (int row = 0; row < MATRIX_MAX_ROWS; ++row) {
      matrixGlyphs[i][row] = esp_random() % MATRIX_GLYPH_COUNT;
      matrixGlint[i][row] = 0;
    }
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

uint16_t matrixTrailColor(float trail, uint8_t length) {
  if (trail < 0.5f) return M5.Display.color565(245, 255, 250);
  float position = min(1.0f, trail / max(1, (int)length - 1));
  uint8_t themeR = (matrixRainColor >> 16) & 255;
  uint8_t themeG = (matrixRainColor >> 8) & 255;
  uint8_t themeB = matrixRainColor & 255;
  if (position <= 0.28f) {
    // The first part of the tail smoothly changes from the white falling tip
    // to the selected Matrix phosphor colour.
    float blend = position / 0.28f;
    return M5.Display.color565(
      245 - (245 - themeR) * blend,
      255 - (255 - themeG) * blend,
      250 - (250 - themeB) * blend);
  }
  // The remainder keeps the theme hue while fading gently to transparent.
  float fade = 1.0f - (position - 0.28f) / 0.72f;
  fade = fade * fade * (3.0f - 2.0f * fade);
  return matrixColor((uint8_t)(fade * 100.0f));
}

void addMatrixGlintBloom(M5Canvas& canvas, int cx, int cy, uint8_t strength) {
  const int radius = matrixGlyphScale == 1 ? 4 : 6;
  const int radiusSquared = radius * radius;
  const uint8_t themeR = (matrixRainColor >> 16) & 255;
  const uint8_t themeG = (matrixRainColor >> 8) & 255;
  const uint8_t themeB = matrixRainColor & 255;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      int distanceSquared = dx * dx + dy * dy;
      if (distanceSquared > radiusSquared) continue;
      int px = cx + dx, py = cy + dy;
      if (px < 0 || px >= 320 || py < 0 || py >= 240) continue;
      uint16_t base = canvas.readPixel(px, py);
      int falloff = (radiusSquared + 1 - distanceSquared) * strength / (radiusSquared + 1);
      uint8_t r = min(255, ((base >> 11) & 0x1F) * 255 / 31 + themeR * falloff / 100);
      uint8_t g = min(255, ((base >> 5) & 0x3F) * 255 / 63 + themeG * falloff / 100);
      uint8_t b = min(255, (base & 0x1F) * 255 / 31 + themeB * falloff / 100);
      canvas.drawPixel(px, py, M5.Display.color565(r, g, b));
    }
  }
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
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", shownHour, dt.time.minutes, dt.time.seconds);
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
  String ipLabel = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi offline";
  canvas.drawString(ipLabel, 5, 7);
  drawStatusDeviceName(canvas, 5 + canvas.textWidth(ipLabel), theme);
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

void drawMatrixPairedGearNavigationIcon(M5Canvas& canvas, int cx, int cy) {
  uint16_t theme = matrixColor(100);
  canvas.drawCircle(cx, cy, 6, theme);
  canvas.fillCircle(cx, cy, 2, theme);
  for (int i = 0; i < 8; ++i) {
    float a = i * PI / 4.0f;
    int x1 = cx + lroundf(cosf(a) * 6), y1 = cy + lroundf(sinf(a) * 6);
    int x2 = cx + lroundf(cosf(a) * 8), y2 = cy + lroundf(sinf(a) * 8);
    canvas.drawLine(x1, y1, x2, y2, theme);
  }
}

void drawMatrixNavigationIcons(M5Canvas& canvas) {
  canvas.drawPng(nav_companion_png, nav_companion_png_len, 29, 218);
  canvas.drawPng(nav_emotion_png, nav_emotion_png_len, 57, 218);
  canvas.drawPng(nav_meditation_png, nav_meditation_png_len, 136, 218);
  canvas.drawPng(nav_hass_png, nav_hass_png_len, 164, 218);
  canvas.drawPng(nav_settings_png, nav_settings_png_len, 243, 218);
  canvas.drawPng(nav_nightlight_png, nav_nightlight_png_len, 271, 218);
}

void drawMatrixRainFrame(uint32_t nowMs) {
  if (clockFace != ClockFace::Matrix || screenNow != Screen::Clock || alarmActive >= 0) return;
  if (!matrixCanvasReady) {
    // Do not silently draw into a failed sprite. Keep navigation available and
    // display a diagnostic using a built-in font that needs no external assets.
    if (matrixMemoryErrorDrawn) return;
    matrixMemoryErrorDrawn = true;
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("Matrix display memory unavailable", 10, 95);
    M5.Display.drawString("Restart device; settings are preserved", 10, 120);
    drawClockNavigationIcons();
    return;
  }
  // 20 fps: smooth enough to read as falling rain while leaving time for the
  // SPI push of the full 320x240 frame and the rest of loop().
  uint32_t frameInterval = 50;
  if (nowMs - lastMatrixFrame < frameInterval) return;
  float dt = lastMatrixFrame ? (nowMs - lastMatrixFrame) / 1000.0f : frameInterval / 1000.0f;
  if (dt > 0.25f) dt = 0.25f;
  lastMatrixFrame = nowMs;
  const int scale = matrixGlyphScale == 1 ? 1 : 2;
  const int columnPitch = 16 * scale;
  const int rowPitch = 18 * scale;
  const int columns = min((int)MATRIX_COLUMNS, 320 / columnPitch);
  const int rows = min((int)MATRIX_MAX_ROWS, 240 / rowPitch + 2);
  matrixCanvas.fillSprite(TFT_BLACK);
  // Keep the font's 12 px glyphs inside separate 16×18 px cells at 1x;
  // the extra black gutters prevent adjacent symbols from joining into lines.
  matrixCanvas.setTextDatum(middle_center);
  matrixCanvas.setFont(&SourceHanSansTC_UI8pt8b);
  matrixCanvas.setTextSize(matrixGlyphScale);
  for (int i = 0; i < MATRIX_COLUMNS; ++i) {
    if (i >= columns) { matrixActive[i] = false; continue; }
    if (!matrixActive[i]) {
      // Reintroduce inactive lanes over time so lower density never goes blank.
      if ((esp_random() % 1000) < matrixRainDensity * 3) {
        matrixActive[i] = true;
        matrixHead[i] = -((float)(esp_random() % max(1, rows / 2)));
      } else continue;
    }
    int cellX = i * columnPitch;
    int previousHead = (int)matrixHead[i];
    matrixHead[i] += matrixSpeed[i] * (0.7f + matrixRainSpeed * 0.095f) * dt;
    int head = (int)matrixHead[i];
    if (head != previousHead && head >= 0) {
      matrixGlyphs[i][head % MATRIX_MAX_ROWS] = esp_random() % MATRIX_GLYPH_COUNT;
    }
    // Occasional glyph mutations (rate independent of frame rate).
    if ((esp_random() % 1000) < (uint32_t)(dt * 300.0f)) matrixGlyphs[i][esp_random() % rows] = esp_random() % MATRIX_GLYPH_COUNT;
    for (int row = 0; row < rows; ++row) if (matrixGlint[i][row]) --matrixGlint[i][row];

    for (int trail = 0; trail < matrixLength[i]; ++trail) {
      int row = head - trail;
      if (row < 0 || row >= rows) continue;
      // A few glyphs in the middle of a trail briefly brighten and leave a
      // small additive phosphor halo.
      if (trail > 1 && trail < matrixLength[i] - 1 && !matrixGlint[i][row] && (esp_random() % 1200) == 0) {
        matrixGlint[i][row] = 3 + esp_random() % 4;
      }
      // Use the head's sub-cell position so the tail fades continuously
      // instead of stepping one whole cell at a time.
      float frac = matrixHead[i] - floorf(matrixHead[i]);
      matrixCanvas.setTextColor(matrixGlint[i][row]
        ? M5.Display.color565(220, 255, 230)
        : matrixTrailColor(trail == 0 ? 0.0f : trail - 0.5f + frac, matrixLength[i]));
      matrixCanvas.drawString(MATRIX_GLYPH_SET[matrixGlyphs[i][row]], cellX + columnPitch / 2,
                              row * rowPitch + rowPitch / 2);
    }
    if (matrixHead[i] - matrixLength[i] > rows) {
      matrixHead[i] = -((float)(esp_random() % max(1, rows / 2)));
      matrixSpeed[i] = 1.0f + (esp_random() % 100) / 100.0f * 2.5f;
      matrixLength[i] = 5 + (esp_random() % max(2, rows - 4));
      // Always leave a few seed columns running; the others follow the density setting.
      matrixActive[i] = i < 3 || (esp_random() % 100) < matrixRainDensity;
    }
  }
  // Add the rare glows after the text is composed, then redraw the crisp symbol.
  for (int i = 0; i < MATRIX_COLUMNS; ++i) {
    if (!matrixActive[i] || i >= columns) continue;
    int cellX = i * columnPitch;
    for (int row = 0; row < rows; ++row) {
      if (!matrixGlint[i][row]) continue;
      addMatrixGlintBloom(matrixCanvas, cellX + columnPitch / 2, row * rowPitch + rowPitch / 2,
                          matrixGlint[i][row] * 9);
      matrixCanvas.setTextColor(M5.Display.color565(230, 255, 238));
      matrixCanvas.drawString(MATRIX_GLYPH_SET[matrixGlyphs[i][row]], cellX + columnPitch / 2,
                              row * rowPitch + rowPitch / 2);
    }
  }
  matrixCanvas.setTextDatum(top_left);
  drawMatrixClockPanel(matrixCanvas);
  drawMatrixStatus(matrixCanvas);
  drawMatrixNavigationIcons(matrixCanvas);
  matrixCanvas.pushSprite(0, 0);
}

void drawMeditationNavigationIcons() {
  M5.Display.fillRect(0, 215, 320, 25, BG);
  M5.Display.drawPng(nav_companion_png, nav_companion_png_len, 43, 218);
  M5.Display.drawPng(action_clock_png, action_clock_png_len, 150, 218);
  M5.Display.drawPng(nav_settings_png, nav_settings_png_len, 257, 218);
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
  if (full) {
    matrixMemoryErrorDrawn = false;
    drawClockStatic();
  }
  m5::rtc_datetime_t dt;
  getClockDateTime(&dt);
  char buf[24];
  if (clockFace == ClockFace::Space) {
    // The clock owns x >= 116; the astronaut owns x <= 112. Keeping these
    // regions disjoint prevents the minute repaint from cutting the sprite.
    int shownHour = use24HourTime ? dt.time.hours : (dt.time.hours % 12 ? dt.time.hours % 12 : 12);
    uint16_t fg = alarmActive >= 0 ? TFT_RED : FG;
    if (spaceTimeCanvasReady) {
      // Compose time, seconds and date off-screen, then push in one go.
      M5Canvas& c = spaceTimeCanvas;
      c.fillSprite(BG);
      c.setTextColor(fg, BG);
      c.setTextDatum(top_left);
      c.setFont(&SourceHanSansTC_Medium28pt7b); c.setTextSize(1);
      snprintf(buf, sizeof(buf), "%02d:%02d", shownHour, dt.time.minutes);
      c.drawString(buf, 4, 0);
      int secondsX = 4 + c.textWidth(buf) + 4;
      c.setFont(&SourceHanSansTC_UI14pt8b);
      c.setTextColor(0x9CF3, BG);
      snprintf(buf, sizeof(buf), "%02d", dt.time.seconds);
      c.drawString(buf, secondsX, 20);
      c.setTextColor(fg, BG);
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
      c.drawString(buf, 4, 58);
      c.pushSprite(116, 116);
    } else {
      M5.Display.setTextColor(fg, BG);
      M5.Display.setTextDatum(top_left);
      useUILargeFont();
      snprintf(buf, sizeof(buf), "%02d:%02d:%02d", shownHour, dt.time.minutes, dt.time.seconds);
      M5.Display.drawString(buf, 120, 116);
      useUIMediumFont();
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
      M5.Display.drawString(buf, 120, 174);
    }
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
      M5.Display.drawString(days[(uint8_t)dt.date.weekDay % 7], 160, 41);
      M5.Display.fillRoundRect(3, 151, 22, 27, 5, use24HourTime ? TFT_BLACK : 0x2124);
      M5.Display.setTextDatum(middle_center); useUIFont(1);
      if (!use24HourTime) M5.Display.drawString(dt.time.hours >= 12 ? "PM" : "AM", 14, 164);
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
      M5.Display.fillRect(0, 181, 320, 34, TFT_BLACK);
      M5.Display.setTextDatum(middle_center); useUIMediumFont(); M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      M5.Display.drawString(buf, 140, 197);
      shownDay = dt.date.date;
    }
    // Seconds tick in their own small buffer to the right of the date.
    snprintf(buf, sizeof(buf), "%02d", dt.time.seconds);
    if (clockSecondsCanvasReady) {
      clockSecondsCanvas.fillSprite(TFT_BLACK);
      clockSecondsCanvas.fillRoundRect(8, 1, 52, 30, 7, 0x2124);
      clockSecondsCanvas.setFont(&SourceHanSansTC_UI14pt8b); clockSecondsCanvas.setTextSize(1);
      clockSecondsCanvas.setTextDatum(middle_center);
      clockSecondsCanvas.setTextColor(TFT_WHITE, 0x2124);
      clockSecondsCanvas.drawString(buf, 34, 16);
      clockSecondsCanvas.pushSprite(248, 182);
    } else {
      M5.Display.setTextDatum(middle_center); useUIMediumFont(); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Display.drawString(buf, 280, 197);
    }
  }
}

void drawAlarmCanvasBottomBar(M5Canvas& canvas) {
  canvas.fillRect(0, 214, 320, 26, TFT_BLACK);
  canvas.drawFastHLine(0, 214, 320, 0x2945);
  canvas.setFont(&SourceHanSansTC_UI8pt8b);
  canvas.setTextSize(1);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas.drawString("貪睡 5 分鐘", 160, 228);
}

void drawAlarmGear(M5Canvas& canvas, int cx, int cy, int rootRadius, int toothRadius,
                   int teeth, float rotation, int hubRadius, bool knob) {
  canvas.fillCircle(cx, cy, rootRadius - 2, 0x0841);
  float step = PI / teeth;
  int previousX = cx + lroundf(cosf(rotation) * toothRadius);
  int previousY = cy + lroundf(sinf(rotation) * toothRadius);
  for (int i = 1; i <= teeth * 2; ++i) {
    float angle = rotation + step * i;
    int radius = (i & 1) ? rootRadius : toothRadius;
    int x = cx + lroundf(cosf(angle) * radius);
    int y = cy + lroundf(sinf(angle) * radius);
    canvas.drawLine(previousX, previousY, x, y, TFT_WHITE);
    previousX = x; previousY = y;
  }
  canvas.fillCircle(cx, cy, hubRadius, 0xBDF7);
  canvas.drawCircle(cx, cy, hubRadius, TFT_WHITE);
  canvas.drawCircle(cx, cy, hubRadius - 1, 0x7BEF);
  if (knob) {
    float knobAngle = -2.15f + rotation;
    int knobX = cx + lroundf(cosf(knobAngle) * (hubRadius - 11));
    int knobY = cy + lroundf(sinf(knobAngle) * (hubRadius - 11));
    canvas.fillCircle(knobX, knobY, 8, TFT_WHITE);
    canvas.drawCircle(knobX, knobY, 9, 0xCE79);
  }
}

void drawFlipAlarmChallenge() {
  if (!matrixCanvasReady) {
    M5.Display.fillScreen(TFT_BLACK);
    useUIFont(1); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK); M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("按住白點，順時鐘轉一圈", 160, 110);
    drawBottomBar("", "貪睡 5 分鐘", "");
    return;
  }
  matrixCanvas.fillSprite(TFT_BLACK);
  matrixCanvas.setFont(&SourceHanSansTC_UI8pt8b);
  matrixCanvas.setTextSize(1);
  matrixCanvas.setTextDatum(middle_center);
  matrixCanvas.setTextColor(0xFBAE, TFT_BLACK);
  matrixCanvas.drawString("按住白點，順時鐘轉一圈", 160, 13);

  float rotation = alarmGearProgress;
  drawAlarmGear(matrixCanvas, 88, 114, 54, 63, 18, rotation, 40, true);
  drawAlarmGear(matrixCanvas, 232, 75, 43, 51, 16, -rotation * 1.20f, 31, false);
  drawAlarmGear(matrixCanvas, 195, 165, 25, 31, 12, -rotation * 1.85f, 17, false);

  uint16_t progressColor = 0x2E5F;
  int progressSegments = constrain((int)(alarmGearProgress / (2.0f * PI) * 44.0f), 0, 44);
  for (int i = 0; i < progressSegments; ++i) {
    float a = -PI / 2.0f + i * (2.0f * PI / 44.0f);
    int x1 = 88 + lroundf(cosf(a) * 68), y1 = 114 + lroundf(sinf(a) * 68);
    int x2 = 88 + lroundf(cosf(a) * 72), y2 = 114 + lroundf(sinf(a) * 72);
    matrixCanvas.drawLine(x1, y1, x2, y2, progressColor);
  }
  drawAlarmCanvasBottomBar(matrixCanvas);
  matrixCanvas.pushSprite(0, 0);
}

void drawMatrixAlarmChallenge(uint32_t nowMs) {
  if (!matrixCanvasReady) {
    M5.Display.fillScreen(TFT_BLACK);
    useUIFont(1); M5.Display.setTextColor(TFT_CYAN, TFT_BLACK); M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("長按藍色藥丸直到消失", 160, 110);
    drawBottomBar("", "貪睡 5 分鐘", "");
    return;
  }
  matrixCanvas.fillSprite(TFT_BLACK);
  matrixCanvas.setFont(&SourceHanSansTC_UI8pt8b);
  matrixCanvas.setTextSize(1);
  matrixCanvas.setTextDatum(middle_center);
  const int rainTick = nowMs / 95UL;
  for (int column = 0; column < 20; ++column) {
    int x = 6 + column * 16;
    int head = ((rainTick * (1 + column % 3) + column * 29) % 280) - 28;
    for (int tail = 0; tail < 6; ++tail) {
      int y = head - tail * 14;
      if (y < 0 || y >= 214) continue;
      uint8_t strength = max(8, 65 - tail * 10);
      matrixCanvas.setTextColor(matrixColor(strength), TFT_BLACK);
      int glyph = (column * 13 + rainTick + tail * 7) % MATRIX_GLYPH_COUNT;
      matrixCanvas.drawString(MATRIX_GLYPH_SET[glyph], x, y);
    }
  }
  matrixCanvas.fillRoundRect(30, 9, 260, 25, 8, TFT_BLACK);
  matrixCanvas.drawRoundRect(30, 9, 260, 25, 8, matrixColor(45));
  matrixCanvas.setTextColor(matrixColor(100), TFT_BLACK);
  matrixCanvas.drawString("長按藍色藥丸直到消失", 160, 22);

  float progress = alarmPillHolding
    ? constrain((nowMs - alarmPillHoldStarted) / 2400.0f, 0.0f, 1.0f) : 0.0f;
  int pillWidth = max<int>(0, (int)lroundf(112.0f * (1.0f - progress)));
  int pillHeight = max<int>(0, (int)lroundf(46.0f * (1.0f - progress * 0.72f)));
  if (pillWidth > 4 && pillHeight > 4) {
    int x = 160 - pillWidth / 2, y = 121 - pillHeight / 2;
    uint8_t blue = max(15, 255 - (int)(progress * 170.0f));
    uint16_t pillColor = M5.Display.color565(0, blue / 2, blue);
    uint16_t glowColor = M5.Display.color565(0, blue / 4, blue / 2);
    matrixCanvas.drawRoundRect(x - 4, y - 4, pillWidth + 8, pillHeight + 8, pillHeight / 2 + 4, glowColor);
    matrixCanvas.fillRoundRect(x, y, pillWidth, pillHeight, pillHeight / 2, pillColor);
    matrixCanvas.drawRoundRect(x, y, pillWidth, pillHeight, pillHeight / 2, TFT_CYAN);
    if (pillWidth > 26) matrixCanvas.drawFastVLine(160, y + 3, pillHeight - 6, 0xBFFF);
    int shineWidth = max(2, pillWidth / 3);
    matrixCanvas.drawFastHLine(x + pillWidth / 6, y + max(2, pillHeight / 4), shineWidth, 0xBFFF);
  }
  if (progress > 0.25f) {
    for (int i = 0; i < 12; ++i) {
      float a = i * (2.0f * PI / 12.0f) + progress * 2.0f;
      int radius = 36 + lroundf(progress * 38.0f) + (i % 3) * 4;
      int px = 160 + lroundf(cosf(a) * radius);
      int py = 121 + lroundf(sinf(a) * radius * 0.45f);
      matrixCanvas.fillCircle(px, py, progress > 0.75f ? 1 : 2, i & 1 ? TFT_CYAN : 0x04FF);
    }
  }
  matrixCanvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  matrixCanvas.drawString(alarmPillHolding ? String(lroundf(progress * 100.0f)) + "%" : "按住藥丸", 160, 181);
  drawAlarmCanvasBottomBar(matrixCanvas);
  matrixCanvas.pushSprite(0, 0);
}

void resetAlarmChallengeState() {
  astronautDragging = false;
  alarmGearDragging = false;
  alarmGearProgress = 0.0f;
  alarmGearLastAngle = 0.0f;
  alarmPillHolding = false;
  alarmPillHoldStarted = 0;
  lastAlarmChallengeDraw = 0;
}

void drawAlarmChallenge() {
  if (clockFace == ClockFace::Space) drawAstronaut();
  else if (clockFace == ClockFace::Minimal) drawFlipAlarmChallenge();
  else drawMatrixAlarmChallenge(millis());
}

void drawAstronaut() {
  if (alarmActive >= 0 && clockFace == ClockFace::Space) {
    M5.Display.fillRect(0, 0, 320, 215, BG);
    int sx = 0, sy = satelliteTop ? 0 : 143;
    const Asset& sat = satelliteAssets[satelliteTop ? 0 : 1];
    M5.Display.drawPng(sat.data, sat.size, sx, sy);
    M5.Display.drawPng(cosmonaut_1_png, cosmonaut_1_png_len, astronautX, astronautY);
    M5.Display.setTextColor(TFT_RED, BG);
    useUIFont(1);
    M5.Display.drawCentreString("拖曳太空人到太空艙以關閉鬧鐘", 160, 196, 1);
    drawBottomBar("", "貪睡 5 分鐘", "");
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

uint8_t menuPage = 0;
static const char* const MENU_ROWS[] = {"Wi-Fi & Companion", "Clock faces", "Clock settings", "Alarms", "Meditation settings", "Firmware update"};
// A settings row with a slider (used for the meditation preset times).
static constexpr int SLIDER_LEFT = 24, SLIDER_RIGHT = 296;
void drawSettingsSliderRow(uint8_t index, const String& label, int value, int minValue, int maxValue, const String& unit) {
  int y = SETTINGS_ROW_TOP + index * SETTINGS_ROW_PITCH;
  int h = SETTINGS_ROW_PITCH - 5;
  uint16_t fill = index & 1 ? UI_PANEL_ALT : PANEL;
  M5.Display.fillRoundRect(8, y, 304, h, 8, fill);
  M5.Display.drawRoundRect(8, y, 304, h, 8, UI_BORDER);
  useUIFont(1);
  M5.Display.setTextColor(TFT_WHITE, fill);
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(label, 18, y + 2);
  M5.Display.setTextColor(0x9EFF, fill);
  M5.Display.setTextDatum(top_right);
  M5.Display.drawString(String(value) + " " + unit, 302, y + 2);
  int trackY = y + h - 10;
  int knobX = SLIDER_LEFT + (value - minValue) * (SLIDER_RIGHT - SLIDER_LEFT) / max(1, maxValue - minValue);
  M5.Display.fillRoundRect(SLIDER_LEFT, trackY - 2, SLIDER_RIGHT - SLIDER_LEFT, 5, 2, UI_BORDER);
  M5.Display.fillRoundRect(SLIDER_LEFT, trackY - 2, knobX - SLIDER_LEFT + 1, 5, 2, 0x65DF);
  M5.Display.fillCircle(knobX, trackY, 7, TFT_WHITE);
  M5.Display.drawCircle(knobX, trackY, 7, 0x65DF);
  M5.Display.setTextDatum(top_left);
}

int sliderValueAt(int x, int minValue, int maxValue) {
  x = constrain(x, SLIDER_LEFT, SLIDER_RIGHT);
  return minValue + ((x - SLIDER_LEFT) * (maxValue - minValue) + (SLIDER_RIGHT - SLIDER_LEFT) / 2) / (SLIDER_RIGHT - SLIDER_LEFT);
}

void showMenu() {
  screenNow = Screen::Menu;
  title(menuPage ? "Settings 2/2" : "Settings 1/2");
  for (int row = 0; row < 4; ++row) {
    int i = menuPage * 4 + row;
    if (i < 6) drawSettingsRow(row, MENU_ROWS[i], ">");
  }
  drawBottomBar(menuPage ? "Previous" : "", menuPage ? "" : "Next", "Close");
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

const char* hassAssistStateText() {
  switch (hassAssistState) {
    case HassAssistState::Disabled: return "Not configured";
    case HassAssistState::Disconnected: return "Disconnected";
    case HassAssistState::Connecting: return "Connecting...";
    case HassAssistState::Authenticating: return "Authenticating...";
    case HassAssistState::Ready: return hassAssistVoiceMode == HASS_MODE_HOLD ? "Hold to talk" : "Tap to talk";
    case HassAssistState::Paused: return "Wake listening paused";
    case HassAssistState::WaitingWakeWord: return "Waiting for wake word...";
    case HassAssistState::Starting: return "Starting Assist...";
    case HassAssistState::Listening: return hassAssistWakeSessionActive ? "Listening..." : (hassAssistToggleListen ? "Listening... tap to send" : "Listening... release to send");
    case HassAssistState::Processing: return "Thinking...";
    case HassAssistState::Downloading: return "Loading voice reply...";
    case HassAssistState::Speaking: return "Speaking...";
    case HassAssistState::Error: return "Assist error";
  }
  return "Assist";
}

uint16_t hassAssistStateColor() {
  if (hassAssistState == HassAssistState::Ready) return 0x07E0;
  if (hassAssistState == HassAssistState::Paused) return 0x8410;
  if (hassAssistState == HassAssistState::WaitingWakeWord) return TFT_CYAN;
  if (hassAssistState == HassAssistState::Listening) return TFT_CYAN;
  if (hassAssistState == HassAssistState::Speaking) return 0xFD20;
  if (hassAssistState == HassAssistState::Error || hassAssistState == HassAssistState::Disabled) return 0xF986;
  return 0xFFE0;
}

String hassAssistDrawnSignature;

// Wrap Assist text into at most maxLines lines of maxWidth pixels. Prefer
// breaking right after punctuation (，。！？、；：,.!? and spaces); otherwise
// break between characters. Overflow on the last line ends with "…".
void drawAssistWrappedText(const String& text, int x, int y, int maxWidth, uint8_t maxLines, int lineHeight) {
  auto isBreakAfter = [](const String& ch) {
    static const char* marks[] = {"，", "。", "！", "？", "、", "；", "：", "）", "」", ",", ".", "!", "?", ";", ":", " "};
    for (const char* m : marks) if (ch == m) return true;
    return false;
  };
  auto charLen = [](uint8_t c) { return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1; };
  size_t pos = 0;
  for (uint8_t line = 0; line < maxLines && pos < text.length(); ++line) {
    bool lastLine = line + 1 == maxLines;
    size_t end = pos, lastBreak = 0;
    while (end < text.length()) {
      size_t next = end + charLen((uint8_t)text[end]);
      if (M5.Display.textWidth(text.substring(pos, next)) > maxWidth) break;
      if (isBreakAfter(text.substring(end, next))) lastBreak = next;
      end = next;
    }
    String out;
    if (end >= text.length()) {
      out = text.substring(pos);
    } else if (lastLine) {
      // Trim until the ellipsis fits.
      while (end > pos && M5.Display.textWidth(text.substring(pos, end) + "…") > maxWidth) {
        do { --end; } while (end > pos && ((uint8_t)text[end] & 0xC0) == 0x80);
      }
      out = text.substring(pos, end) + "…";
    } else {
      if (lastBreak > pos) end = lastBreak;
      out = text.substring(pos, end);
    }
    M5.Display.drawString(out, x, y + line * lineHeight);
    pos = end;
    while (pos < text.length() && text[pos] == ' ') ++pos;
  }
}

void drawHassAssist(bool force = false) {
  if (screenNow != Screen::HassAssist) return;
  // Every pipeline event (VAD start/end, wake-word, stt progress...) used to
  // clear and repaint the whole screen, causing visible flicker and stalling
  // loop() long enough to drop microphone chunks. Repaint only on change.
  String signature = String((int)hassAssistState) + '|' + hassAssistWakeWordEnabled + hassAssistWakeWordPaused
    + '|' + hassAssistError + '|' + hassAssistReply + '|' + hassAssistTranscript;
  if (!force && signature == hassAssistDrawnSignature) return;
  hassAssistDrawnSignature = signature;
  M5.Display.fillScreen(TFT_BLACK);
  uint16_t stateColor = hassAssistStateColor();
  M5.Display.fillCircle(16, 16, 5, stateColor);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(0x07E0, TFT_BLACK);
  useUIFont(1);
  M5.Display.drawString("HASS ASSIST", 28, 8);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  useUIFont(1);
  M5.Display.drawString(hassAssistStateText(), 160, 45);

  uint16_t buttonColor = hassAssistState == HassAssistState::Listening ? 0x04BF : (hassAssistState == HassAssistState::Speaking ? 0xFBE0 : 0x03E0);
  M5.Display.fillCircle(160, 117, 48, 0x1082);
  M5.Display.drawCircle(160, 117, 48, buttonColor);
  M5.Display.drawCircle(160, 117, 47, buttonColor);
  // Microphone icon.
  M5.Display.fillRoundRect(151, 88, 18, 38, 9, TFT_WHITE);
  M5.Display.drawRoundRect(143, 101, 34, 34, 14, TFT_WHITE);
  M5.Display.fillRect(158, 133, 4, 12, TFT_WHITE);
  M5.Display.fillRoundRect(149, 143, 22, 4, 2, TFT_WHITE);

  M5.Display.setClipRect(12, 160, 296, 50);
  M5.Display.setTextDatum(top_left);
  useUIFont(1);
  if (hassAssistError.length()) {
    M5.Display.setTextColor(0xF986, TFT_BLACK);
    drawAssistWrappedText(hassAssistError, 14, 161, 292, 2, 24);
  } else if (hassAssistReply.length()) {
    M5.Display.setTextColor(0xBDF7, TFT_BLACK);
    drawAssistWrappedText(hassAssistReply, 14, 161, 292, 2, 24);
  } else if (hassAssistTranscript.length()) {
    M5.Display.setTextColor(0x7DFF, TFT_BLACK);
    drawAssistWrappedText(hassAssistTranscript, 14, 161, 292, 2, 24);
  } else {
    M5.Display.setTextColor(0x7BEF, TFT_BLACK);
    const char* hint = hassAssistWakeWordEnabled
      ? "Say wake word · tap mic to pause/resume"
      : (hassAssistVoiceMode == HASS_MODE_HOLD ? "Hold mic while talking, release to send"
                                              : "Tap mic to talk, tap again to send");
    M5.Display.drawString(hassAssistWakeWordEnabled && hassAssistWakeWordPaused
      ? "Wake listening paused · tap mic to resume" : hint, 18, 174);
  }
  M5.Display.clearClipRect();
  drawBottomBar("", "", "Close");
}

void cleanupHassAssistAudio(bool stopSpeaker = false) {
  if (hassAssistMp3Decoder) { MP3FreeDecoder(hassAssistMp3Decoder); hassAssistMp3Decoder = nullptr; }
  if (hassAssistAudioData) { heap_caps_free(hassAssistAudioData); hassAssistAudioData = nullptr; }
  hassAssistAudioLength = 0;
  hassAssistMp3Position = 0;
  hassAssistAudioDecodeDone = false;
  if (stopSpeaker) M5.Speaker.stop(2);
}

void abortHassAssistMic() {
  hassAssistHolding = false;
  hassAssistStopRequested = false;
  hassAssistToggleListen = false;
  if (hassAssistMicRunning || M5.Mic.isRunning()) M5.Mic.end();
  M5.Mic.setBufferReleaseCallback(nullptr, nullptr);
  hassAssistMicOutstanding = 0;
  hassAssistMicReadyMask.store(0, std::memory_order_release);
  hassAssistMicRunning = false;
  M5.Speaker.begin();
}

// M5Unified owns two asynchronous recording slots. A falling isRecording()
// count is not a safe indication that a particular buffer is ready: its
// release callback can arrive slightly later. Mark the exact buffer here,
// then send it from the main loop where the WebSocket is safe to use.
void onHassAssistMicBufferReady(void*, void* data, size_t) {
  for (uint8_t i = 0; i < 4; ++i) {
    if (data == hassAssistMicBuffers[i]) {
      hassAssistMicReadyMask.fetch_or((uint8_t)(1U << i), std::memory_order_release);
      return;
    }
  }
}

void stopHassAssist() {
  abortHassAssistMic();
  cleanupHassAssistAudio(true);
  hassAssistTtsPending = false;
  hassAssistTtsUrl = "";
  hassAssistWebSocket.disconnect();
  hassAssistSocketStarted = false;
  hassAssistSocketConnected = false;
  hassAssistAuthenticated = false;
  hassAssistAudioHandlerId = -1;
  hassAssistPipelineActive = false;
  hassAssistWakeSessionActive = false;
  hassAssistWakeDetected = false;
  hassAssistRestartAt = 0;
  hassAssistPipelineListCommandId = 0;
  hassAssistPipelineCount = 0;
  hassAssistPreferredPipeline = "";
  hassAssistDiscoveryError = "";
  hassAssistState = hassAssistEnabled ? HassAssistState::Disconnected : HassAssistState::Disabled;
}

void beginHassAssistMic() {
  if (hassAssistMicRunning || hassAssistAudioHandlerId < 0 || !hassAssistSocketConnected) return;
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.setBufferReleaseCallback(nullptr, onHassAssistMicBufferReady);
  if (!M5.Mic.begin()) {
    M5.Mic.setBufferReleaseCallback(nullptr, nullptr);
    hassAssistError = "Microphone could not start";
    hassAssistState = HassAssistState::Error;
    drawHassAssist();
    return;
  }
  hassAssistMicQueueIndex = 0;
  hassAssistMicSendIndex = 0;
  hassAssistMicOutstanding = 0;
  hassAssistMicReadyMask.store(0, std::memory_order_release);
  hassAssistMicChunksSent = 0;
  hassAssistMicBytesSent = 0;
  hassAssistMicPeak = 0;
  for (int i = 0; i < 2; ++i) {
    if (M5.Mic.record(hassAssistMicBuffers[hassAssistMicQueueIndex], HASS_MIC_SAMPLES, 16000, false)) {
      hassAssistMicQueueIndex = (hassAssistMicQueueIndex + 1) % 4;
      ++hassAssistMicOutstanding;
    }
  }
  hassAssistMicRunning = true;
  hassAssistListenStarted = millis();
  hassAssistState = hassAssistWakeSessionActive && !hassAssistWakeDetected
    ? HassAssistState::WaitingWakeWord : HassAssistState::Listening;
  drawHassAssist();
}

void sendHassAssistAudioBuffer(const int16_t* samples, size_t sampleCount) {
  if (!hassAssistSocketConnected || hassAssistAudioHandlerId < 0 || !sampleCount) return;
  static uint8_t packet[1 + HASS_MIC_SAMPLES * sizeof(int16_t)];
  packet[0] = (uint8_t)hassAssistAudioHandlerId;
  memcpy(packet + 1, samples, sampleCount * sizeof(int16_t));
  hassAssistWebSocket.sendBIN(packet, 1 + sampleCount * sizeof(int16_t));
  uint16_t peak = 0;
  for (size_t i = 0; i < sampleCount; ++i) {
    int32_t magnitude = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
    if (magnitude > peak) peak = (uint16_t)min<int32_t>(magnitude, 32767);
  }
  hassAssistMicPeak = max(hassAssistMicPeak, peak);
  ++hassAssistMicChunksSent;
  hassAssistMicBytesSent += sampleCount * sizeof(int16_t);
}

void finishHassAssistMic() {
  Serial.printf("[assist] mic finished: chunks=%lu bytes=%lu peak=%u after %lu ms\n",
                (unsigned long)hassAssistMicChunksSent, (unsigned long)hassAssistMicBytesSent,
                hassAssistMicPeak, (unsigned long)(millis() - hassAssistListenStarted));
  if (hassAssistMicRunning) M5.Mic.end();
  M5.Mic.setBufferReleaseCallback(nullptr, nullptr);
  hassAssistMicRunning = false;
  hassAssistMicOutstanding = 0;
  hassAssistMicReadyMask.store(0, std::memory_order_release);
  M5.Speaker.begin();
  if (hassAssistSocketConnected && hassAssistAudioHandlerId >= 0) {
    uint8_t endMarker = (uint8_t)hassAssistAudioHandlerId;
    hassAssistWebSocket.sendBIN(&endMarker, 1);
  }
  hassAssistAudioHandlerId = -1;
  hassAssistStopRequested = false;
  hassAssistToggleListen = false;
  hassAssistState = HassAssistState::Processing;
  drawHassAssist();
}

void maintainHassAssistMic(uint32_t nowMs) {
  if (!hassAssistMicRunning) return;
  // nowMs is sampled at the top of loop(), but the mic may have started later
  // in the same pass (inside WebSocket event handling). Compare signed so a
  // listen start that is "newer" than nowMs does not underflow to a huge age
  // and stop every recording after the first two chunks.
  if (!hassAssistWakeSessionActive && (int32_t)(nowMs - hassAssistListenStarted) >= 15000) {
    hassAssistHolding = false;
    hassAssistStopRequested = true;
  }
  // Send in queue order and only after M5Unified has released this exact
  // pointer. This prevents incomplete PCM chunks from reaching HASS.
  while (hassAssistMicOutstanding) {
    uint8_t bit = (uint8_t)(1U << hassAssistMicSendIndex);
    uint8_t ready = hassAssistMicReadyMask.load(std::memory_order_acquire);
    if (!(ready & bit)) break;
    hassAssistMicReadyMask.fetch_and((uint8_t)~bit, std::memory_order_acq_rel);
    sendHassAssistAudioBuffer(hassAssistMicBuffers[hassAssistMicSendIndex], HASS_MIC_SAMPLES);
    hassAssistMicSendIndex = (hassAssistMicSendIndex + 1) % 4;
    --hassAssistMicOutstanding;
  }
  if (hassAssistStopRequested) {
    if (!hassAssistMicOutstanding && !M5.Mic.isRecording()) finishHassAssistMic();
    return;
  }
  while (hassAssistMicOutstanding < 2) {
    if (!M5.Mic.record(hassAssistMicBuffers[hassAssistMicQueueIndex], HASS_MIC_SAMPLES, 16000, false)) break;
    hassAssistMicQueueIndex = (hassAssistMicQueueIndex + 1) % 4;
    ++hassAssistMicOutstanding;
  }
}

void startHassAssistPipeline(bool wakeWordMode = false) {
  if (!hassAssistEnabled || !hassAssistAuthenticated || !hassAssistSocketConnected) {
    hassAssistError = hassAssistEnabled ? "Home Assistant is not connected" : "Configure HASS Assist in the web settings";
    hassAssistState = hassAssistEnabled ? HassAssistState::Disconnected : HassAssistState::Disabled;
    drawHassAssist();
    return;
  }
  if (hassAssistPipelineActive || hassAssistMicRunning || hassAssistMp3Decoder || hassAssistAudioData || hassAssistTtsPending) return;
  hassAssistError = "";
  hassAssistTranscript = "";
  hassAssistReply = "";
  hassAssistAudioHandlerId = -1;
  hassAssistStopRequested = false;
  hassAssistWakeSessionActive = wakeWordMode;
  hassAssistWakeDetected = false;
  hassAssistPipelineActive = true;
  hassAssistActiveCommandId = ++hassAssistCommandId;
  hassAssistLastEventAt = millis();
  hassAssistReplyInterrupted = false;
  JsonDocument command;
  command["id"] = hassAssistActiveCommandId;
  command["type"] = "assist_pipeline/run";
  command["start_stage"] = wakeWordMode ? "wake_word" : "stt";
  command["end_stage"] = "tts";
  command["input"]["sample_rate"] = 16000;
  if (wakeWordMode) {
    command["input"]["timeout"] = 30;
    command["input"]["noise_suppression_level"] = 2;
    command["input"]["auto_gain_dbfs"] = 31;
    command["input"]["volume_multiplier"] = 2.0;
  }
  if (hassAssistPipeline.length()) command["pipeline"] = hassAssistPipeline;
  String payload;
  serializeJson(command, payload);
  hassAssistWebSocket.sendTXT(payload);
  // Wake-word runs restart continuously; keep showing "waiting" instead of
  // flashing "Starting" between runs.
  hassAssistState = wakeWordMode ? HassAssistState::WaitingWakeWord : HassAssistState::Starting;
  drawHassAssist();
}

void processHassAssistEvent(JsonObject event) {
  String eventType = event["type"] | "";
  hassAssistLastEvent = eventType;
  hassAssistLastEventAt = millis();
  {
    String dataText; serializeJson(event["data"], dataText);
    Serial.printf("[assist] %lu event=%s state=%d mic=%d wake=%d chunks=%lu data=%.300s\n", (unsigned long)millis(),
                  eventType.c_str(), (int)hassAssistState, hassAssistMicRunning ? 1 : 0, hassAssistWakeSessionActive ? 1 : 0,
                  (unsigned long)hassAssistMicChunksSent, dataText.c_str());
  }
  JsonVariant data = event["data"];
  if (eventType == "run-start") {
    hassAssistAudioHandlerId = data["runner_data"]["stt_binary_handler_id"] | -1;
    if (hassAssistWakeSessionActive) beginHassAssistMic();
  } else if (eventType == "wake_word-start") {
    hassAssistWakeStageStartedAt = millis();
    hassAssistState = HassAssistState::WaitingWakeWord;
    if (hassAssistWakeSessionActive && !hassAssistMicRunning) beginHassAssistMic();
  } else if (eventType == "wake_word-end") {
    if (data["wake_word_output"].isNull() || data["wake_word_output"].size() == 0) {
      // No detection. If the engine gives up within seconds (instead of the
      // 30 s timeout) it is misconfigured or disconnecting; back off so the
      // device does not hammer Home Assistant and tell the user why.
      bool quick = millis() - hassAssistWakeStageStartedAt < 5000UL;
      hassAssistWakeEmptyStreak = quick ? min(hassAssistWakeEmptyStreak + 1, 50) : 0;
      if (hassAssistWakeEmptyStreak >= 3) {
        hassAssistError = "Wake word engine stops early - check the pipeline wake word in HA";
      }
    } else {
      hassAssistWakeEmptyStreak = 0;
      hassAssistError = "";
      hassAssistWakeDetected = true;
      hassAssistState = HassAssistState::Listening;
      if (screenNow != Screen::HassAssist) {
        haptic(12);
        wakeDisplay();
        hassAssistTranscript = ""; hassAssistReply = ""; hassAssistError = "";
        enterHassAssistScreenForShortcut();
      }
    }
  } else if (eventType == "stt-start") {
    if (hassAssistWakeSessionActive) {
      hassAssistWakeDetected = true;
      hassAssistState = HassAssistState::Listening;
      if (!hassAssistMicRunning) beginHassAssistMic();
    } else if (hassAssistHolding) beginHassAssistMic();
    else {
      uint8_t endMarker = (uint8_t)max(hassAssistAudioHandlerId, 0);
      if (hassAssistAudioHandlerId >= 0) hassAssistWebSocket.sendBIN(&endMarker, 1);
      hassAssistState = HassAssistState::Processing;
    }
  } else if (eventType == "stt-vad-end") {
    // Home Assistant detected the end of the spoken command after the wake
    // word. Finish the binary stream so Sherpa can transcribe immediately.
    hassAssistStopRequested = true;
  } else if (eventType == "stt-end") {
    hassAssistTranscript = data["stt_output"]["text"] | "";
    hassAssistState = HassAssistState::Processing;
  } else if (eventType == "intent-end") {
    hassAssistReply = data["intent_output"]["response"]["speech"]["plain"]["speech"] | "";
  } else if (eventType == "tts-end") {
    JsonVariant output;
    if (data["tts_output"].isNull()) output = data;
    else output = data["tts_output"].as<JsonVariant>();
    hassAssistTtsUrl = output["url"] | "";
    hassAssistTtsMime = output["mime_type"] | "";
    hassAssistTtsPending = hassAssistTtsUrl.length() && !hassAssistReplyInterrupted;
    if (hassAssistTtsPending) hassAssistState = HassAssistState::Downloading;
  } else if (eventType == "error") {
    String errorCode = data["code"] | "";
    bool wakeTimeout = hassAssistWakeSessionActive && errorCode == "wake-word-timeout";
    hassAssistError = wakeTimeout ? "" : String(data["message"] | "Assist pipeline failed");
    // Wake-word timeouts are normal between runs. Keep the display in the
    // wake-word state while the next pipeline run is scheduled.
    hassAssistState = wakeTimeout ? HassAssistState::WaitingWakeWord : HassAssistState::Error;
    abortHassAssistMic();
    hassAssistPipelineActive = false;
    hassAssistWakeSessionActive = false;
    hassAssistWakeDetected = false;
    if (wakeTimeout) hassAssistRestartAt = millis() + 350UL;
  } else if (eventType == "run-end") {
    bool wasWakeSession = hassAssistWakeSessionActive;
    if (hassAssistMicRunning) abortHassAssistMic();
    hassAssistPipelineActive = false;
    hassAssistWakeSessionActive = false;
    hassAssistWakeDetected = false;
    if (!hassAssistTtsPending && !hassAssistMp3Decoder && !hassAssistAudioData) {
      hassAssistState = !hassAssistAuthenticated ? HassAssistState::Disconnected
        : (wasWakeSession ? HassAssistState::WaitingWakeWord : HassAssistState::Ready);
    }
    if (wasWakeSession) {
      hassAssistRestartAt = millis() + (hassAssistWakeEmptyStreak >= 3 ? 8000UL : 350UL);
      if (hassAssistState == HassAssistState::Error) hassAssistState = HassAssistState::WaitingWakeWord;
    }
  }
  if (hassAssistWakeWordPaused && hassAssistState != HassAssistState::Error) {
    hassAssistState = HassAssistState::Paused;
  }
  drawHassAssist();
}

void requestHassAssistPipelines() {
  if (!hassAssistSocketConnected || !hassAssistAuthenticated) return;
  hassAssistPipelineListCommandId = ++hassAssistCommandId;
  JsonDocument command;
  command["id"] = hassAssistPipelineListCommandId;
  command["type"] = "assist_pipeline/pipeline/list";
  String payload;
  serializeJson(command, payload);
  hassAssistDiscoveryError = "";
  hassAssistWebSocket.sendTXT(payload);
}

void processHassAssistPipelineList(JsonVariantConst result) {
  hassAssistPipelineCount = 0;
  hassAssistPreferredPipeline = result["preferred_pipeline"] | "";
  JsonArrayConst pipelines = result["pipelines"].as<JsonArrayConst>();
  for (JsonObjectConst pipeline : pipelines) {
    if (hassAssistPipelineCount >= HASS_ASSIST_MAX_PIPELINES) break;
    String id = pipeline["id"] | "";
    if (!id.length()) continue;
    String name = pipeline["name"] | id;
    hassAssistPipelineIds[hassAssistPipelineCount] = id;
    hassAssistPipelineNames[hassAssistPipelineCount] = name;
    ++hassAssistPipelineCount;
  }
  hassAssistDiscoveryError = hassAssistPipelineCount ? "" : "No Assist pipelines were found";
}

void onHassAssistWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    hassAssistSocketConnected = true;
    hassAssistAuthenticated = false;
    hassAssistState = HassAssistState::Authenticating;
    drawHassAssist();
    return;
  }
  if (type == WStype_DISCONNECTED) {
    hassAssistSocketConnected = false;
    hassAssistAuthenticated = false;
    if (hassAssistMicRunning) abortHassAssistMic();
    hassAssistPipelineActive = false;
    hassAssistWakeSessionActive = false;
    hassAssistWakeDetected = false;
    hassAssistRestartAt = 0;
    hassAssistState = !hassAssistEnabled ? HassAssistState::Disabled
      : (hassAssistWakeWordPaused ? HassAssistState::Paused : HassAssistState::Disconnected);
    drawHassAssist();
    return;
  }
  if (type != WStype_TEXT || !payload || !length) return;
  JsonDocument message;
  if (deserializeJson(message, payload, length)) return;
  String messageType = message["type"] | "";
  if (messageType == "auth_required") {
    JsonDocument auth;
    auth["type"] = "auth";
    auth["access_token"] = hassAssistToken;
    String authPayload;
    serializeJson(auth, authPayload);
    hassAssistWebSocket.sendTXT(authPayload);
  } else if (messageType == "auth_ok") {
    hassAssistAuthenticated = true;
    hassAssistState = hassAssistWakeWordPaused ? HassAssistState::Paused : HassAssistState::Ready;
    hassAssistError = "";
    requestHassAssistPipelines();
    drawHassAssist();
  } else if (messageType == "auth_invalid") {
    hassAssistAuthenticated = false;
    hassAssistError = "Home Assistant token was rejected";
    hassAssistState = HassAssistState::Error;
    drawHassAssist();
  } else if (messageType == "result" && (uint32_t)(message["id"] | 0) == hassAssistPipelineListCommandId) {
    if (message["success"].as<bool>()) {
      processHassAssistPipelineList(message["result"].as<JsonVariantConst>());
    } else {
      hassAssistDiscoveryError = message["error"]["message"] | "Could not list Assist pipelines";
    }
  } else if (messageType == "event" && (uint32_t)(message["id"] | 0) == hassAssistActiveCommandId) {
    processHassAssistEvent(message["event"].as<JsonObject>());
  } else if (messageType == "result" && !message["success"].as<bool>() && (uint32_t)(message["id"] | 0) == hassAssistActiveCommandId) {
    hassAssistError = message["error"]["message"] | "Assist request was rejected";
    hassAssistState = HassAssistState::Error;
    hassAssistPipelineActive = false;
    hassAssistWakeSessionActive = false;
    hassAssistWakeDetected = false;
    drawHassAssist();
  }
}

bool parseHassAssistUrl(String& host, uint16_t& port, String& websocketPath, bool& secure) {
  String url = hassAssistBaseUrl;
  url.trim();
  secure = url.startsWith("https://");
  if (!secure && !url.startsWith("http://")) return false;
  url.remove(0, secure ? 8 : 7);
  int slash = url.indexOf('/');
  String hostPort = slash >= 0 ? url.substring(0, slash) : url;
  String prefix = slash >= 0 ? url.substring(slash) : "";
  while (prefix.endsWith("/")) prefix.remove(prefix.length() - 1);
  int colon = hostPort.lastIndexOf(':');
  port = secure ? 443 : 8123;
  if (colon > 0) {
    port = constrain(hostPort.substring(colon + 1).toInt(), 1, 65535);
    hostPort = hostPort.substring(0, colon);
  }
  host = hostPort;
  websocketPath = prefix + "/api/websocket";
  return host.length();
}

void connectHassAssist() {
  if (hassAssistSocketStarted || WiFi.status() != WL_CONNECTED || !hassAssistEnabled) return;
  if (!hassAssistBaseUrl.length() || !hassAssistToken.length()) {
    hassAssistState = HassAssistState::Disabled;
    hassAssistError = "Set the Home Assistant URL and token on the web page";
    drawHassAssist();
    return;
  }
  String host, path;
  uint16_t port;
  bool secure;
  if (!parseHassAssistUrl(host, port, path, secure)) {
    hassAssistState = HassAssistState::Error;
    hassAssistError = "Invalid Home Assistant URL";
    drawHassAssist();
    return;
  }
  hassAssistWebSocket.onEvent(onHassAssistWebSocketEvent);
  hassAssistWebSocket.setReconnectInterval(4000);
  hassAssistWebSocket.enableHeartbeat(10000, 3000, 2);
  if (secure) hassAssistWebSocket.beginSSL(host.c_str(), port, path.c_str(), "", "");
  else hassAssistWebSocket.begin(host.c_str(), port, path.c_str(), "");
  hassAssistSocketStarted = true;
  hassAssistState = HassAssistState::Connecting;
  drawHassAssist();
}

bool hassAssistReplyActive() {
  return hassAssistAudioData || hassAssistMp3Decoder || hassAssistTtsPending
    || (hassAssistPipelineActive && !hassAssistMicRunning && !hassAssistWakeSessionActive
        && (hassAssistState == HassAssistState::Processing || hassAssistState == HassAssistState::Downloading
            || hassAssistState == HassAssistState::Speaking));
}

// Stop the spoken reply (or the wait for it) immediately.
void interruptHassAssistReply() {
  Serial.println("[assist] reply interrupted by user");
  hassAssistReplyInterrupted = true;
  hassAssistTtsPending = false;
  cleanupHassAssistAudio(true);
  M5.Speaker.stop();
  hassAssistPipelineActive = false;
  hassAssistState = hassAssistWakeWordEnabled
    ? (hassAssistWakeWordPaused ? HassAssistState::Paused : HassAssistState::WaitingWakeWord)
    : HassAssistState::Ready;
  if (hassAssistWakeWordEnabled) hassAssistRestartAt = millis() + 500UL;
  drawHassAssist();
}

void enterHassAssistScreenForShortcut() {
  screenNow = Screen::HassAssist;
  hassAssistReturnToClock = true;
  hassAssistReturnAt = 0;
  drawHassAssist(true);
}

void showHassAssist() {
  hassAssistReturnToClock = false;
  screenNow = Screen::HassAssist;
  hassAssistTranscript = "";
  hassAssistReply = "";
  hassAssistError = "";
  if (!hassAssistEnabled) hassAssistState = HassAssistState::Disabled;
  else if (hassAssistWakeWordEnabled && hassAssistWakeWordPaused) hassAssistState = HassAssistState::Paused;
  else if (!hassAssistAuthenticated) hassAssistState = HassAssistState::Disconnected;
  else if (hassAssistWakeSessionActive) {
    hassAssistState = hassAssistWakeDetected ? HassAssistState::Listening : HassAssistState::WaitingWakeWord;
  } else hassAssistState = HassAssistState::Ready;
  drawHassAssist(true);
  connectHassAssist();
}

String absoluteHassAssistTtsUrl(String url) {
  url.trim();
  if (url.startsWith("http://") || url.startsWith("https://")) return url;
  if (!url.startsWith("/")) url = "/" + url;
  return hassAssistBaseUrl + url;
}

bool downloadHassAssistAudio() {
  String url = absoluteHassAssistTtsUrl(hassAssistTtsUrl);
  if (!url.length()) return false;
  uint32_t downloadStartedAt = millis();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  // HA streams TTS with chunked encoding on a keep-alive connection, so the
  // raw stream never "ends" and every reply waited for the 8 s idle timeout
  // (and chunk-size lines leaked into the MP3). HTTP/1.0 makes the server
  // send plain bytes and close the connection when the reply is complete.
  http.useHTTP10(true);
  WiFiClient plain;
  WiFiClientSecure secure;
  bool https = url.startsWith("https://");
  if (https) { secure.setInsecure(); if (!http.begin(secure, url)) return false; }
  else if (!http.begin(plain, url)) return false;
  if (hassAssistToken.length()) http.addHeader("Authorization", "Bearer " + hassAssistToken);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); hassAssistError = "Voice download HTTP " + String(code); return false; }
  int declared = http.getSize();
  static constexpr size_t MAX_ASSIST_AUDIO = 2 * 1024 * 1024;
  if (declared > (int)MAX_ASSIST_AUDIO) { http.end(); hassAssistError = "Voice reply is too large"; return false; }
  size_t capacity = declared > 0 ? (size_t)declared : 131072;
  capacity = max<size_t>(capacity, 4096);
  hassAssistAudioData = (uint8_t*)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!hassAssistAudioData) { http.end(); hassAssistError = "Not enough memory for voice reply"; return false; }
  WiFiClient* stream = http.getStreamPtr();
  hassAssistAudioLength = 0;
  bool streamMp3 = hassAssistTtsMime.indexOf("mpeg") >= 0 || hassAssistTtsMime.indexOf("mp3") >= 0;
  uint32_t lastDataAt = millis();
  while (http.connected() && (declared < 0 || hassAssistAudioLength < (size_t)declared)) {
    size_t available = stream->available();
    if (!available) {
      if (millis() - lastDataAt > 8000UL) break;
      if (!http.connected()) break;
      delay(1);
      continue;
    }
    if (hassAssistAudioLength + available > capacity) {
      size_t nextCapacity = min(MAX_ASSIST_AUDIO, max(capacity * 2, hassAssistAudioLength + available));
      if (nextCapacity <= capacity) break;
      uint8_t* grown = (uint8_t*)heap_caps_realloc(hassAssistAudioData, nextCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!grown) break;
      hassAssistAudioData = grown;
      capacity = nextCapacity;
    }
    size_t readNow = stream->readBytes(hassAssistAudioData + hassAssistAudioLength, min(available, capacity - hassAssistAudioLength));
    if (!readNow) break;
    hassAssistAudioLength += readNow;
    lastDataAt = millis();
    // Sherpa TTS streams the MP3 while it is still synthesising. Start
    // speaking as soon as a few frames are here instead of waiting for the
    // whole reply; playRaw() on a fixed channel paces the decoder.
    // The download runs inside loop(); poll the touch panel so a tap can
    // interrupt a long reply while it is still streaming.
    M5.update();
    if (M5.Touch.getDetail().wasPressed()) {
      http.end();
      interruptHassAssistReply();
      return false;
    }
    if (streamMp3) {
      if (!hassAssistMp3Decoder && hassAssistAudioLength >= 4096) {
        if (!beginHassAssistPlayback() || !hassAssistAudioData) { http.end(); return false; }
        Serial.printf("[assist] TTS playback started after %lu ms\n", (unsigned long)(millis() - downloadStartedAt));
      }
      while (hassAssistMp3Decoder && hassAssistAudioLength - hassAssistMp3Position > 2048) decodeNextHassAssistMp3Frame();
    }
  }
  http.end();
  Serial.printf("[assist] TTS download %u bytes in %lu ms\n", (unsigned)hassAssistAudioLength,
                (unsigned long)(millis() - downloadStartedAt));
  if (hassAssistAudioLength < 16) {
    cleanupHassAssistAudio();
    hassAssistError = "Voice reply was empty";
    return false;
  }
  return true;
}

bool beginHassAssistPlayback() {
  bool isWav = hassAssistAudioLength >= 4 && !memcmp(hassAssistAudioData, "RIFF", 4);
  bool isMp3 = hassAssistTtsMime.indexOf("mpeg") >= 0 || hassAssistTtsMime.indexOf("mp3") >= 0
    || (hassAssistAudioLength >= 3 && !memcmp(hassAssistAudioData, "ID3", 3))
    || (hassAssistAudioLength >= 2 && hassAssistAudioData[0] == 0xFF && (hassAssistAudioData[1] & 0xE0) == 0xE0);
  if (!isWav && !isMp3) {
    hassAssistError = "TTS must return MP3 or WAV audio";
    cleanupHassAssistAudio();
    return false;
  }
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.stop();
  M5.Speaker.setVolume((uint8_t)(hassAssistVolume * 255 / 100));
  hassAssistAudioDecodeDone = false;
  if (isWav) {
    if (!M5.Speaker.playWav(hassAssistAudioData, hassAssistAudioLength, 1, 2, true)) {
      hassAssistError = "WAV reply could not be played";
      cleanupHassAssistAudio();
      return false;
    }
    hassAssistAudioDecodeDone = true;
  } else {
    hassAssistMp3Decoder = MP3InitDecoder();
    if (!hassAssistMp3Decoder) {
      hassAssistError = "MP3 decoder could not start";
      cleanupHassAssistAudio();
      return false;
    }
    hassAssistMp3Position = 0;
    hassAssistMp3BufferIndex = 0;
  }
  hassAssistState = HassAssistState::Speaking;
  drawHassAssist();
  return true;
}

void decodeNextHassAssistMp3Frame() {
  if (!hassAssistMp3Decoder || hassAssistAudioDecodeDone) return;
  if (hassAssistMp3Position >= hassAssistAudioLength) { hassAssistAudioDecodeDone = true; return; }
  unsigned char* search = hassAssistAudioData + hassAssistMp3Position;
  int remaining = min<size_t>(INT_MAX, hassAssistAudioLength - hassAssistMp3Position);
  int syncOffset = MP3FindSyncWord(search, remaining);
  if (syncOffset < 0) { hassAssistAudioDecodeDone = true; return; }
  hassAssistMp3Position += syncOffset;
  unsigned char* frame = hassAssistAudioData + hassAssistMp3Position;
  int bytesLeft = min<size_t>(INT_MAX, hassAssistAudioLength - hassAssistMp3Position);
  int before = bytesLeft;
  int result = MP3Decode(hassAssistMp3Decoder, &frame, &bytesLeft, hassAssistMp3DecodeBuffer, 0);
  size_t consumed = before - bytesLeft;
  hassAssistMp3Position += consumed ? consumed : 1;
  if (result != ERR_MP3_NONE) return;
  MP3FrameInfo info;
  MP3GetLastFrameInfo(hassAssistMp3Decoder, &info);
  int channels = max(1, info.nChans);
  size_t samples = min<size_t>(HASS_MP3_FRAME_SAMPLES, info.outputSamps / channels);
  int16_t* output = hassAssistMp3Buffers[hassAssistMp3BufferIndex];
  for (size_t i = 0; i < samples; ++i) {
    output[i] = channels > 1 ? (int16_t)(((int32_t)hassAssistMp3DecodeBuffer[i * channels] + hassAssistMp3DecodeBuffer[i * channels + 1]) / 2) : hassAssistMp3DecodeBuffer[i];
  }
  M5.Speaker.playRaw(output, samples, info.samprate ? info.samprate : 16000, false, 1, 2, false);
  hassAssistMp3BufferIndex = (hassAssistMp3BufferIndex + 1) % 3;
}

bool hassAssistWakeWordAllowed() {
  return alarmActive < 0
    && meditationState != MeditationState::Running
    && screenNow != Screen::FirmwareUpdate;
}

void maintainHassAssist(uint32_t nowMs) {
  if (!hassAssistEnabled) return;
  if (!hassAssistSocketStarted) connectHassAssist();
  if (hassAssistSocketStarted) hassAssistWebSocket.loop();
  // Keep the authenticated WebSocket alive in the background so the local
  // settings page can discover Home Assistant pipelines (including Wyoming
  // pipelines backed by Sherpa ONNX) without exposing the access token.
  if (hassAssistWakeSessionActive && !hassAssistWakeWordAllowed()) {
    // Alarm audio, meditation audio and firmware installation take priority.
    // Closing the socket cancels the active pipeline cleanly; background
    // listening is started again when the device becomes available.
    stopHassAssist();
    return;
  }
  maintainHassAssistMic(nowMs);
  // A run that stops producing events (e.g. HA stalls after an empty audio
  // stream) used to leave pipelineActive set forever, so every later tap was
  // ignored. Reset so the user can talk again.
  if (hassAssistPipelineActive && !hassAssistWakeSessionActive && !hassAssistMicRunning
      && (int32_t)(millis() - hassAssistLastEventAt) > 20000) {
    Serial.println("[assist] pipeline stalled; resetting");
    abortHassAssistMic();
    hassAssistPipelineActive = false;
    hassAssistTtsPending = false;
    hassAssistError = "Assist timed out, please try again";
    hassAssistState = HassAssistState::Error;
    drawHassAssist();
  }
  if (hassAssistTtsPending && !hassAssistMicRunning && !hassAssistAudioData) {
    hassAssistTtsPending = false;
    hassAssistState = HassAssistState::Downloading;
    drawHassAssist();
    if ((!downloadHassAssistAudio() || (!hassAssistMp3Decoder && !beginHassAssistPlayback()))
        && !hassAssistReplyInterrupted) {
      hassAssistState = HassAssistState::Error;
      drawHassAssist();
    }
  }
  if (hassAssistMp3Decoder && !hassAssistAudioDecodeDone) decodeNextHassAssistMp3Frame();
  if (hassAssistAudioData && hassAssistAudioDecodeDone && !M5.Speaker.isPlaying(2)) {
    cleanupHassAssistAudio();
    hassAssistState = !hassAssistAuthenticated ? HassAssistState::Disconnected
      : (hassAssistWakeWordEnabled && hassAssistWakeWordPaused ? HassAssistState::Paused : HassAssistState::Ready);
    drawHassAssist();
  }
  if (hassAssistReturnToClock) {
    bool busy = hassAssistMicRunning || hassAssistTtsPending || hassAssistAudioData || hassAssistMp3Decoder
      || (hassAssistPipelineActive && !(hassAssistWakeSessionActive && !hassAssistWakeDetected));
    if (screenNow != Screen::HassAssist) {
      hassAssistReturnToClock = false;
    } else if (busy) {
      hassAssistReturnAt = 0;
    } else if (!hassAssistReturnAt) {
      hassAssistReturnAt = millis() + 2500UL;  // leave the reply readable briefly
    } else if ((int32_t)(millis() - hassAssistReturnAt) >= 0) {
      hassAssistReturnToClock = false;
      hassAssistReturnAt = 0;
      screenNow = Screen::Clock;
      drawClock(true); drawAstronaut();
    }
  }
  if (hassAssistWakeWordEnabled && !hassAssistWakeWordPaused && hassAssistAuthenticated && hassAssistWakeWordAllowed()
      && hassAssistState != HassAssistState::Error
      && !hassAssistPipelineActive && !hassAssistMicRunning && !hassAssistTtsPending
      && !hassAssistAudioData && !hassAssistMp3Decoder
      && (!hassAssistRestartAt || (int32_t)(nowMs - hassAssistRestartAt) >= 0)) {
    hassAssistRestartAt = 0;
    startHassAssistPipeline(true);
  }
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
  useUIFont(1); M5.Display.setTextColor(TFT_WHITE, BG);
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
  for (int i = 0; i < 4; ++i) {
    if (meditationSettingsPage == 0 && i < 2) {
      drawSettingsSliderRow(i, i ? "Preset time 2" : "Preset time 1", meditationPresetMinutes[i], 1, 60, "min");
    } else {
      drawSettingsRow(i, rows[i], values[i]);
    }
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
    M5.Display.fillRoundRect(18, 52 + i * 48, 284, 36, 7, selected ? UI_BLUE : PANEL);
    M5.Display.drawRoundRect(18, 52 + i * 48, 284, 36, 7, selected ? 0x65DF : UI_BORDER);
    M5.Display.setTextColor(TFT_WHITE);
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
  M5.Display.setTextColor(TFT_WHITE, BG);
  for (int row = 0; row < ALARMS_PER_PAGE; ++row) {
    int i = alarmPage * ALARMS_PER_PAGE + row;
    int y = 43 + row * 42;
    M5.Display.drawFastHLine(5, y + 37, 310, UI_BORDER);
    char b[16]; snprintf(b, sizeof(b), "%02d:%02d", alarms[i].hour, alarms[i].minute);
    useUIMediumFont(); M5.Display.drawString(b, 14, y);
    useUIFont(1); M5.Display.drawString(weekdaysText(alarms[i].weekdays), 160, y + 7);
    M5.Display.fillRoundRect(250, y + 3, 50, 25, 12, alarms[i].enabled ? TFT_GREEN : 0xAD55);
    M5.Display.setTextColor(TFT_WHITE); M5.Display.drawCentreString(alarms[i].enabled ? "ON" : "OFF", 275, y + 10, 1);
    M5.Display.setTextColor(TFT_WHITE, BG);
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
  char heading[24]; snprintf(heading, sizeof(heading), "Clock settings %u/3", clockSettingsPage + 1);
  title(heading);
  if (clockSettingsPage == 0) {
    drawSettingsRow(0, "Time zone", TIME_ZONES[timeZoneIndex].city);
    drawSettingsRow(1, "Auto brightness", adaptiveBrightness ? "ON" : "OFF");
    drawSettingsRow(2, "Day brightness", String(dayBrightness) + "%");
    drawSettingsRow(3, "Night brightness", String(nightBrightness) + "%");
  } else if (clockSettingsPage == 1) {
    drawSettingsRow(0, "Alarm volume", String(alarmVolume) + "%");
    drawSettingsRow(1, "Screen off", screenOffText());
    drawSettingsRow(2, "Time format", use24HourTime ? "24 hour" : "12 hour");
    drawSettingsRow(3, "Flat buttons", flatVirtualButtonsEnabled ? "ON" : "OFF");
  } else {
    const char* modes[] = {"Stay on", "Timed off", "Timed fade"};
    drawSettingsRow(0, "Night light", nightLightEnabled ? "ON" : "OFF");
    drawSettingsRow(1, "Color", "", (int32_t)nightLightColor);
    drawSettingsRow(2, "LED brightness", String(nightLightBrightness) + "%");
    drawSettingsRow(3, "Mode", modes[nightLightMode]);
  }
  drawBottomBar(clockSettingsPage ? "Previous" : "NTP sync", clockSettingsPage < 2 ? "Next" : "", "Close");
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
  useUIFont(1); M5.Display.setTextColor(TFT_WHITE, BG);
  M5.Display.drawString("Current", 14, 45); M5.Display.drawString(SPACE_CLOCK_VERSION, 190, 45);
  M5.Display.drawString("Latest", 14, 75); M5.Display.drawString(latestFirmwareVersion.length() ? latestFirmwareVersion : "Not checked", 190, 75);
  M5.Display.drawString("Automatic update", 14, 105); M5.Display.drawString(automaticFirmwareUpdate ? "ON" : "OFF", 250, 105);
  char checkTime[8]; snprintf(checkTime, sizeof(checkTime), "%02u:00", firmwareCheckHour);
  M5.Display.drawString("Daily check", 14, 135); M5.Display.drawString(checkTime, 238, 135);
  M5.Display.setTextColor(firmwareUpdateAvailable ? TFT_GREEN : UI_MUTED, BG);
  M5.Display.setTextDatum(top_left);
  String line1 = firmwareUpdateMessage, line2;
  if (line1.length() > 43) { int split = line1.lastIndexOf(' ', 43); if (split < 15) split = 43; line2 = line1.substring(split + 1); line1 = line1.substring(0, split); }
  M5.Display.drawString(line1, 14, 165); if (line2.length()) M5.Display.drawString(line2, 14, 184);
  drawBottomBar("Check", firmwareUpdateAvailable ? "Install" : "", "Close");
}

void drawFirmwareDownloadProgress(uint8_t percent, uint8_t attempt, uint8_t attempts, const char* label = "Downloading") {
  if (screenNow != Screen::FirmwareUpdate) return;
  percent = min((uint8_t)100, percent);
  String attemptText = attempts ? String(attempt) + "/" + String(attempts) : String("Web");
  if (firmwareProgressCanvasReady) {
    firmwareProgressCanvas.fillSprite(BG);
    firmwareProgressCanvas.setFont(&SourceHanSansTC_UI8pt8b);
    firmwareProgressCanvas.setTextSize(1);
    firmwareProgressCanvas.setTextColor(TFT_GREEN, BG);
    firmwareProgressCanvas.setTextDatum(top_left);
    firmwareProgressCanvas.drawString(String(label) + " " + String(percent) + "%", 0, 1);
    firmwareProgressCanvas.setTextColor(UI_MUTED, BG);
    firmwareProgressCanvas.setTextDatum(top_right);
    firmwareProgressCanvas.drawString(attemptText, 291, 1);
    firmwareProgressCanvas.drawRoundRect(0, 27, 292, 13, 4, UI_BORDER);
    if (percent) firmwareProgressCanvas.fillRoundRect(2, 29, (288 * percent) / 100, 9, 3, TFT_GREEN);
    firmwareProgressCanvas.pushSprite(14, 162);
    return;
  }

  // Low-memory fallback still updates only the progress area instead of
  // clearing and redrawing the entire screen.
  M5.Display.fillRect(14, 162, 292, 44, BG);
  useUIFont(1);
  M5.Display.setTextColor(TFT_GREEN, BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(String(label) + " " + String(percent) + "%", 14, 163);
  M5.Display.setTextColor(UI_MUTED, BG);
  M5.Display.setTextDatum(top_right);
  M5.Display.drawString(attemptText, 305, 163);
  M5.Display.drawRoundRect(14, 189, 292, 13, 4, UI_BORDER);
  if (percent) M5.Display.fillRoundRect(16, 191, (288 * percent) / 100, 9, 3, TFT_GREEN);
}

void drawFirmwareVerificationStatus(uint8_t attempt, uint8_t attempts) {
  if (screenNow != Screen::FirmwareUpdate) return;
  String attemptText = attempts ? String(attempt) + "/" + String(attempts) : String("Web");
  if (firmwareProgressCanvasReady) {
    firmwareProgressCanvas.fillSprite(BG);
    firmwareProgressCanvas.setFont(&SourceHanSansTC_UI8pt8b);
    firmwareProgressCanvas.setTextSize(1);
    firmwareProgressCanvas.setTextColor(TFT_YELLOW, BG);
    firmwareProgressCanvas.setTextDatum(top_left);
    firmwareProgressCanvas.drawString("Verifying firmware...", 0, 1);
    firmwareProgressCanvas.setTextColor(UI_MUTED, BG);
    firmwareProgressCanvas.setTextDatum(top_right);
    firmwareProgressCanvas.drawString(attemptText, 291, 1);
    firmwareProgressCanvas.drawRoundRect(0, 27, 292, 13, 4, UI_BORDER);
    firmwareProgressCanvas.fillRoundRect(2, 29, 86, 9, 3, TFT_YELLOW);
    firmwareProgressCanvas.pushSprite(14, 162);
    return;
  }
  M5.Display.fillRect(14, 162, 292, 44, BG);
  useUIFont(1);
  M5.Display.setTextColor(TFT_YELLOW, BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString("Verifying firmware...", 14, 163);
  M5.Display.setTextColor(UI_MUTED, BG);
  M5.Display.setTextDatum(top_right);
  M5.Display.drawString(attemptText, 305, 163);
  M5.Display.drawRoundRect(14, 189, 292, 13, 4, UI_BORDER);
  M5.Display.fillRoundRect(16, 191, 86, 9, 3, TFT_YELLOW);
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
  latestFirmwareSha256 = doc["sha256"].is<const char*>() ? doc["sha256"].as<String>() : "";
  latestFirmwareExpectedSize = doc["size"].is<uint32_t>() ? doc["size"].as<uint32_t>() : 0;
  firmwareUpdateAvailable = compareFirmwareVersions(SPACE_CLOCK_VERSION, latestFirmwareVersion) < 0;
  firmwareUpdateMessage = firmwareUpdateAvailable ? "New firmware is ready. Tap Install." : "This firmware is up to date.";
  if (redraw) showFirmwareUpdate();
  return true;
}

bool webFirmwareUploadOk = false;
int webFirmwareLastPercent = -1;

struct FirmwareFinalizeJob {
  bool evenIfRemaining;
  volatile bool done;
  volatile bool ok;
};

void firmwareFinalizeTask(void* arg) {
  FirmwareFinalizeJob* job = static_cast<FirmwareFinalizeJob*>(arg);
  job->ok = Update.end(job->evenIfRemaining);
  job->done = true;
  vTaskDelete(nullptr);
}

bool finalizeFirmwareUpdateSafely(bool exactSize = true) {
  // esp_ota_set_boot_partition() re-reads the whole 5+ MB image through
  // thousands of flash mmap/unmap calls. Run from loopTask (CPU1) with the
  // CPU clock switched, this repeatedly deadlocked in
  // spi_flash_disable_interrupts_caches_and_other_cpu() until the watchdog
  // rebooted into the old firmware. Verify on a dedicated CPU0 task instead,
  // keep the clock unchanged, and let loopTask yield so IDLE1 stays fed.
  uint32_t verifyStartedAt = millis();
  Serial.printf("[ota] finalizing image size=%u, update progress=%u, exact=%d\n",
                (unsigned)Update.size(), (unsigned)Update.progress(), exactSize ? 1 : 0);
  // Manifest downloads know the exact size: refuse to activate a short image.
  // Browser uploads start with UPDATE_SIZE_UNKNOWN, so they must finalize at
  // the received length or end() always fails with "premature end".
  static FirmwareFinalizeJob job;
  job.evenIfRemaining = !exactSize;
  job.done = false;
  job.ok = false;
  if (xTaskCreatePinnedToCore(firmwareFinalizeTask, "otaFinalize", 8192, &job, 5, nullptr, 0) != pdPASS) {
    Serial.println("[ota] could not start finalize task; finalizing inline");
    job.ok = Update.end(job.evenIfRemaining);
    job.done = true;
  }
  while (!job.done) {
    if (millis() - verifyStartedAt > 180000UL) {
      Serial.println("[ota] finalize timed out; restarting into current firmware");
      delay(100);
      ESP.restart();
    }
    delay(20);
  }
  Serial.printf("[ota] finalize=%s, elapsed=%lu ms\n",
                job.ok ? "ok" : Update.errorString(), (unsigned long)(millis() - verifyStartedAt));
  return job.ok;
}

bool installLatestFirmware(bool redraw = true) {
  if (!firmwareUpdateAvailable || !latestFirmwareUrl.length()) return false;
  static constexpr uint8_t MAX_ATTEMPTS = 2;
  static constexpr uint32_t STALL_TIMEOUT_MS = 12000;
  wifi_ps_type_t previousSleepMode = WiFi.getSleep();
  WiFi.setSleep(false);
  bool success = false;
  String failure = "Download failed.";

  for (uint8_t attempt = 1; attempt <= MAX_ATTEMPTS && !success; ++attempt) {
    firmwareUpdateMessage = "Connecting (attempt " + String(attempt) + "/" + String(MAX_ATTEMPTS) + ")...";
    if (redraw) showFirmwareUpdate();
    Serial.printf("[ota] attempt %u/%u: %s\n", attempt, MAX_ATTEMPTS, latestFirmwareUrl.c_str());

    WiFiClientSecure secure;
    secure.setInsecure();
    secure.setTimeout(5);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(secure, latestFirmwareUrl)) {
      failure = "Could not open firmware URL.";
      Serial.println("[ota] HTTP begin failed");
      continue;
    }

    int status = http.GET();
    int imageSize = http.getSize();
    Serial.printf("[ota] HTTP %d, content length %d\n", status, imageSize);
    if (status != HTTP_CODE_OK || imageSize <= 0) {
      failure = "Firmware server returned HTTP " + String(status) + ".";
      http.end();
      continue;
    }
    if (latestFirmwareExpectedSize && (uint32_t)imageSize != latestFirmwareExpectedSize) {
      failure = "Firmware size does not match manifest.";
      Serial.printf("[ota] size mismatch: manifest %u, response %d\n", latestFirmwareExpectedSize, imageSize);
      http.end();
      continue;
    }
    if (!Update.begin((size_t)imageSize, U_FLASH)) {
      failure = "OTA slot unavailable: " + String(Update.errorString()) + ".";
      Serial.printf("[ota] Update.begin failed: %s\n", Update.errorString());
      http.end();
      continue;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    NetworkClient* stream = http.getStreamPtr();
    static uint8_t buffer[4096];
    size_t written = 0;
    uint32_t lastDataAt = millis();
    uint32_t lastDrawAt = 0;
    int lastPercent = -1;
    bool streamFailed = false;

    while (written < (size_t)imageSize) {
      int available = stream ? stream->available() : 0;
      if (available > 0) {
        size_t toRead = (size_t)available;
        if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
        if (toRead > (size_t)imageSize - written) toRead = (size_t)imageSize - written;
        int received = stream->read(buffer, toRead);
        if (received > 0) {
          if (Update.write(buffer, (size_t)received) != (size_t)received) {
            failure = "Flash write failed: " + String(Update.errorString()) + ".";
            Serial.printf("[ota] flash write failed at %u: %s\n", (unsigned)written, Update.errorString());
            streamFailed = true;
            break;
          }
          mbedtls_sha256_update(&sha, buffer, (size_t)received);
          written += (size_t)received;
          lastDataAt = millis();
          int percent = (int)((written * 100ULL) / (size_t)imageSize);
          if (percent != lastPercent && (percent == 100 || percent >= lastPercent + 2 || millis() - lastDrawAt >= 1000)) {
            lastPercent = percent;
            lastDrawAt = millis();
            firmwareUpdateMessage = "Downloading " + String(percent) + "% (" + String(attempt) + "/" + String(MAX_ATTEMPTS) + ")";
            if (redraw) drawFirmwareDownloadProgress((uint8_t)percent, attempt, MAX_ATTEMPTS);
            Serial.printf("[ota] %d%% (%u/%d bytes)\n", percent, (unsigned)written, imageSize);
          }
          continue;
        }
      }

      if (stream && !stream->connected()) {
        failure = "Firmware connection closed at " + String((written * 100ULL) / (size_t)imageSize) + "%.";
        Serial.printf("[ota] connection closed at %u/%d bytes\n", (unsigned)written, imageSize);
        streamFailed = true;
        break;
      }
      if (millis() - lastDataAt >= STALL_TIMEOUT_MS) {
        failure = "Download stalled for 12 seconds.";
        Serial.printf("[ota] stalled at %u/%d bytes\n", (unsigned)written, imageSize);
        streamFailed = true;
        break;
      }
      delay(2);
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    http.end();

    if (!streamFailed && written == (size_t)imageSize) {
      char digestHex[65];
      for (uint8_t i = 0; i < sizeof(digest); ++i) snprintf(digestHex + i * 2, 3, "%02x", digest[i]);
      digestHex[64] = 0;
      if (latestFirmwareSha256.length() == 64 && !latestFirmwareSha256.equalsIgnoreCase(digestHex)) {
        failure = "Firmware checksum mismatch.";
        Serial.printf("[ota] SHA-256 mismatch: expected %s, got %s\n", latestFirmwareSha256.c_str(), digestHex);
        Update.abort();
      } else {
        // Show a distinct state so 100% no longer looks frozen while the
        // complete image and boot partition are validated.
        if (redraw) drawFirmwareVerificationStatus(attempt, MAX_ATTEMPTS);
        Serial.println("[ota] download verified; validating boot partition");
        bool updateEnded = finalizeFirmwareUpdateSafely();
        if (!updateEnded) {
          failure = "Update finalize failed: " + String(Update.errorString()) + ".";
          Serial.printf("[ota] Update.end failed: %s\n", Update.errorString());
        } else {
          Serial.printf("[ota] verified %s; update ready\n", digestHex);
          success = true;
        }
      }
    } else {
      Update.abort();
    }

    if (!success && attempt < MAX_ATTEMPTS) {
      firmwareUpdateMessage = failure + " Retrying...";
      if (redraw) showFirmwareUpdate();
      delay(750);
    }
  }

  WiFi.setSleep(previousSleepMode);
  if (!success) {
    firmwareUpdateMessage = failure;
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
  M5.Display.setTextColor(0x65DF, BG); useUIFont(1);
  M5.Display.drawCentreString(SPACE_CLOCK_VERSION, 160, 150, 2);
  M5.Display.drawCentreString("Native firmware for M5Stack Core2", 160, 177, 2);
  drawBottomBar("", "", "Close");
}

void runWifiPortal(bool automatic) {
  if (settingsServerReady) settingsServer.stop();
  title("Wi-Fi setup");
  M5.Display.setTextColor(TFT_WHITE, BG); useUIFont(1);
  M5.Display.drawCentreString("Connect to SpaceClock-Setup", 160, 90, 2);
  M5.Display.drawCentreString("and open the captive portal", 160, 115, 2);
  if (automatic) M5.Display.drawCentreString("No Wi-Fi saved yet - first-time setup", 160, 145, 2);
  WiFiManager wm;
  char compHost[40] = {0}, compPort[8] = {0};
  companionHosts[0].substring(0, 39).toCharArray(compHost, sizeof(compHost));
  snprintf(compPort, sizeof(compPort), "%u", companionPorts[0]);
  WiFiManagerParameter companionHostField("companion_host", "Companion page 1 host/IP", compHost, 39);
  WiFiManagerParameter companionPortField("companion_port", "Companion page 1 port", compPort, 7);
  wm.addParameter(&companionHostField);
  wm.addParameter(&companionPortField);
  drawBottomBar("", "", "Back");
  // Non-blocking portal so the Back button works while waiting for a phone.
  wm.setConfigPortalBlocking(false);
  wm.startConfigPortal(SPACE_CLOCK_WIFI_AP);
  bool saved = false;
  uint32_t portalStartedAt = millis();
  while (millis() - portalStartedAt < 180000UL) {
    if (wm.process()) { saved = true; break; }
    M5.update();
    auto t = M5.Touch.getDetail();
    if (t.wasReleased() && t.y >= 210 && t.x >= 214) { haptic(15); break; }
    delay(10);
  }
  if (!saved) wm.stopConfigPortal();
  if (saved) {
    companionHosts[0] = companionHostField.getValue();
    companionPorts[0] = max(1, atoi(companionPortField.getValue()));
    saveSettings();
  }
  // Return to normal station mode (the portal leaves the soft AP running).
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  if (WiFi.status() != WL_CONNECTED) WiFi.begin();
  if (WiFi.status() == WL_CONNECTED) {
    if (settingsServerReady) settingsServer.begin(); else setupSettingsServer();
    syncTime();
  }
  if (automatic) { screenNow = Screen::Clock; drawClock(true); drawAstronaut(); }
  else showMenu();
}

void dismissAlarm() {
  m5::rtc_datetime_t now; getClockDateTime(&now);
  alarms[alarmActive].lastDay = now.date.year * 512 + now.date.month * 32 + now.date.date;
  if (!alarms[alarmActive].weekdays) alarms[alarmActive].enabled = false;
  alarmActive = -1;
  resetAlarmChallengeState();
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
  } else if (screenNow == Screen::NightLight) {
    color = nightLightColor;
    strength = nightLightBrightness;
  } else if (manualNightLightOverride) {
    if (manualNightLightActive) {
      color = nightLightColor;
      strength = nightLightBrightness;
    }
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
  // Only the manual night light (long press on the right button) enables
  // the full-screen night light.
  nightLedShowing = alarmActive < 0 && !screenSleeping && manualNightLightOverride && manualNightLightActive;
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
  resetAlarmChallengeState();
  playSoundChoice(alarmSound, alarmVolume, UINT32_MAX);
  drawAlarmChallenge();
}

void snoozeAlarm() {
  if (alarmActive < 0) return;
  m5::rtc_datetime_t now; getClockDateTime(&now);
  alarms[alarmActive].lastDay = now.date.year * 512 + now.date.month * 32 + now.date.date;
  snoozedAlarm = alarmActive;
  snoozeStarted = millis();
  alarmActive = -1;
  resetAlarmChallengeState();
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

String emotionLocalTimeText(const m5::rtc_datetime_t& dt) {
  char value[24];
  snprintf(value, sizeof(value), "%04d-%02d-%02d %02u:%02u", dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes);
  return String(value);
}

uint8_t emotionDaysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return days[constrain((int)month, 1, 12) - 1];
}

bool emotionApiConnected() {
  return emotionApiState == EmotionApiState::Connected && WiFi.status() == WL_CONNECTED
    && emotionApiToken.length() && emotionApiUserId.length();
}

bool emotionApiConfigured() {
  return emotionApiToken.length() && emotionApiUserId.length();
}

uint16_t emotionTheme(uint8_t strength = 100) {
  return matrixColor(strength);
}

uint16_t emotionPanel(uint8_t strength = 12) {
  return emotionTheme(strength);
}

void drawEmotionMatrixBackground() {
  M5.Display.fillScreen(TFT_BLACK);
  useUIFont(1);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(emotionTheme(12), TFT_BLACK);
  static const char matrixGlyphs[] = "01ABCDEFGHIJKLMNOPQRSTUVWXYZ<>[]{}+-";
  const uint8_t glyphCount = sizeof(matrixGlyphs) - 1;
  for (int i = 0; i < 30; ++i) {
    char glyph[2] = {matrixGlyphs[(i * 11 + emotionFormPage * 7) % glyphCount], 0};
    int x = (i * 47 + emotionFormPage * 23) % 316;
    int y = 34 + ((i * 61 + emotionFormPage * 19) % 170);
    M5.Display.drawString(glyph, x, y);
  }
  M5.Display.fillRect(0, 0, 320, 34, TFT_BLACK);
  M5.Display.fillRect(0, 212, 320, 28, TFT_BLACK);
}

void drawEmotionConnectionIndicator() {
  uint16_t color = emotionApiConnected() ? TFT_GREEN
    : (emotionApiState == EmotionApiState::Checking ? TFT_YELLOW : TFT_RED);
  M5.Display.fillCircle(9, 13, 6, TFT_BLACK);
  M5.Display.drawCircle(9, 13, 5, emotionTheme(55));
  M5.Display.fillCircle(9, 13, 4, color);
}

void drawEmotionResetIcon() {
  if (emotionFormPage >= 7) return;
  uint16_t color = emotionTheme(100);
  M5.Display.drawCircle(300, 14, 9, color);
  M5.Display.fillRect(299, 4, 10, 7, TFT_BLACK);
  M5.Display.fillTriangle(307, 5, 313, 7, 309, 12, color);
}

void drawEmotionBottomBar(const char* left, const char* middle, const char* right) {
  M5.Display.fillRect(0, 212, 320, 28, TFT_BLACK);
  M5.Display.drawFastHLine(0, 214, 320, emotionTheme(32));
  useUIFont(1);
  M5.Display.setTextColor(emotionTheme(100), TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  if (!drawBottomActionIcon(left, 53)) M5.Display.drawString(left, 53, 228);
  if (!drawBottomActionIcon(middle, 160)) M5.Display.drawString(middle, 160, 228);
  if (!drawBottomActionIcon(right, 267)) M5.Display.drawString(right, 267, 228);
}

String emotionTitleText() {
  if (emotionFormPage == 5) return "情緒觀察 6/8 觀察次數";
  if (emotionFormPage == 7) return "情緒觀察 8/8 本筆總覽";
  return "情緒觀察 " + String(emotionFormPage + 1) + "/8 " + EMOTION_PAGE_TITLES[emotionFormPage];
}

void drawEmotionRow(int y, const String& label, const String& value, bool selected = false) {
  uint16_t fill = selected ? emotionPanel(20) : emotionPanel((y / 32 & 1) ? 10 : 14);
  M5.Display.fillRoundRect(10, y, 300, 28, 5, fill);
  M5.Display.drawRoundRect(10, y, 300, 28, 5, selected ? emotionTheme(75) : emotionTheme(30));
  useUIFont(1);
  M5.Display.setTextColor(selected ? emotionTheme(100) : TFT_WHITE, fill);
  M5.Display.setTextDatum(middle_left);
  M5.Display.drawString(label, 18, y + 14);
  M5.Display.setTextDatum(middle_right);
  M5.Display.drawString(value, 302, y + 14);
}

// Four large rows fill the space between the title and the bottom bar.
static constexpr int EMOTION_BIG_ROW_TOP = 32;
static constexpr int EMOTION_BIG_ROW_PITCH = 44;
void drawEmotionBigRow(uint8_t index, const String& label, const String& value, bool selectable = false) {
  int y = EMOTION_BIG_ROW_TOP + index * EMOTION_BIG_ROW_PITCH;
  uint16_t fill = emotionPanel(selectable ? 20 : ((index & 1) ? 10 : 14));
  M5.Display.fillRoundRect(8, y, 304, EMOTION_BIG_ROW_PITCH - 4, 8, fill);
  M5.Display.drawRoundRect(8, y, 304, EMOTION_BIG_ROW_PITCH - 4, 8, selectable ? emotionTheme(75) : emotionTheme(30));
  useUIMediumFont();
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(selectable ? emotionTheme(100) : emotionTheme(75), fill);
  M5.Display.drawString(label, 18, y + (EMOTION_BIG_ROW_PITCH - 4) / 2);
  M5.Display.setClipRect(150, y + 2, 156, EMOTION_BIG_ROW_PITCH - 8);
  M5.Display.setTextDatum(middle_right);
  M5.Display.setTextColor(TFT_WHITE, fill);
  M5.Display.drawString(value, 302, y + (EMOTION_BIG_ROW_PITCH - 4) / 2);
  M5.Display.clearClipRect();
}

void drawEmotionChoicePanel(int y, const char* label, const String& value, uint16_t fill, uint16_t accent) {
  M5.Display.fillRoundRect(12, y, 296, 61, 10, fill);
  M5.Display.drawRoundRect(12, y, 296, 61, 10, accent);

  useUIFont(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(emotionTheme(70), fill);
  M5.Display.drawString(label, 160, y + 13);

  // Keep navigation separate from the value so every category and emotion is
  // rendered on exactly the same baseline and with exactly the same font.
  M5.Display.fillTriangle(28, y + 43, 37, y + 35, 37, y + 51, accent);
  M5.Display.fillTriangle(292, y + 43, 283, y + 35, 283, y + 51, accent);
  M5.Display.setClipRect(44, y + 22, 232, 36);
  useUIMediumFont();
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, fill);
  M5.Display.drawString(value, 160, y + 43);
  M5.Display.clearClipRect();
}

void drawEmotionRecordsRow(int y, const char* label, const String& value) {
  uint16_t fill = emotionPanel((y / 21 & 1) ? 10 : 14);
  M5.Display.fillRoundRect(8, y, 304, 19, 5, fill);
  M5.Display.drawRoundRect(8, y, 304, 19, 5, emotionTheme(30));
  useUIFont(1);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(emotionTheme(72), fill);
  M5.Display.drawString(label, 15, y + 10);
  M5.Display.setClipRect(136, y + 1, 168, 17);
  M5.Display.setTextDatum(middle_right);
  M5.Display.setTextColor(TFT_WHITE, fill);
  M5.Display.drawString(value, 303, y + 10);
  M5.Display.clearClipRect();
}

void drawEmotionRecords() {
  if (screenNow != Screen::EmotionRecords) return;
  drawEmotionMatrixBackground();
  drawEmotionConnectionIndicator();
  useUIFont(1);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(emotionTheme(100), TFT_BLACK);
  M5.Display.drawString("我的紀錄 " + String(emotionRecordsPage + 1) + "/2", 20, 5);

  if (emotionRecordsState == EmotionRecordsState::Loading) {
    useUIMediumFont();
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("讀取統計中…", 160, 101);
    useUIFont(1);
    M5.Display.setTextColor(emotionTheme(62), TFT_BLACK);
    M5.Display.drawString("正在同步資料庫紀錄", 160, 139);
  } else if (emotionRecordsState == EmotionRecordsState::Error) {
    M5.Display.fillRoundRect(14, 55, 292, 114, 12, emotionPanel(12));
    M5.Display.drawRoundRect(14, 55, 292, 114, 12, TFT_RED);
    useUIMediumFont();
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, emotionPanel(12));
    M5.Display.drawString("無法讀取紀錄", 160, 88);
    useUIFont(1);
    M5.Display.setTextColor(0xF986, emotionPanel(12));
    M5.Display.setClipRect(24, 111, 272, 45);
    M5.Display.drawString(emotionRecordsError, 160, 132);
    M5.Display.clearClipRect();
  } else {
    if (!emotionRecordsPage) {
      drawEmotionBigRow(0, "學習天數", String(emotionRecordsLearningDays) + " 天");
      drawEmotionBigRow(1, "填寫張數", String(emotionRecordsSheetCount) + " 張");
      drawEmotionBigRow(2, "今日張數", String(emotionRecordsTodaySheets) + " 張");
      drawEmotionBigRow(3, "平均一天", emotionRecordsAverage + " 張");
    } else {
      drawEmotionBigRow(0, "最常身體反應", emotionRecordsTopBody);
      drawEmotionBigRow(1, "最常出現情緒", emotionRecordsTopEmotion);
      drawEmotionBigRow(2, "最強烈的情緒", emotionRecordsStrongest);
      drawEmotionBigRow(3, "最常情緒落地", emotionRecordsTopGrounding);
    }
  }
  drawEmotionBottomBar(emotionRecordsPage ? "上一頁" : "重新整理", emotionRecordsPage ? "" : "下一頁", "返回");
}

void drawEmotionObservation() {
  if (screenNow != Screen::EmotionObservation) return;
  drawEmotionMatrixBackground();
  const uint16_t panel = emotionPanel(12);
  const uint16_t selected = emotionPanel(22);
  const uint16_t border = emotionTheme(55);
  const uint16_t accent = emotionTheme(100);
  useUIFont(1);
  M5.Display.setTextColor(accent, TFT_BLACK);
  M5.Display.setTextDatum(top_left);
  drawEmotionConnectionIndicator();
  M5.Display.drawString(emotionTitleText(), 20, 5);
  drawEmotionResetIcon();

  if (!emotionApiConfigured()) {
    M5.Display.fillRoundRect(17, 48, 286, 137, 14, panel);
    M5.Display.drawRoundRect(17, 48, 286, 137, 14, TFT_RED);
    useUIMediumFont(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(TFT_WHITE, panel);
    M5.Display.drawString(emotionApiState == EmotionApiState::Checking ? "正在確認資料庫…" : "資料庫尚未連線", 160, 84);
    useUIFont(1); M5.Display.setTextColor(emotionTheme(80), panel);
    M5.Display.drawString(WiFi.status() != WL_CONNECTED ? "請先連接 Wi-Fi" : "請先從網頁設定登入 API", 160, 126);
    M5.Display.setTextColor(emotionTheme(55), panel);
    M5.Display.drawString("連線成功後才可開始填寫", 160, 156);
    drawEmotionBottomBar(emotionFormPage == 0 ? "我的紀錄" : "", "", "取消");
    return;
  }

  if (emotionFormPage == 0) {
    const char* labels[] = {"年", "月", "日", "時", "分"};
    int values[] = {emotionFormTime.date.year, emotionFormTime.date.month, emotionFormTime.date.date,
                    emotionFormTime.time.hours, emotionFormTime.time.minutes};
    for (int i = 0; i < 5; ++i) {
      int x = 5 + i * 63;
      M5.Display.fillRoundRect(x, 48, 58, 111, 8, panel);
      M5.Display.drawRoundRect(x, 48, 58, 111, 8, border);
      useUIFont(1); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(emotionTheme(65), panel);
      M5.Display.drawString(labels[i], x + 29, 67);
      M5.Display.fillTriangle(x + 25, 83, x + 33, 83, x + 29, 77, accent);
      M5.Display.setTextColor(TFT_WHITE, panel);
      char value[6];
      if (i == 0) snprintf(value, sizeof(value), "%04d", values[i]);
      else snprintf(value, sizeof(value), "%02d", values[i]);
      M5.Display.drawString(value, x + 29, 107);
      M5.Display.fillTriangle(x + 25, 133, x + 33, 133, x + 29, 139, accent);
    }
    useUIFont(1); M5.Display.setTextColor(emotionTheme(62), TFT_BLACK); M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("點上半部後退，點下半部增加", 160, 183);
  } else if (emotionFormPage == 1) {
    for (int i = 0; i < 8; ++i) {
      int x = (i % 2) ? 164 : 10, y = 39 + (i / 2) * 41;
      uint16_t fill = emotionTriggers[i] ? emotionPanel(30) : panel;
      M5.Display.fillRoundRect(x, y, 146, 35, 7, fill);
      M5.Display.drawRoundRect(x, y, 146, 35, 7, emotionTriggers[i] ? accent : border);
      useUIFont(1); M5.Display.setTextColor(emotionTriggers[i] ? accent : TFT_WHITE, fill); M5.Display.setTextDatum(middle_center);
      M5.Display.drawString(String(emotionTriggers[i] ? "✓ " : "") + EMOTION_TRIGGERS[i], x + 73, y + 18);
    }
  } else if (emotionFormPage == 2) {
    const char* labels[] = {"心律", "呼吸", "出汗", "身體訊號", "身體部位", "行為線索"};
    String values[] = {EMOTION_HEART_RATES[emotionHeartRate], EMOTION_BREATH_RATES[emotionBreathRate],
      EMOTION_SWEATING[emotionSweating], emotionBodySignal < 0 ? "不填" : EMOTION_BODY_SIGNALS[(uint8_t)emotionBodySignal],
      emotionBodyPart < 0 ? "不填" : EMOTION_BODY_PARTS[(uint8_t)emotionBodyPart], EMOTION_BEHAVIOR_CUES[emotionBehaviorCue]};
    for (int i = 0; i < 6; ++i) {
      int x = 5 + (i % 3) * 105, y = 41 + (i / 3) * 79;
      M5.Display.fillRoundRect(x, y, 100, 72, 8, panel);
      M5.Display.drawRoundRect(x, y, 100, 72, 8, border);
      useUIFont(1); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(emotionTheme(62), panel);
      M5.Display.drawString(labels[i], x + 50, y + 17);
      M5.Display.setTextColor(TFT_WHITE, panel);
      M5.Display.drawString(values[i], x + 50, y + 47);
    }
  } else if (emotionFormPage == 3) {
    String choiceText = emotionChoice < 0 ? "未選" : EMOTION_CHOICES[emotionCategory][emotionChoice];
    drawEmotionChoicePanel(46, "類別", EMOTION_CATEGORIES[emotionCategory], selected, accent);
    drawEmotionChoicePanel(118, "情緒", choiceText, selected, accent);
    if (emotionSubmitMessage.length()) {
      useUIFont(1); M5.Display.setTextColor(TFT_RED, TFT_BLACK);
      M5.Display.drawString(emotionSubmitMessage, 160, 198);
    }
  } else if (emotionFormPage == 4) {
    M5.Display.fillRoundRect(16, 46, 288, 139, 12, panel);
    M5.Display.drawRoundRect(16, 46, 288, 139, 12, border);
    useUILargeFont(); M5.Display.setTextColor(TFT_WHITE, panel); M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(String(emotionIndexPercent) + "%", 160, 88);
    const int sliderLeft = 28, sliderRight = 292, sliderY = 151;
    int thumbX = map(emotionIndexPercent, 5, 120, sliderLeft, sliderRight);
    M5.Display.fillRoundRect(sliderLeft, sliderY - 5, sliderRight - sliderLeft, 10, 5, emotionTheme(20));
    M5.Display.fillRoundRect(sliderLeft, sliderY - 5, thumbX - sliderLeft, 10, 5, accent);
    M5.Display.fillCircle(thumbX, sliderY, 12, TFT_WHITE); M5.Display.drawCircle(thumbX, sliderY, 12, accent);
    useUIFont(1); M5.Display.setTextColor(emotionTheme(65), panel);
    M5.Display.drawString("5", sliderLeft, 181); M5.Display.drawString("120", sliderRight, 181);
  } else if (emotionFormPage == 5) {
    M5.Display.fillRoundRect(12, 48, 296, 61, 10, selected); M5.Display.drawRoundRect(12, 48, 296, 61, 10, accent);
    M5.Display.fillRoundRect(12, 120, 296, 61, 10, selected); M5.Display.drawRoundRect(12, 120, 296, 61, 10, accent);
    useUIFont(1); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(emotionTheme(70), selected);
    M5.Display.drawString("觀察次數　◀ / ▶", 160, 63); M5.Display.drawString("觀察時長　◀ / ▶", 160, 135);
    useUIMediumFont(); M5.Display.setTextColor(TFT_WHITE, selected);
    M5.Display.drawString(String(emotionObserveCount) + " 次", 160, 89);
    M5.Display.drawString(String(emotionObserveMinutes) + " 分鐘", 160, 161);
  } else if (emotionFormPage == 6) {
    M5.Display.fillRoundRect(12, 48, 296, 61, 10, selected); M5.Display.drawRoundRect(12, 48, 296, 61, 10, accent);
    M5.Display.fillRoundRect(12, 120, 296, 61, 10, selected); M5.Display.drawRoundRect(12, 120, 296, 61, 10, accent);
    useUIFont(1); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(emotionTheme(70), selected);
    M5.Display.drawString("落地時機　◀ / ▶", 160, 63); M5.Display.drawString("落地方式　◀ / ▶", 160, 135);
    useUIMediumFont(); M5.Display.setTextColor(TFT_WHITE, selected);
    M5.Display.drawString(EMOTION_GROUNDING_TIMES[emotionGroundingTiming], 160, 89);
    M5.Display.drawString(EMOTION_GROUNDING_ACTIONS[emotionGroundingAction], 160, 161);
  } else {
    M5.Display.fillRoundRect(7, 34, 306, 174, 10, panel);
    M5.Display.drawRoundRect(7, 34, 306, 174, 10, border);
    useUIFont(1); M5.Display.setTextColor(TFT_WHITE, panel); M5.Display.setTextDatum(top_left);
    String triggerSummary;
    for (int i = 0; i < 8; ++i) if (emotionTriggers[i]) { if (triggerSummary.length()) triggerSummary += "、"; triggerSummary += EMOTION_TRIGGERS[i]; }
    if (!triggerSummary.length()) triggerSummary = "未選";
    const char* selectedEmotion = emotionChoice < 0 ? "未選" : EMOTION_CHOICES[emotionCategory][emotionChoice];
    M5.Display.drawString("時間  " + emotionLocalTimeText(emotionFormTime), 13, 39);
    M5.Display.drawString("觸發  " + triggerSummary, 13, 63);
    M5.Display.drawString("反應  " + String(EMOTION_HEART_RATES[emotionHeartRate]) + " / " + String(EMOTION_BREATH_RATES[emotionBreathRate]), 13, 87);
    M5.Display.drawString("情緒  " + String(EMOTION_CATEGORIES[emotionCategory]) + " · " + selectedEmotion + " " + String(emotionIndexPercent) + "%", 13, 111);
    M5.Display.drawString("觀察  " + String(emotionObserveCount) + "次 / " + String(emotionObserveMinutes) + "分鐘", 13, 135);
    M5.Display.drawString("落地  " + String(EMOTION_GROUNDING_TIMES[emotionGroundingTiming]) + " · " + EMOTION_GROUNDING_ACTIONS[emotionGroundingAction], 13, 159);
    M5.Display.setTextColor(emotionSubmitMessage.length() ? accent : emotionTheme(62), panel);
    String prompt = emotionSubmitMessage.length() ? emotionSubmitMessage :
      (emotionSubmitArmed ? "請再次確認送出；左鍵可取消" : "檢查內容後按中鍵送出");
    M5.Display.drawString(prompt, 13, 184);
  }
  if (emotionFormPage == 0) drawEmotionBottomBar("我的紀錄", "下一頁", "取消");
  else if (emotionFormPage < 7) drawEmotionBottomBar("上一頁", "下一頁", "取消");
  else if (emotionSubmitCompleted) drawEmotionBottomBar("撤回", "已送出", "取消");
  else if (emotionSubmitArmed) drawEmotionBottomBar("取消送出", "確認送出", "取消");
  else drawEmotionBottomBar("上一頁", "送出", "取消");
}

void resetEmotionPage(uint8_t page) {
  if (page == 0) {
    getClockDateTime(&emotionFormTime);
  } else if (page == 1) {
    memset(emotionTriggers, 0, sizeof(emotionTriggers));
  } else if (page == 2) {
    emotionHeartRate = 4; emotionBreathRate = 4; emotionSweating = 0;
    emotionBodySignal = -1; emotionBodyPart = -1; emotionBehaviorCue = 0;
  } else if (page == 3) {
    emotionCategory = 0; emotionChoice = -1;
  } else if (page == 4) {
    emotionIndexPercent = 50;
  } else if (page == 5) {
    emotionObserveCount = 1; emotionObserveMinutes = 1;
  } else if (page == 6) {
    emotionGroundingTiming = 1; emotionGroundingAction = 10;
  }
  emotionSubmitArmed = false;
  emotionSubmitMessage = "";
}

void adjustEmotionTimeField(uint8_t field, int direction) {
  direction = direction >= 0 ? 1 : -1;
  if (field == 0) {
    int year = emotionFormTime.date.year + direction;
    emotionFormTime.date.year = year > 2099 ? 2020 : (year < 2020 ? 2099 : year);
  } else if (field == 1) {
    int month = emotionFormTime.date.month + direction;
    emotionFormTime.date.month = month > 12 ? 1 : (month < 1 ? 12 : month);
  } else if (field == 2) {
    int lastDay = emotionDaysInMonth(emotionFormTime.date.year, emotionFormTime.date.month);
    int day = emotionFormTime.date.date + direction;
    emotionFormTime.date.date = day > lastDay ? 1 : (day < 1 ? lastDay : day);
  } else if (field == 3) {
    int hour = emotionFormTime.time.hours + direction;
    emotionFormTime.time.hours = hour > 23 ? 0 : (hour < 0 ? 23 : hour);
  } else {
    int minute = emotionFormTime.time.minutes + direction;
    emotionFormTime.time.minutes = minute > 59 ? 0 : (minute < 0 ? 59 : minute);
  }
  emotionFormTime.date.date = min<int>((int)emotionFormTime.date.date,
    (int)emotionDaysInMonth(emotionFormTime.date.year, emotionFormTime.date.month));
}

void showEmotionObservation(bool newEntry = false) {
  screenNow = Screen::EmotionObservation;
  if (newEntry) {
    emotionFormPage = 0;
    emotionSubmitArmed = false;
    emotionSubmitCompleted = false;
    emotionSubmitMessage = "";
    emotionLastSubmittedId = "";
    emotionLastSubmittedPayload = "";
    for (uint8_t page = 0; page < 7; ++page) resetEmotionPage(page);
  }
  drawEmotionObservation();
}

String emotionReminderTimeText(uint16_t minutes) {
  if (minutes == 0xFFFF) return "關閉";
  char value[6]; snprintf(value, sizeof(value), "%02u:%02u", minutes / 60, minutes % 60);
  return String(value);
}

void showEmotionSettings() {
  screenNow = Screen::EmotionSettings;
  drawEmotionMatrixBackground();
  drawEmotionConnectionIndicator();
  static const char* pageTitles[] = {"填寫提醒", "提醒方式", "提醒音量"};
  useUIFont(1); M5.Display.setTextDatum(top_left); M5.Display.setTextColor(emotionTheme(100), TFT_BLACK);
  M5.Display.drawString("情緒觀察設定 " + String(emotionSettingsPage + 1) + "/3 · " + pageTitles[emotionSettingsPage], 20, 5);
  if (emotionSettingsPage == 0) {
    const char* modes[] = {"關閉", "整點提醒", "固定鬧鐘"};
    drawEmotionBigRow(0, "提醒模式", modes[emotionReminderMode], true);
    if (emotionReminderMode == 1) {
      drawEmotionBigRow(1, "開始時間", emotionReminderTimeText(emotionReminderWindowStart), true);
      drawEmotionBigRow(2, "結束時間", emotionReminderTimeText(emotionReminderWindowEnd), true);
      drawEmotionBigRow(3, "提醒間隔", String(emotionReminderIntervalMinutes) + " 分鐘", true);
    } else if (emotionReminderMode == 2) {
      drawEmotionBigRow(1, "固定時間 1", emotionReminderTimeText(emotionReminderTimes[0]), true);
      drawEmotionBigRow(2, "固定時間 2", emotionReminderTimeText(emotionReminderTimes[1]), true);
      drawEmotionBigRow(3, "固定時間 3", emotionReminderTimeText(emotionReminderTimes[2]), true);
    }
  } else if (emotionSettingsPage == 1) {
    const char* sounds[] = {"打版", "磬聲", "流水聲", "水滴聲"};
    drawEmotionBigRow(0, "振動", emotionReminderVibration ? "開" : "關", true);
    drawEmotionBigRow(1, "鬧鐘", emotionReminderSound ? "開" : "關", true);
    drawEmotionBigRow(2, "提醒時間", String(emotionReminderDurationSeconds) + " 秒", true);
    drawEmotionBigRow(3, "鬧鐘鈴聲", sounds[emotionReminderSoundChoice], true);
  } else {
    drawEmotionBigRow(0, "音量", String(emotionReminderVolume) + "%", true);
  }
  drawEmotionBottomBar(emotionSettingsPage ? "上一頁" : "", emotionSettingsPage < 2 ? "下一頁" : "", "完成");
}

String emotionUuid() {
  uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
  char value[37];
  snprintf(value, sizeof(value), "%08lx-%04lx-4%03lx-%04lx-%08lx%04lx", (unsigned long)a,
    (unsigned long)(b >> 16), (unsigned long)(c & 0x0FFF), (unsigned long)((d >> 16 & 0x3FFF) | 0x8000),
    (unsigned long)(d & 0xFFFFFFFFUL), (unsigned long)(b & 0xFFFF));
  return String(value);
}

String normalizeEmotionApiBase(String base) {
  base.trim();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!base.startsWith("https://") || base.indexOf('#') >= 0 || base.indexOf('?') >= 0) return "";
  return base;
}

String emotionApiHost(const String& base) {
  int start = base.indexOf("://");
  if (start < 0) return "";
  start += 3;
  int end = base.indexOf('/', start);
  String authority = end < 0 ? base.substring(start) : base.substring(start, end);
  if (authority.startsWith("[")) {
    int close = authority.indexOf(']');
    return close > 0 ? authority.substring(1, close) : "";
  }
  int colon = authority.indexOf(':');
  if (colon >= 0) authority.remove(colon);
  return authority;
}

bool emotionApiLogin(const String& identity, const String& password, String& message) {
  emotionApiState = EmotionApiState::Checking;
  auto fail = [&](const String& reason) {
    message = reason; emotionApiState = EmotionApiState::Disconnected; emotionApiLastChecked = millis(); return false;
  };
  if (WiFi.status() != WL_CONNECTED) return fail("請先連接 Wi-Fi，再登入 API。");
  String base = normalizeEmotionApiBase(emotionApiBase);
  if (!base.length()) return fail("API 網址必須使用 https://，請先儲存有效網址。");

  String host = emotionApiHost(base);
  IPAddress resolvedAddress;
  if (!host.length() || WiFi.hostByName(host.c_str(), resolvedAddress) != 1) {
    return fail("找不到 API 主機（DNS 解析失敗）：" + host + "。請確認設備 Wi-Fi 可連上網際網路及 DNS。");
  }
  if (time(nullptr) < 1700000000) {
    syncTime();
    if (time(nullptr) < 1700000000) {
      return fail("設備時間尚未同步，無法安全驗證 HTTPS 憑證。請確認網路可連外後重試。");
    }
  }

  DynamicJsonDocument requestDoc(512);
  requestDoc["identity"] = identity;
  requestDoc["password"] = password;
  String body; serializeJson(requestDoc, body);
  WiFiClientSecure secure;
  secure.setCACert(EMOTION_API_ROOT_CA);
  secure.setHandshakeTimeout(15);
  secure.setTimeout(15);
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(secure, base + "/api/collections/users/auth-with-password")) return fail("無法建立 HTTPS 連線。");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  char tlsError[128] = {};
  int tlsErrorCode = code < 0 ? secure.lastError(tlsError, sizeof(tlsError)) : 0;
  uint32_t freeHeap = ESP.getFreeHeap();
  http.end(); secure.stop();
  body = "";
  if (code != 200) {
    if (code < 0 && tlsErrorCode != 0) {
      String detail = String(tlsError);
      if (detail.indexOf("certificate verification failed") >= 0 || tlsErrorCode == -9984) {
        message = "TLS 憑證驗證失敗；請確認設備日期時間正確，或伺服器憑證鏈是否完整。";
      } else if (detail.indexOf("alloc") >= 0 || detail.indexOf("memory") >= 0) {
        message = "TLS 記憶體不足，請先返回時鐘頁、重開設備後再試（可用 RAM " + String(freeHeap) + " bytes）。";
      } else {
        message = "TLS 握手失敗：" + detail + "（" + String(tlsErrorCode) + "；可用 RAM " + String(freeHeap) + " bytes）。";
      }
    } else if (code == -1) {
      message = "DNS 已解析為 " + resolvedAddress.toString() + "，但 HTTPS 連線失敗（-1）。請確認網路允許連出 TCP 443；可用 RAM " + String(freeHeap) + " bytes。";
    } else if (code < 0) {
      message = "HTTPS 請求失敗（" + String(code) + "），資料未送出。";
    } else {
      DynamicJsonDocument errorDoc(512);
      String serverMessage;
      if (!deserializeJson(errorDoc, response) && errorDoc["message"].is<const char*>()) {
        serverMessage = errorDoc["message"].as<String>();
      }
      if (code == 400 || code == 401) {
        message = "API 拒絕登入（HTTP " + String(code) + "），請檢查 Email 與密碼。";
      } else {
        message = "API 回應 HTTP " + String(code) + (serverMessage.length() ? "：" + serverMessage : "。請確認 API 網址及服務狀態。");
      }
    }
    emotionApiState = EmotionApiState::Disconnected;
    emotionApiLastChecked = millis();
    return false;
  }
  DynamicJsonDocument responseDoc(4096);
  if (deserializeJson(responseDoc, response) || !responseDoc["token"].is<const char*>() || !responseDoc["record"]["id"].is<const char*>()) {
    return fail("伺服器回應格式不符 PocketBase 規格。");
  }
  emotionApiToken = responseDoc["token"].as<String>();
  emotionApiUserId = responseDoc["record"]["id"].as<String>();
  emotionApiIdentity = identity;
  prefs.begin("spaceclock", false);
  prefs.putString("emoIdent", emotionApiIdentity);
  prefs.putString("emoUser", emotionApiUserId);
  prefs.putString("emoToken", emotionApiToken);
  prefs.end();
  emotionApiState = EmotionApiState::Connected;
  emotionApiLastChecked = millis();
  message = "API 登入成功；帳號密碼未儲存，token 僅保存在本機設備。";
  return true;
}

bool refreshEmotionApiToken() {
  if (!emotionApiToken.length() || !emotionApiUserId.length() || WiFi.status() != WL_CONNECTED) {
    emotionApiState = EmotionApiState::Disconnected;
    return false;
  }
  String base = normalizeEmotionApiBase(emotionApiBase);
  if (!base.length()) { emotionApiState = EmotionApiState::Disconnected; return false; }
  WiFiClientSecure secure; secure.setCACert(EMOTION_API_ROOT_CA);
  HTTPClient http; http.setTimeout(9000);
  if (!http.begin(secure, base + "/api/collections/users/auth-refresh")) { emotionApiState = EmotionApiState::Disconnected; return false; }
  http.addHeader("Authorization", "Bearer " + emotionApiToken);
  int code = http.POST("");
  String response = code > 0 ? http.getString() : "";
  http.end(); secure.stop();
  if (code != 200) { emotionApiState = EmotionApiState::Disconnected; return false; }
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, response) || !doc["token"].is<const char*>()) { emotionApiState = EmotionApiState::Disconnected; return false; }
  emotionApiToken = doc["token"].as<String>();
  if (doc["record"]["id"].is<const char*>()) emotionApiUserId = doc["record"]["id"].as<String>();
  prefs.begin("spaceclock", false); prefs.putString("emoToken", emotionApiToken); prefs.putString("emoUser", emotionApiUserId); prefs.end();
  emotionApiState = EmotionApiState::Connected;
  return true;
}

bool verifyEmotionApiConnection(bool force) {
  uint32_t nowMs = millis();
  if (!force && emotionApiState == EmotionApiState::Connected && nowMs - emotionApiLastChecked < 60000UL) return true;
  if (WiFi.status() != WL_CONNECTED || !emotionApiToken.length() || !emotionApiUserId.length()) {
    emotionApiState = EmotionApiState::Disconnected;
    emotionApiLastChecked = nowMs;
    return false;
  }
  emotionApiState = EmotionApiState::Checking;
  if (screenNow == Screen::EmotionObservation) drawEmotionObservation();
  else if (screenNow == Screen::EmotionRecords) drawEmotionRecords();
  bool connected = refreshEmotionApiToken();
  emotionApiState = connected ? EmotionApiState::Connected : EmotionApiState::Disconnected;
  emotionApiLastChecked = millis();
  return connected;
}

bool saveEmotionQueue(const String& localId, const String& body) {
  if (!emotionStorageReady || !emotionApiUserId.length() || body.length() > 8192) return false;
  DynamicJsonDocument queue(32768);
  File input = SPIFFS.open("/emotion_queue.json", FILE_READ);
  if (input) { deserializeJson(queue, input); input.close(); }
  JsonArray items = queue.is<JsonArray>() ? queue.as<JsonArray>() : queue.to<JsonArray>();
  if (items.size() >= 20) return false;
  JsonObject item = items.createNestedObject();
  item["local_id"] = localId;
  item["user"] = emotionApiUserId;
  item["body"] = body;
  File output = SPIFFS.open("/emotion_queue.json", FILE_WRITE);
  if (!output) return false;
  bool ok = serializeJson(queue, output) == measureJson(queue);
  output.close();
  emotionPendingCount = items.size();
  return ok;
}

uint8_t countEmotionQueue() {
  if (!emotionStorageReady) return 0;
  File input = SPIFFS.open("/emotion_queue.json", FILE_READ);
  if (!input) return 0;
  DynamicJsonDocument queue(32768);
  deserializeJson(queue, input);
  input.close();
  return queue.is<JsonArray>() ? (uint8_t)min<size_t>(queue.size(), 255) : 0;
}

bool loadEmotionStatsCache(DynamicJsonDocument& result) {
  if (!emotionStorageReady) return false;
  File cacheFile = SPIFFS.open("/emotion_stats.json", FILE_READ);
  if (!cacheFile) return false;
  DynamicJsonDocument stored(4096);
  DeserializationError err = deserializeJson(stored, cacheFile);
  cacheFile.close();
  if (err || String((const char*)(stored["user"] | "")) != emotionApiUserId) return false;
  result.set(stored["data"]);
  return result["data"]["summary"].is<JsonObject>();
}

void applyEmotionStatistics(DynamicJsonDocument& stats) {
  JsonObject summary = stats["data"]["summary"].as<JsonObject>();
  emotionRecordsSheetCount = summary["filledSheets"] | 0UL;
  emotionRecordsTodaySheets = summary["todaySheets"] | 0UL;
  emotionRecordsLearningDays = summary["learningDays"] | 0;
  char average[16];
  snprintf(average, sizeof(average), "%.1f", summary["averagePerDay"] | 0.0);
  emotionRecordsAverage = average;
  const char* topBody = summary["mostCommonBodyReaction"]["name"] | "-";
  const char* topEmotion = summary["mostCommonEmotion"]["name"] | "-";
  const char* strongest = summary["strongestEmotion"]["emotion"] | "-";
  const char* topGrounding = summary["mostCommonGrounding"]["name"] | "-";
  emotionRecordsTopBody = topBody && topBody[0] ? topBody : "-";
  emotionRecordsTopEmotion = topEmotion && topEmotion[0] ? topEmotion : "-";
  emotionRecordsTopGrounding = topGrounding && topGrounding[0] ? topGrounding : "-";
  emotionRecordsStrongest = strongest && strongest[0] ? strongest : "-";
  int strongestIndex = summary["strongestEmotion"]["index"] | -1;
  if (emotionRecordsStrongest != "-" && strongestIndex >= 0) emotionRecordsStrongest += " " + String(strongestIndex) + "%";
}

bool flushOneEmotionRecord() {
  if (!emotionStorageReady || WiFi.status() != WL_CONNECTED || !emotionApiConfigured() || millis() - emotionLastQueueSyncAt < 5000UL) return false;
  emotionLastQueueSyncAt = millis();
  File input = SPIFFS.open("/emotion_queue.json", FILE_READ);
  if (!input) { emotionPendingCount = 0; return false; }
  DynamicJsonDocument queue(32768);
  if (deserializeJson(queue, input) || !queue.is<JsonArray>() || queue.size() == 0) { input.close(); return false; }
  input.close();
  JsonArray items = queue.as<JsonArray>();
  if (String((const char*)(items[0]["user"] | "")) != emotionApiUserId) return false;
  String body = items[0]["body"] | "";
  if (!body.length()) return false;
  String base = normalizeEmotionApiBase(emotionApiBase);
  WiFiClientSecure secure; secure.setCACert(EMOTION_API_ROOT_CA);
  HTTPClient http; http.setTimeout(8000);
  if (!base.length() || !http.begin(secure, base + "/api/collections/entries/records")) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + emotionApiToken);
  int code = http.POST(body);
  if (code == 401) {
    http.end(); secure.stop();
    if (!refreshEmotionApiToken()) return false;
    WiFiClientSecure retrySecure; retrySecure.setCACert(EMOTION_API_ROOT_CA);
    HTTPClient retry; retry.setTimeout(8000);
    if (!retry.begin(retrySecure, base + "/api/collections/entries/records")) return false;
    retry.addHeader("Content-Type", "application/json"); retry.addHeader("Authorization", "Bearer " + emotionApiToken);
    code = retry.POST(body); retry.end(); retrySecure.stop();
  } else { http.end(); secure.stop(); }
  if (code != 200 && code != 201) return false;
  queue.as<JsonArray>().remove(0);
  File output = SPIFFS.open("/emotion_queue.json", FILE_WRITE);
  if (!output) return false;
  serializeJson(queue, output); output.close();
  emotionPendingCount = queue.size();
  emotionApiState = EmotionApiState::Connected;
  emotionApiLastChecked = millis();
  return true;
}

// Long-press the middle button: reconnect to the database right away and
// upload every record waiting in the offline queue.
void drawEmotionSyncToast(const String& message) {
  // Overlay just above the bottom bar so it works on every emotion page.
  M5.Display.fillRoundRect(10, 172, 300, 34, 10, emotionPanel(18));
  M5.Display.drawRoundRect(10, 172, 300, 34, 10, emotionTheme(80));
  useUIFont(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, emotionPanel(18));
  M5.Display.setClipRect(14, 174, 292, 30);
  M5.Display.drawString(message, 160, 189);
  M5.Display.clearClipRect();
}

// Long-press the middle button: reconnect to the database right away and
// upload every record waiting in the offline queue.
void forceEmotionSync() {
  if (WiFi.status() != WL_CONNECTED) { drawEmotionSyncToast("Wi-Fi 未連線，無法同步"); return; }
  drawEmotionSyncToast("重新連線資料庫並上傳離線資料…");
  emotionApiState = EmotionApiState::Unknown;
  bool connected = verifyEmotionApiConnection(true);
  if (!connected && refreshEmotionApiToken()) connected = verifyEmotionApiConnection(true);
  uint8_t before = emotionStorageReady ? countEmotionQueue() : 0;
  emotionPendingCount = before;
  uint8_t uploaded = 0;
  while (connected && emotionPendingCount) {
    emotionLastQueueSyncAt = 0;  // bypass the 5 s background pacing
    if (!flushOneEmotionRecord()) break;
    ++uploaded;
  }
  String result;
  if (!connected) result = "資料庫連線失敗，請檢查登入或網路";
  else if (!emotionStorageReady) result = "已連線；本機儲存未啟用，無離線資料";
  else if (!before) result = "已重新連線，沒有待上傳的離線資料";
  else result = "已連線，上傳 " + String(uploaded) + " 筆，剩 " + String(emotionPendingCount) + " 筆";
  Serial.printf("[emotion] forced sync: connected=%d uploaded=%u remaining=%u\n", connected ? 1 : 0, uploaded, emotionPendingCount);
  if (screenNow == Screen::EmotionRecords) { emotionStatsCacheAt = 0; showEmotionRecords(true); }
  else drawEmotionObservation();
  drawEmotionSyncToast(result);
}

int fetchEmotionStatistics(DynamicJsonDocument& result, String& errorBody) {
  String base = normalizeEmotionApiBase(emotionApiBase);
  if (!base.length()) return -10001;

  WiFiClientSecure secure;
  secure.setCACert(EMOTION_API_ROOT_CA);
  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(secure, base + "/api/statistics?period=all&includeDeleted=false")) return -10001;
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + emotionApiToken);
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument filter(1024);
    JsonObject summary = filter["data"].createNestedObject("summary");
    summary["learningDays"] = true;
    summary["filledSheets"] = true;
    summary["todaySheets"] = true;
    summary["averagePerDay"] = true;
    summary["mostCommonBodyReaction"]["name"] = true;
    summary["mostCommonEmotion"]["name"] = true;
    summary["strongestEmotion"]["emotion"] = true;
    summary["strongestEmotion"]["index"] = true;
    summary["mostCommonGrounding"]["name"] = true;
    String response = http.getString();
    DeserializationError parseError = deserializeJson(result, response, DeserializationOption::Filter(filter));
    if (parseError) {
      errorBody = parseError.c_str();
      code = -10002;
    }
  } else if (code > 0) {
    errorBody = http.getString();
  }
  http.end();
  secure.stop();
  return code;
}

bool loadEmotionRecordStats() {
  emotionRecordsState = EmotionRecordsState::Loading;
  emotionRecordsError = "";
  DynamicJsonDocument cached(4096);
  bool haveCache = loadEmotionStatsCache(cached);
  if (haveCache) {
    applyEmotionStatistics(cached);
    emotionRecordsState = EmotionRecordsState::Ready;
    emotionPendingCount = countEmotionQueue();
    // The cache is only a fallback for offline use. Always fetch fresh
    // statistics when online: treating a cache loaded at boot as "fresh"
    // made two devices show different, stale numbers (and made Refresh a no-op).
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (haveCache) return true;
    emotionRecordsError = "離線且尚無快取資料";
    emotionRecordsState = EmotionRecordsState::Error;
    return false;
  }
  if (!emotionApiToken.length() || !emotionApiUserId.length()) {
    if (haveCache) return true;
    emotionRecordsError = "請先從網頁登入情緒觀察 API";
    emotionRecordsState = EmotionRecordsState::Error;
    return false;
  }
  DynamicJsonDocument stats(3072);
  String errorBody;
  int code = fetchEmotionStatistics(stats, errorBody);
  if (code == 401 && refreshEmotionApiToken()) {
    stats.clear();
    errorBody = "";
    code = fetchEmotionStatistics(stats, errorBody);
  }
  if (code != 200) {
    if (haveCache) { emotionApiState = EmotionApiState::Disconnected; return true; }
    if (code == 401) emotionRecordsError = "登入已過期，請從網頁重新登入";
    else if (code == -10002) emotionRecordsError = "統計資料格式解析失敗";
    else if (code < 0) emotionRecordsError = "HTTPS／網路連線失敗";
    else emotionRecordsError = "統計 API 回應 HTTP " + String(code);
    emotionRecordsState = EmotionRecordsState::Error;
    if (code == 401) emotionApiState = EmotionApiState::Disconnected;
    return false;
  }

  JsonObject summary = stats["data"]["summary"].as<JsonObject>();
  if (summary.isNull()) {
    emotionRecordsError = "統計 API 缺少 summary 欄位";
    emotionRecordsState = EmotionRecordsState::Error;
    return false;
  }
  applyEmotionStatistics(stats);
  if (emotionStorageReady) {
    DynamicJsonDocument stored(4096);
    stored["user"] = emotionApiUserId;
    stored["data"] = stats;
    File cache = SPIFFS.open("/emotion_stats.json", FILE_WRITE);
    if (cache) { serializeJson(stored, cache); cache.close(); }
  }
  emotionStatsCacheAt = millis();

  emotionApiState = EmotionApiState::Connected;
  emotionApiLastChecked = millis();
  emotionRecordsState = EmotionRecordsState::Ready;
  return true;
}

void showEmotionRecords(bool refresh) {
  emotionRecordsPage = 0;
  screenNow = Screen::EmotionRecords;
  if (refresh) {
    DynamicJsonDocument cached(4096);
    if (loadEmotionStatsCache(cached)) {
      applyEmotionStatistics(cached);
      emotionRecordsState = EmotionRecordsState::Ready;
      emotionPendingCount = countEmotionQueue();
    } else emotionRecordsState = EmotionRecordsState::Loading;
  }
  drawEmotionRecords();
  if (refresh) {
    loadEmotionRecordStats();
    drawEmotionRecords();
  }
}

bool submitEmotionObservation() {
  if (!emotionApiConfigured()) { emotionSubmitMessage = "請先從網頁登入情緒觀察 API。"; drawEmotionObservation(); return false; }
  time_t now = time(nullptr); struct tm utcNow;
  bool hasUtc = now >= 1700000000 && gmtime_r(&now, &utcNow);
  if (!hasUtc && WiFi.status() == WL_CONNECTED) { emotionSubmitMessage = "設備時間尚未同步，請連線後重試。"; drawEmotionObservation(); return false; }
  emotionSubmitMessage = "送出中，請稍候…"; drawEmotionObservation();
  DynamicJsonDocument doc(6144);
  doc["user"] = emotionApiUserId;
  doc["local_id"] = emotionUuid();
  doc["deleted"] = false;
  if (hasUtc) {
    char updatedAt[32]; strftime(updatedAt, sizeof(updatedAt), "%Y-%m-%dT%H:%M:%S.000Z", &utcNow);
    doc["client_updated_at"] = updatedAt;
  }
  JsonObject data = doc.createNestedObject("data");
  char emotionTime[24]; snprintf(emotionTime, sizeof(emotionTime), "%04d-%02d-%02dT%02u:%02u", emotionFormTime.date.year, emotionFormTime.date.month, emotionFormTime.date.date, emotionFormTime.time.hours, emotionFormTime.time.minutes);
  data["emotion_time"] = emotionTime;
  const char* chosenEmotion = emotionChoice < 0 ? "" : EMOTION_CHOICES[emotionCategory][emotionChoice];
  data["emotion"] = chosenEmotion;
  data["emotion_selected"] = chosenEmotion;
  data["emotion_custom"] = "";
  data["emotion_category"] = EMOTION_CATEGORIES[emotionCategory];
  data["emotion_index"] = emotionIndexPercent;
  data["emotion_index_percent"] = emotionIndexPercent;
  JsonArray triggers = data.createNestedArray("triggers"); String triggerText;
  for (int i = 0; i < 8; ++i) if (emotionTriggers[i]) { triggers.add(EMOTION_TRIGGERS[i]); if (triggerText.length()) triggerText += "、"; triggerText += EMOTION_TRIGGERS[i]; }
  data["trigger_point"] = triggerText;
  data["trigger_other"] = "";
  data["heart_rate"] = EMOTION_HEART_RATES[emotionHeartRate];
  data["breath_rate"] = EMOTION_BREATH_RATES[emotionBreathRate];
  data["sweating"] = emotionSweating != 0;
  JsonArray bodySignals = data.createNestedArray("body_signals");
  if (emotionBodySignal >= 0) bodySignals.add(EMOTION_BODY_SIGNALS[(uint8_t)emotionBodySignal]);
  JsonArray bodyParts = data.createNestedArray("body_parts");
  if (emotionBodyPart >= 0) bodyParts.add(EMOTION_BODY_PARTS[(uint8_t)emotionBodyPart]);
  data["body_other"] = "";
  data["body_reaction"] = String(EMOTION_HEART_RATES[emotionHeartRate]) + "、" + EMOTION_BREATH_RATES[emotionBreathRate]
    + (emotionSweating ? "、冒汗" : "");
  JsonArray behaviors = data.createNestedArray("behavior_cues");
  if (emotionBehaviorCue > 0) behaviors.add(EMOTION_BEHAVIOR_CUES[emotionBehaviorCue]);
  data["behavior_other"] = "";
  data["observe_count"] = emotionObserveCount;
  data["observe_minutes"] = emotionObserveMinutes;
  data["observation_count_duration"] = String(emotionObserveCount) + "次 / " + String(emotionObserveMinutes) + "分鐘";
  data["grounding_timing"] = EMOTION_GROUNDING_TIMES[emotionGroundingTiming];
  JsonArray groundingActions = data.createNestedArray("grounding_actions");
  groundingActions.add(EMOTION_GROUNDING_ACTIONS[emotionGroundingAction]);
  data["grounding_other"] = "";
  data["emotional_grounding"] = String(EMOTION_GROUNDING_TIMES[emotionGroundingTiming]) + "、" + EMOTION_GROUNDING_ACTIONS[emotionGroundingAction];
  data["inner_voice"] = ""; data["natural_voice"] = ""; data["daily_review"] = ""; data["event"] = "";
  String body; serializeJson(doc, body);
  String localId = doc["local_id"].as<String>();
  // Save locally first so submitting never waits on a slow server. The same
  // queue handles online and offline operation; the loop uploads it later.
  if (saveEmotionQueue(localId, body)) {
    emotionLastSubmittedId = "queued:" + localId;
    emotionLastSubmittedPayload = body;
    emotionSubmitMessage = WiFi.status() == WL_CONNECTED
      ? "已保存在設備，稍後自動同步，可按左鍵撤回。"
      : "已離線保存，連線後會自動同步，可按左鍵撤回。";
    drawEmotionObservation();
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    emotionSubmitMessage = "離線佇列已滿或本機儲存不可用，資料尚未保存。";
    drawEmotionObservation();
    return false;
  }
  WiFiClientSecure secure; secure.setCACert(EMOTION_API_ROOT_CA);
  HTTPClient http; http.setTimeout(8000);
  String base = normalizeEmotionApiBase(emotionApiBase);
  if (!base.length() || !http.begin(secure, base + "/api/collections/entries/records")) { emotionSubmitMessage = "無法建立 HTTPS 連線。"; drawEmotionObservation(); return false; }
  http.addHeader("Content-Type", "application/json"); http.addHeader("Authorization", "Bearer " + emotionApiToken);
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end(); secure.stop();
  if (code == 401 && refreshEmotionApiToken()) {
    WiFiClientSecure retrySecure; retrySecure.setCACert(EMOTION_API_ROOT_CA);
    HTTPClient retry; retry.setTimeout(8000);
    if (retry.begin(retrySecure, base + "/api/collections/entries/records")) {
      retry.addHeader("Content-Type", "application/json"); retry.addHeader("Authorization", "Bearer " + emotionApiToken);
      code = retry.POST(body); if (code > 0) response = retry.getString(); retry.end(); retrySecure.stop();
    }
  }
  if (code == 200 || code == 201) {
    emotionApiState = EmotionApiState::Connected; emotionApiLastChecked = millis();
    emotionSubmitMessage = "已成功送出。";
    DynamicJsonDocument reply(1024);
    emotionLastSubmittedId = "";
    if (!deserializeJson(reply, response) && reply["id"].is<const char*>()) {
      emotionLastSubmittedId = reply["id"].as<String>();
      emotionLastSubmittedPayload = body;
      emotionSubmitMessage = "已成功送出，可按左鍵撤回。";
    }
    drawEmotionObservation();
    return true;
  }
  if (code < 0 && saveEmotionQueue(localId, body)) {
    emotionLastSubmittedId = "queued:" + localId;
    emotionLastSubmittedPayload = body;
    emotionSubmitMessage = "網路暫時不可用，已離線保存，稍後自動同步。";
    drawEmotionObservation();
    return true;
  }
  emotionApiState = EmotionApiState::Disconnected; emotionApiLastChecked = millis();
  if (code == 401) emotionSubmitMessage = "登入已過期，請回網頁「情緒觀察」重新登入。";
  else if (code < 0) emotionSubmitMessage = "HTTPS／網路連線失敗（" + String(code) + "），資料未送出。";
  else emotionSubmitMessage = "送出失敗（HTTP " + String(code) + "），資料未送出。";
  drawEmotionObservation();
  return false;
}

bool withdrawEmotionObservation() {
  if (!emotionLastSubmittedId.length()) {
    emotionSubmitMessage = "找不到剛送出的紀錄，無法撤回。";
    drawEmotionObservation();
    return false;
  }
  if (emotionLastSubmittedId.startsWith("queued:")) {
    if (!emotionStorageReady) { emotionSubmitMessage = "本機儲存未啟動，無法撤回佇列紀錄。"; drawEmotionObservation(); return false; }
    String localId = emotionLastSubmittedId.substring(7);
    File input = SPIFFS.open("/emotion_queue.json", FILE_READ);
    DynamicJsonDocument queue(32768);
    if (input) { deserializeJson(queue, input); input.close(); }
    JsonArray items = queue.as<JsonArray>();
    for (size_t i = 0; i < items.size(); ++i) {
      if (String((const char*)(items[i]["local_id"] | "")) == localId) { items.remove(i); break; }
    }
    File output = SPIFFS.open("/emotion_queue.json", FILE_WRITE);
    if (!output) { emotionSubmitMessage = "無法撤回離線佇列紀錄。"; drawEmotionObservation(); return false; }
    serializeJson(queue, output); output.close();
    emotionPendingCount = queue.size();
    emotionLastSubmittedId = ""; emotionLastSubmittedPayload = "";
    emotionSubmitCompleted = false; emotionSubmitArmed = false;
    emotionSubmitMessage = "已取消尚未同步的離線紀錄。";
    drawEmotionObservation();
    return true;
  }
  if (WiFi.status() != WL_CONNECTED || !emotionApiConfigured()) {
    emotionSubmitMessage = "資料庫未連線，暫時無法撤回。";
    drawEmotionObservation();
    return false;
  }
  emotionSubmitMessage = "正在撤回…";
  drawEmotionObservation();
  DynamicJsonDocument withdrawDoc(6144);
  if (!emotionLastSubmittedPayload.length() || deserializeJson(withdrawDoc, emotionLastSubmittedPayload)) {
    emotionSubmitMessage = "找不到原始紀錄內容，無法安全撤回。";
    drawEmotionObservation();
    return false;
  }
  time_t now = time(nullptr); struct tm utcNow;
  if (now < 1700000000 || !gmtime_r(&now, &utcNow)) {
    emotionSubmitMessage = "設備時間尚未同步，暫時無法撤回。";
    drawEmotionObservation();
    return false;
  }
  char updatedAt[32]; strftime(updatedAt, sizeof(updatedAt), "%Y-%m-%dT%H:%M:%S.000Z", &utcNow);
  withdrawDoc["deleted"] = true;
  withdrawDoc["client_updated_at"] = updatedAt;
  String withdrawBody; serializeJson(withdrawDoc, withdrawBody);
  String base = normalizeEmotionApiBase(emotionApiBase);
  String endpoint = base + "/api/collections/entries/records/" + emotionLastSubmittedId;
  WiFiClientSecure secure; secure.setCACert(EMOTION_API_ROOT_CA);
  HTTPClient http; http.setTimeout(8000);
  int code = -1;
  if (base.length() && http.begin(secure, endpoint)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + emotionApiToken);
    code = http.sendRequest("PATCH", withdrawBody);
    http.end(); secure.stop();
  }
  if (code == 401 && refreshEmotionApiToken()) {
    WiFiClientSecure retrySecure; retrySecure.setCACert(EMOTION_API_ROOT_CA);
    HTTPClient retry; retry.setTimeout(8000);
    if (retry.begin(retrySecure, endpoint)) {
      retry.addHeader("Content-Type", "application/json");
      retry.addHeader("Authorization", "Bearer " + emotionApiToken);
      code = retry.sendRequest("PATCH", withdrawBody);
      retry.end(); retrySecure.stop();
    }
  }
  if (code == 200 || code == 204) {
    emotionLastSubmittedId = "";
    emotionLastSubmittedPayload = "";
    emotionSubmitCompleted = false;
    emotionSubmitArmed = false;
    emotionSubmitMessage = "已撤回，可修改後重新送出。";
    emotionApiState = EmotionApiState::Connected;
    emotionApiLastChecked = millis();
    drawEmotionObservation();
    return true;
  }
  if (code == 404) emotionSubmitMessage = "這筆紀錄已不存在。";
  else if (code == 401) emotionSubmitMessage = "登入已過期，請重新登入後撤回。";
  else emotionSubmitMessage = "撤回失敗（HTTP " + String(code) + "）。";
  drawEmotionObservation();
  return false;
}

void drawEmotionReminder() {
  screenNow = Screen::EmotionReminder;
  drawEmotionMatrixBackground();
  uint16_t panel = emotionPanel(14);
  M5.Display.fillRoundRect(18, 28, 284, 159, 18, panel);
  M5.Display.drawRoundRect(18, 28, 284, 159, 18, emotionTheme(85));
  useUIMediumFont(); M5.Display.setTextColor(emotionTheme(100), panel); M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("情緒觀察提醒", 160, 63);
  useUIFont(1); M5.Display.setTextColor(TFT_WHITE, panel);
  M5.Display.drawString("停一下，留意此刻的感受", 160, 102);
  M5.Display.drawString("填寫約需 1 分鐘，可隨時取消", 160, 130);
  drawEmotionBottomBar("開始填寫", "稍後", "關閉");
}

void startEmotionReminder(uint32_t nowMs) {
  if (alarmActive >= 0 || emotionReminderEnd || (meditationState == MeditationState::Running && meditationNoiseEnabled)) return;
  emotionReminderReturnScreen = screenNow;
  emotionReminderEnd = nowMs + (uint32_t)emotionReminderDurationSeconds * 1000UL;
  emotionLastVibrationToggle = 0;
  emotionReminderVibrationOn = false;
  if (screenSleeping) wakeDisplay();
  if (emotionReminderSound) playSoundChoice(emotionReminderSoundChoice, emotionReminderVolume, 1000);
  drawEmotionReminder();
}

void stopEmotionReminder(bool redraw = true) {
  emotionReminderEnd = 0;
  emotionReminderVibrationOn = false;
  M5.Power.setVibration(0);
  M5.Speaker.stop();
  if (redraw) {
    screenNow = emotionReminderReturnScreen;
    if (screenNow == Screen::Clock) { drawClock(true); drawAstronaut(); }
    else if (screenNow == Screen::Companion) drawCompanionButtons();
    else if (screenNow == Screen::Meditation) drawMeditation();
    else if (screenNow == Screen::EmotionObservation) drawEmotionObservation();
    else if (screenNow == Screen::EmotionSettings) showEmotionSettings();
    else showMenu();
  }
}

void checkEmotionReminder(uint32_t nowMs, const m5::rtc_datetime_t& dt) {
  if (emotionReminderEnd) {
    if ((int32_t)(nowMs - emotionReminderEnd) >= 0) { stopEmotionReminder(); return; }
    if (emotionReminderVibration && nowMs - emotionLastVibrationToggle >= 350UL) {
      emotionLastVibrationToggle = nowMs;
      emotionReminderVibrationOn = !emotionReminderVibrationOn;
      M5.Power.setVibration(emotionReminderVibrationOn ? 180 : 0);
    }
    return;
  }
  if (emotionReminderSnoozeUntil && (int32_t)(nowMs - emotionReminderSnoozeUntil) >= 0) {
    emotionReminderSnoozeUntil = 0;
    startEmotionReminder(nowMs);
    return;
  }
  if (emotionReminderMode == 1) {
    int32_t day = dt.date.year * 512 + dt.date.month * 32 + dt.date.date;
    uint16_t minuteOfDay = dt.time.hours * 60 + dt.time.minutes;
    int32_t minuteKey = day * 1440L + minuteOfDay;
    bool inWindow = minuteOfDay >= emotionReminderWindowStart && minuteOfDay <= emotionReminderWindowEnd;
    bool onInterval = inWindow && ((minuteOfDay - emotionReminderWindowStart) % emotionReminderIntervalMinutes == 0);
    if (onInterval && minuteKey != emotionLastReminderMinuteKey) {
      emotionLastReminderMinuteKey = minuteKey;
      startEmotionReminder(nowMs);
    }
  } else if (emotionReminderMode == 2) {
    int32_t day = dt.date.year * 512 + dt.date.month * 32 + dt.date.date;
    int32_t minuteKey = day * 1440L + dt.time.hours * 60 + dt.time.minutes;
    if (minuteKey == emotionLastReminderMinuteKey) return;
    for (int i = 0; i < 3; ++i) if (emotionReminderTimes[i] != 0xFFFF && emotionReminderTimes[i] == dt.time.hours * 60 + dt.time.minutes) {
      emotionLastReminderMinuteKey = minuteKey;
      startEmotionReminder(nowMs);
      return;
    }
  }
}

void handleEmotionTouch(const m5::touch_detail_t& t) {
  if (screenNow == Screen::EmotionRecords) {
    if (t.wasPressed()) emotionMiddlePressedAt = (t.y >= 210 && t.x >= 107 && t.x < 214) ? millis() : 0;
    if (!t.wasReleased()) return;
    if (t.y >= 210 && t.x >= 107 && t.x < 214 && emotionMiddlePressedAt && millis() - emotionMiddlePressedAt >= 700UL) {
      emotionMiddlePressedAt = 0; haptic(20); forceEmotionSync(); return;
    }
    if (t.y >= 210) {
      haptic(12);
      if (t.x < 107 && emotionRecordsPage) { emotionRecordsPage = 0; drawEmotionRecords(); }
      else if (t.x < 107) showEmotionRecords(true);
      else if (t.x < 214 && !emotionRecordsPage) { emotionRecordsPage = 1; drawEmotionRecords(); }
      else if (t.x >= 214) { emotionRecordsPage = 0; showEmotionObservation(false); }
    }
    return;
  }
  if (screenNow == Screen::EmotionReminder) {
    if (!t.wasReleased()) return;
    haptic(12);
    if (t.y >= 210 && t.x < 107) { stopEmotionReminder(false); showEmotionObservation(true); }
    else if (t.y >= 210 && t.x < 214) { stopEmotionReminder(); emotionReminderSnoozeUntil = millis() + 600000UL; }
    else if (t.y >= 210) stopEmotionReminder();
    return;
  }
  if (screenNow == Screen::EmotionSettings) {
    if (!t.wasReleased()) return;
    haptic(12);
    if (t.y >= 210) {
      if (t.x < 107 && emotionSettingsPage) { --emotionSettingsPage; showEmotionSettings(); }
      else if (t.x >= 107 && t.x < 214 && emotionSettingsPage < 2) { ++emotionSettingsPage; showEmotionSettings(); }
      else if (t.x >= 214) { saveSettings(); showEmotionObservation(false); }
      return;
    }
    if (t.y < EMOTION_BIG_ROW_TOP || t.y >= EMOTION_BIG_ROW_TOP + 4 * EMOTION_BIG_ROW_PITCH) return;
    int row = constrain((t.y - EMOTION_BIG_ROW_TOP) / EMOTION_BIG_ROW_PITCH, 0, 3);
    if (emotionSettingsPage == 2) row = row == 0 ? 4 : -1;  // page 3 only has the volume row
    if (row < 0) return;
    if (!emotionSettingsPage) {
      if (row == 0) {
        emotionReminderMode = (emotionReminderMode + 1) % 3;
      } else if (emotionReminderMode == 1 && row == 1) {
        emotionReminderWindowStart = (emotionReminderWindowStart + 10) % 1440;
        if (emotionReminderWindowEnd < emotionReminderWindowStart) emotionReminderWindowEnd = emotionReminderWindowStart;
      } else if (emotionReminderMode == 1 && row == 2) {
        emotionReminderWindowEnd = (emotionReminderWindowEnd + 10) % 1440;
        if (emotionReminderWindowEnd < emotionReminderWindowStart) emotionReminderWindowEnd = emotionReminderWindowStart;
      } else if (emotionReminderMode == 1 && row == 3) {
        const uint16_t choices[] = {10, 15, 30, 60, 120, 180, 240};
        int index = 0; while (index < 7 && choices[index] != emotionReminderIntervalMinutes) ++index;
        emotionReminderIntervalMinutes = choices[(index + 1) % 7];
      } else if (emotionReminderMode == 2 && row >= 1 && row <= 3) {
        int slot = row - 1;
        uint16_t fallback[] = {600, 900, 1200};
        if (emotionReminderTimes[slot] == 0xFFFF) emotionReminderTimes[slot] = fallback[slot];
        else if (emotionReminderTimes[slot] >= 1410) emotionReminderTimes[slot] = 0xFFFF;
        else emotionReminderTimes[slot] += 30;
      }
    } else {
      if (row == 0) emotionReminderVibration = !emotionReminderVibration;
      else if (row == 1) emotionReminderSound = !emotionReminderSound;
      else if (row == 2) {
        const uint8_t choices[] = {10, 30, 60, 120};
        int index = 0; while (index < 4 && choices[index] != emotionReminderDurationSeconds) ++index;
        emotionReminderDurationSeconds = choices[(index + 1) % 4];
      } else if (row == 3) {
        emotionReminderSoundChoice = (emotionReminderSoundChoice + 1) % 4;
        playSoundChoice(emotionReminderSoundChoice, emotionReminderVolume, 1);
      } else {
        emotionReminderVolume = emotionReminderVolume >= 100 ? 10 : emotionReminderVolume + 10;
        playSoundChoice(emotionReminderSoundChoice, emotionReminderVolume, 1);
      }
    }
    emotionLastReminderMinuteKey = -1;
    saveSettings(); showEmotionSettings();
    return;
  }
  if (t.wasPressed()) {
    emotionCancelPressValid = t.y >= 210 && t.x >= 214;
    emotionCancelPressedAt = emotionCancelPressValid ? millis() : 0;
    emotionMiddlePressedAt = (t.y >= 210 && t.x >= 107 && t.x < 214) ? millis() : 0;
  }
  if (emotionApiConfigured() && emotionFormPage == 4 && t.isPressed() && t.y >= 115 && t.y < 195) {
    int next = constrain((int)map(constrain((int)t.x, 28, 292), 28, 292, 5, 120), 5, 120);
    next = constrain(((next + 2) / 5) * 5, 5, 120);
    if (next != emotionIndexPercent) { emotionIndexPercent = next; drawEmotionObservation(); }
    return;
  }
  if (!t.wasReleased()) return;
  haptic(12);
  if (emotionApiConfigured() && emotionFormPage < 7 && t.x >= 282 && t.y < 34) {
    resetEmotionPage(emotionFormPage);
    drawEmotionObservation();
    return;
  }
  if (t.y >= 210 && t.x >= 107 && t.x < 214 && emotionMiddlePressedAt && millis() - emotionMiddlePressedAt >= 700UL) {
    emotionMiddlePressedAt = 0;
    forceEmotionSync();
    return;
  }
  if (t.y >= 210) {
    if (t.x >= 214) {
      bool longPress = emotionCancelPressValid && emotionCancelPressedAt && millis() - emotionCancelPressedAt >= 700UL;
      emotionCancelPressValid = false; emotionCancelPressedAt = 0;
      emotionSubmitArmed = false; emotionSubmitMessage = "";
      if (emotionReminderEnd) stopEmotionReminder();
      else if (longPress) { emotionSettingsPage = 0; showEmotionSettings(); }
      else { screenNow = Screen::Clock; drawClock(true); drawAstronaut(); }
    } else if (t.x < 107 && emotionFormPage == 7 && emotionSubmitCompleted) {
      withdrawEmotionObservation();
    } else if (t.x < 107 && emotionFormPage == 7 && emotionSubmitArmed) {
      emotionSubmitArmed = false; emotionSubmitMessage = ""; drawEmotionObservation();
    } else if (t.x < 107 && emotionFormPage == 0) {
      showEmotionRecords(true);
    } else if (t.x < 107 && emotionFormPage > 0) {
      --emotionFormPage; emotionSubmitArmed = false; emotionSubmitMessage = ""; drawEmotionObservation();
    } else if (t.x >= 107 && t.x < 214) {
      if (emotionFormPage < 7) {
        if (emotionFormPage == 3 && emotionChoice < 0) {
          emotionSubmitMessage = "請先選擇一個情緒";
          drawEmotionObservation();
          return;
        }
        ++emotionFormPage; emotionSubmitArmed = false; emotionSubmitMessage = "";
        drawEmotionObservation();
      } else if (emotionSubmitCompleted) {
        showEmotionObservation(true);
      } else if (!emotionSubmitArmed) {
        emotionSubmitArmed = true; emotionSubmitMessage = "請再次確認送出。"; drawEmotionObservation();
      } else {
        emotionSubmitArmed = false;
        if (submitEmotionObservation()) { emotionSubmitCompleted = true; emotionSubmitArmed = false; drawEmotionObservation(); }
      }
    }
    return;
  }
  emotionCancelPressValid = false; emotionCancelPressedAt = 0;
  if (!emotionApiConfigured()) return;
  if (emotionFormPage == 0) {
    if (t.y >= 48 && t.y < 160) {
      uint8_t field = constrain(((int)t.x - 5) / 63, 0, 4);
      adjustEmotionTimeField(field, t.y < 104 ? -1 : 1);
      drawEmotionObservation();
    }
    return;
  } else if (emotionFormPage == 1) {
    if (t.y >= 39 && t.y < 203) {
      int col = t.x >= 160 ? 1 : 0, row = constrain((t.y - 39) / 41, 0, 3);
      emotionTriggers[row * 2 + col] = !emotionTriggers[row * 2 + col];
      drawEmotionObservation();
    }
  } else if (emotionFormPage == 2) {
    if (t.y < 41 || t.y >= 192) return;
    int row = (t.y >= 120 ? 3 : 0) + constrain((int)t.x / 105, 0, 2);
    if (row == 0) emotionHeartRate = (emotionHeartRate + 1) % (sizeof(EMOTION_HEART_RATES) / sizeof(EMOTION_HEART_RATES[0]));
    else if (row == 1) emotionBreathRate = (emotionBreathRate + 1) % (sizeof(EMOTION_BREATH_RATES) / sizeof(EMOTION_BREATH_RATES[0]));
    else if (row == 2) emotionSweating = emotionSweating ? 0 : 1;
    else if (row == 3) emotionBodySignal = emotionBodySignal < 0 ? 0 :
      (emotionBodySignal + 1 >= (int)(sizeof(EMOTION_BODY_SIGNALS) / sizeof(EMOTION_BODY_SIGNALS[0])) ? -1 : emotionBodySignal + 1);
    else if (row == 4) emotionBodyPart = emotionBodyPart < 0 ? 0 :
      (emotionBodyPart + 1 >= (int)(sizeof(EMOTION_BODY_PARTS) / sizeof(EMOTION_BODY_PARTS[0])) ? -1 : emotionBodyPart + 1);
    else emotionBehaviorCue = (emotionBehaviorCue + 1) % (sizeof(EMOTION_BEHAVIOR_CUES) / sizeof(EMOTION_BEHAVIOR_CUES[0]));
    drawEmotionObservation();
  } else if (emotionFormPage == 3) {
    const uint8_t categoryCount = sizeof(EMOTION_CATEGORIES) / sizeof(EMOTION_CATEGORIES[0]);
    if (t.y >= 46 && t.y < 107) {
      emotionCategory = (emotionCategory + (t.x >= 160 ? 1 : categoryCount - 1)) % categoryCount;
      emotionChoice = -1;
    } else if (t.y >= 118 && t.y < 179) {
      const uint8_t choiceCount = EMOTION_CHOICE_COUNTS[emotionCategory];
      emotionChoice = (emotionChoice + (t.x >= 160 ? 1 : choiceCount - 1)) % choiceCount;
      emotionSubmitMessage = "";
    }
    drawEmotionObservation();
  } else if (emotionFormPage == 4) {
    return;
  } else if (emotionFormPage == 5) {
    bool increment = t.x >= 160;
    if (t.y < 115) emotionObserveCount = constrain((int)emotionObserveCount + (increment ? 1 : -1), 1, 10);
    else emotionObserveMinutes = constrain((int)emotionObserveMinutes + (increment ? 1 : -1), 1, 30);
    drawEmotionObservation();
  } else if (emotionFormPage == 6) {
    bool next = t.x >= 160;
    if (t.y < 115) {
      const uint8_t count = sizeof(EMOTION_GROUNDING_TIMES) / sizeof(EMOTION_GROUNDING_TIMES[0]);
      emotionGroundingTiming = (emotionGroundingTiming + (next ? 1 : count - 1)) % count;
    } else {
      const uint8_t count = sizeof(EMOTION_GROUNDING_ACTIONS) / sizeof(EMOTION_GROUNDING_ACTIONS[0]);
      emotionGroundingAction = (emotionGroundingAction + (next ? 1 : count - 1)) % count;
    }
    drawEmotionObservation();
  }
}

void handleClockTouch(const m5::touch_detail_t& t) {
  if (alarmActive >= 0) {
    if (t.wasReleased() && t.y >= 215 && t.x >= 107 && t.x < 214) { snoozeAlarm(); return; }
    if (clockFace == ClockFace::Space) {
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
    } else if (clockFace == ClockFace::Minimal) {
      float knobAngle = -2.15f + alarmGearProgress;
      int knobX = 88 + lroundf(cosf(knobAngle) * 29.0f);
      int knobY = 114 + lroundf(sinf(knobAngle) * 29.0f);
      if (t.wasPressed() && sq((int)t.x - knobX) + sq((int)t.y - knobY) <= 20 * 20) {
        alarmGearDragging = true;
        alarmGearLastAngle = atan2f((float)t.y - 114.0f, (float)t.x - 88.0f);
      }
      if (alarmGearDragging && t.isPressed()) {
        int dx = (int)t.x - 88, dy = (int)t.y - 114;
        int radiusSquared = dx * dx + dy * dy;
        if (radiusSquared >= 18 * 18 && radiusSquared <= 86 * 86) {
          float angle = atan2f((float)dy, (float)dx);
          float delta = angle - alarmGearLastAngle;
          while (delta > PI) delta -= 2.0f * PI;
          while (delta < -PI) delta += 2.0f * PI;
          alarmGearLastAngle = angle;
          if (delta > 0.0f && delta < 0.8f) {
            alarmGearProgress = min<float>(6.2831853f, alarmGearProgress + delta);
            if (millis() - lastAlarmChallengeDraw >= 28UL) {
              lastAlarmChallengeDraw = millis();
              drawFlipAlarmChallenge();
            }
            if (alarmGearProgress >= 6.2631853f) {
              haptic(45);
              dismissAlarm();
            }
          }
        }
      }
      if (t.wasReleased()) alarmGearDragging = false;
    } else {
      if (t.wasPressed() && t.x >= 100 && t.x <= 220 && t.y >= 90 && t.y <= 151) {
        alarmPillHolding = true;
        alarmPillHoldStarted = millis();
        lastAlarmChallengeDraw = 0;
      }
      if (alarmPillHolding && t.wasReleased()) {
        alarmPillHolding = false;
        alarmPillHoldStarted = 0;
        drawMatrixAlarmChallenge(millis());
      }
    }
    return;
  }
  const bool inNavigation = t.y >= 210;
  const bool inSettings = inNavigation && t.x >= 214;
  const bool inMiddle = inNavigation && t.x >= 107 && t.x < 214;
  if (t.wasPressed()) {
    clockSettingsPressValid = inSettings;
    clockSettingsPressedAt = inSettings ? millis() : 0;
    clockMiddlePressValid = inMiddle;
    clockMiddlePressedAt = inMiddle ? millis() : 0;
    companionNavPressValid = t.y >= 210 && t.x < 107;
    companionNavPressStarted = companionNavPressValid ? millis() : 0;
  }
  if (hassAssistVoiceMode == HASS_MODE_HOLD && hassAssistEnabled && clockMiddlePressValid && t.isPressed()
      && clockMiddlePressedAt && millis() - clockMiddlePressedAt >= 700UL) {
    // Hold-to-talk shortcut: start talking straight from the clock. The
    // Assist screen takes over this touch, so releasing anywhere sends.
    clockMiddlePressValid = false; clockMiddlePressedAt = 0;
    haptic(12);
    hassAssistTranscript = ""; hassAssistReply = ""; hassAssistError = "";
    hassAssistState = hassAssistAuthenticated ? HassAssistState::Ready : HassAssistState::Disconnected;
    enterHassAssistScreenForShortcut();
    hassAssistTouchActive = true;
    hassAssistTouchStartedAt = millis();
    hassAssistTouchLongStarted = true;
    hassAssistHolding = true;
    startHassAssistPipeline();
    if (!hassAssistPipelineActive) hassAssistHolding = false;
    return;
  }
  if (!t.wasReleased()) return;
  if (t.y < 210) {
    clockSettingsPressValid = false;
    clockSettingsPressedAt = 0;
    clockMiddlePressValid = false;
    clockMiddlePressedAt = 0;
    companionNavPressValid = false;
    companionNavPressStarted = 0;
    return;
  }
  haptic();
  if (t.x < 107) {
    const bool longPress = companionNavPressValid && companionNavPressStarted && millis() - companionNavPressStarted >= 700UL;
    companionNavPressValid = false; companionNavPressStarted = 0;
    clockSettingsPressValid = false;
    clockMiddlePressValid = false; clockMiddlePressedAt = 0;
    if (longPress) showEmotionObservation(true); else showCompanion();
  } else if (t.x < 214) {
    const bool longPress = clockMiddlePressValid && clockMiddlePressedAt && millis() - clockMiddlePressedAt >= 700UL;
    companionNavPressValid = false; companionNavPressStarted = 0;
    clockSettingsPressValid = false;
    clockMiddlePressValid = false; clockMiddlePressedAt = 0;
    if (longPress) showHassAssist(); else showMeditation();
  } else {
    const bool longPress = clockSettingsPressValid && clockSettingsPressedAt && millis() - clockSettingsPressedAt >= 700UL;
    clockSettingsPressValid = false;
    clockSettingsPressedAt = 0;
    clockMiddlePressValid = false;
    clockMiddlePressedAt = 0;
    if (longPress) {
      manualNightLightOverride = true;
      manualNightLightActive = !manualNightLightActive;
      updateAlarmBaseLights(millis());
    } else {
      menuPage = 0;
      showMenu();
    }
  }
}

bool deviceIsFlat() {
  if (!M5.Imu.isEnabled()) return false;
  M5.Imu.update();
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  return fabsf(az) > 0.82f && fabsf(ax) < 0.42f && fabsf(ay) < 0.42f;
}


// ---------------------------------------------------------------------------
// Calendar (iCal subscription)
// ---------------------------------------------------------------------------

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int64_t calDaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

time_t calLocalTime(int y, int mo, int d, int h = 0, int mi = 0, int sec = 0) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = sec; t.tm_isdst = -1;
  return mktime(&t);
}

time_t calLocalMidnight(time_t when) {
  struct tm t; localtime_r(&when, &t);
  return calLocalTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

time_t calAddDays(time_t localMidnight, int days) {
  struct tm t; localtime_r(&localMidnight, &t);
  return calLocalTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday + days);
}

// Parse 20260929, 20260929T090000 or 20260929T090000Z.
bool calParseDateTime(const String& value, time_t& out, bool& allDay) {
  if (value.length() < 8) return false;
  int y = value.substring(0, 4).toInt(), mo = value.substring(4, 6).toInt(), d = value.substring(6, 8).toInt();
  allDay = value.length() < 15;
  if (allDay) { out = calLocalTime(y, mo, d); return true; }
  int h = value.substring(9, 11).toInt(), mi = value.substring(11, 13).toInt(), sec = value.substring(13, 15).toInt();
  if (value.endsWith("Z")) out = (time_t)(calDaysFromCivil(y, mo, d) * 86400LL + h * 3600 + mi * 60 + sec);
  else out = calLocalTime(y, mo, d, h, mi, sec);  // TZID: treated as device-local time
  return true;
}

String calUnescape(String v) {
  v.replace("\\n", " "); v.replace("\\N", " "); v.replace("\\,", ","); v.replace("\;", ";"); v.replace("\\\\", "\\");
  v.trim();
  return v;
}

void calAddOccurrence(time_t start, time_t duration, bool allDay, const String& title, const String& location) {
  if (calEventCount >= CAL_MAX_EVENTS) return;
  time_t end = start + duration;
  if (end <= calWindowStart || start >= calWindowEnd) return;
  CalEvent& e = calEvents[calEventCount++];
  e.start = start; e.end = end; e.allDay = allDay;
  strlcpy(e.title, title.length() ? title.c_str() : "(無標題)", sizeof(e.title));
  strlcpy(e.location, location.c_str(), sizeof(e.location));
}

String calRulePart(const String& rule, const char* key) {
  String k = String(key) + "=";
  int p = rule.indexOf(k);
  if (p < 0) return "";
  p += k.length();
  int e = rule.indexOf(';', p);
  return e < 0 ? rule.substring(p) : rule.substring(p, e);
}

// Expand one VEVENT (with an optional simple RRULE) into the window.
void calEmitEvent(time_t start, time_t end, bool allDay, const String& rrule, const time_t* exdates, int exCount,
                  const String& title, const String& location) {
  if (!start) return;
  if (end <= start) end = start + (allDay ? 86400 : 3600);
  time_t duration = end - start;
  auto excluded = [&](time_t when) {
    for (int i = 0; i < exCount; ++i) if (exdates[i] == when) return true;
    return false;
  };
  if (!rrule.length()) { if (!excluded(start)) calAddOccurrence(start, duration, allDay, title, location); return; }
  String freq = calRulePart(rrule, "FREQ");
  int interval = max(1, (int)calRulePart(rrule, "INTERVAL").toInt());
  int count = calRulePart(rrule, "COUNT").toInt();
  time_t until = 0; bool untilAllDay;
  String untilText = calRulePart(rrule, "UNTIL");
  if (untilText.length()) calParseDateTime(untilText, until, untilAllDay);
  if (untilText.length() && untilAllDay) until += 86399;
  String byDay = calRulePart(rrule, "BYDAY");
  struct tm base; localtime_r(&start, &base);
  int emitted = 0;
  for (int step = 0; step < 3000; ++step) {
    // Candidate occurrences for this period.
    time_t candidates[7]; int n = 0;
    if (freq == "WEEKLY" && byDay.length()) {
      static const char* codes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
      time_t weekStart = calLocalTime(base.tm_year + 1900, base.tm_mon + 1, base.tm_mday - base.tm_wday + step * 7 * interval,
                                      base.tm_hour, base.tm_min, base.tm_sec);
      for (int wd = 0; wd < 7; ++wd) {
        if (byDay.indexOf(codes[wd]) < 0) continue;
        struct tm w; localtime_r(&weekStart, &w);
        time_t c = calLocalTime(w.tm_year + 1900, w.tm_mon + 1, w.tm_mday + wd, base.tm_hour, base.tm_min, base.tm_sec);
        if (c >= start) candidates[n++] = c;
      }
    } else {
      int y = base.tm_year + 1900, mo = base.tm_mon + 1, d = base.tm_mday;
      if (freq == "DAILY") d += step * interval;
      else if (freq == "WEEKLY") d += step * 7 * interval;
      else if (freq == "MONTHLY") mo += step * interval;
      else if (freq == "YEARLY") y += step * interval;
      else { if (!excluded(start)) calAddOccurrence(start, duration, allDay, title, location); return; }
      time_t c = calLocalTime(y, mo, d, base.tm_hour, base.tm_min, base.tm_sec);
      struct tm check; localtime_r(&c, &check);
      if ((freq == "MONTHLY" || freq == "YEARLY") && check.tm_mday != d) continue;  // e.g. 31st in a short month
      candidates[n++] = c;
    }
    for (int i = 0; i < n; ++i) {
      time_t c = candidates[i];
      if (until && c > until) return;
      if (count && emitted >= count) return;
      ++emitted;
      if (c >= calWindowEnd) return;
      if (!excluded(c)) calAddOccurrence(c, duration, allDay, title, location);
    }
  }
}

bool fetchCalendar(time_t aroundDay) {
  calError = "";
  if (!calendarIcalUrl.length()) { calError = "請先在網頁後台「日曆」填入 iCal 網址"; return false; }
  if (WiFi.status() != WL_CONNECTED) { calError = "Wi-Fi 未連線"; return false; }
  if (!calEvents) calEvents = (CalEvent*)heap_caps_malloc(sizeof(CalEvent) * CAL_MAX_EVENTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!calEvents) { calError = "記憶體不足"; return false; }
  // Window: from a week before the viewed month to about six weeks after it.
  struct tm v; localtime_r(&aroundDay, &v);
  time_t monthStart = calLocalTime(v.tm_year + 1900, v.tm_mon + 1, 1);
  calWindowStart = calAddDays(monthStart, -7);
  calWindowEnd = calAddDays(monthStart, 45);
  calEventCount = 0;

  String url = calendarIcalUrl;
  if (url.startsWith("webcal://")) url = "https://" + url.substring(9);
  WiFiClientSecure secure; secure.setInsecure(); secure.setTimeout(15);
  WiFiClient plain;
  HTTPClient http;
  http.useHTTP10(true);  // plain bytes, server closes when done
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool https = url.startsWith("https://");
  if (!(https ? http.begin(secure, url) : http.begin(plain, url))) { calError = "無法開啟 iCal 網址"; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); calError = "iCal 下載失敗 HTTP " + String(code); return false; }
  WiFiClient* stream = http.getStreamPtr();
  stream->setTimeout(8);

  bool inEvent = false;
  time_t evStart = 0, evEnd = 0; bool evAllDay = false, endAllDay;
  String rrule, title, location;
  time_t exdates[16]; int exCount = 0;
  String line = stream->readStringUntil('\n');
  uint32_t lines = 0;
  while (line.length() || stream->available() || http.connected()) {
    // Unfold continuation lines (RFC 5545: next line starts with a space).
    while (stream->available() && (stream->peek() == ' ' || stream->peek() == '\t')) {
      stream->read();
      String more = stream->readStringUntil('\n');
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      line += more;
    }
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (++lines % 200 == 0) delay(0);
    int colon = line.indexOf(':');
    String name = colon > 0 ? line.substring(0, colon) : line;
    String value = colon > 0 ? line.substring(colon + 1) : "";
    int semi = name.indexOf(';');
    String key = semi > 0 ? name.substring(0, semi) : name;
    if (line == "BEGIN:VEVENT") {
      inEvent = true; evStart = evEnd = 0; evAllDay = false; rrule = ""; title = ""; location = ""; exCount = 0;
    } else if (line == "END:VEVENT") {
      if (inEvent && name.indexOf("RECURRENCE-ID") < 0) calEmitEvent(evStart, evEnd, evAllDay, rrule, exdates, exCount, title, location);
      inEvent = false;
    } else if (inEvent) {
      if (key == "DTSTART") calParseDateTime(value, evStart, evAllDay);
      else if (key == "DTEND") calParseDateTime(value, evEnd, endAllDay);
      else if (key == "SUMMARY") title = calUnescape(value);
      else if (key == "LOCATION") location = calUnescape(value);
      else if (key == "RRULE") rrule = value;
      else if (key == "RECURRENCE-ID") { evStart = 0; }  // modified instance: skip to avoid duplicates
      else if (key == "EXDATE") {
        int from = 0;
        while (from < (int)value.length() && exCount < 16) {
          int comma = value.indexOf(',', from); if (comma < 0) comma = value.length();
          bool ad; time_t ex;
          if (calParseDateTime(value.substring(from, comma), ex, ad)) exdates[exCount++] = ex;
          from = comma + 1;
        }
      }
    }
    if (!stream->available() && !http.connected()) break;
    line = stream->readStringUntil('\n');
  }
  http.end();
  // Sort by start time (insertion sort; the list is small).
  for (int i = 1; i < calEventCount; ++i) {
    CalEvent tmp = calEvents[i]; int j = i - 1;
    while (j >= 0 && calEvents[j].start > tmp.start) { calEvents[j + 1] = calEvents[j]; --j; }
    calEvents[j + 1] = tmp;
  }
  calFetchedAt = millis();
  Serial.printf("[calendar] %u lines, %d events in window\n", (unsigned)lines, calEventCount);
  return true;
}

bool calEventOnDay(const CalEvent& e, time_t dayStart) {
  time_t dayEnd = calAddDays(dayStart, 1);
  return e.start < dayEnd && e.end > dayStart;
}

// Calendar colours follow the active clock face.
struct CalTheme {
  uint16_t bg, panel, panelAlt, border, accent, title, text, muted, today, todayText, dot, weekend, outside;
};
CalTheme calTheme;

void applyCalendarTheme() {
  if (clockFace == ClockFace::Matrix) {
    // Phosphor palette derived from the user's Matrix rain colour.
    calTheme = {TFT_BLACK, matrixColor(10), matrixColor(16), matrixColor(38), matrixColor(100), matrixColor(100),
                M5.Display.color565(215, 255, 225), matrixColor(58), matrixColor(34), TFT_WHITE, matrixColor(100),
                matrixColor(80), matrixColor(26)};
  } else if (clockFace == ClockFace::Minimal) {
    // Flip clock: charcoal cards on black, white digits, amber accent.
    calTheme = {TFT_BLACK, 0x2124, 0x18C3, 0x4208, 0xFD20, 0xFD20, TFT_WHITE, 0x9CF3, 0xFD20, TFT_BLACK, 0xFD20,
                0xFB2C, 0x528A};
  } else {
    // Space face: the existing blue-grey scheme.
    calTheme = {BG, PANEL, UI_PANEL_ALT, UI_BORDER, ACCENT, 0x65DF, TFT_WHITE, UI_MUTED, UI_BLUE, TFT_WHITE, 0x07E0,
                0xFB2C, 0x528A};
  }
}

void drawCalendarBottomBar(const char* left, const char* middle, const char* right) {
  M5.Display.fillRect(0, 212, 320, 28, calTheme.bg);
  M5.Display.drawFastHLine(8, 212, 304, calTheme.border);
  useUIFont(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(calTheme.accent, calTheme.bg);
  M5.Display.drawString(left, 53, 227);
  M5.Display.drawString(middle, 160, 227);
  M5.Display.drawString(right, 267, 227);
}

static const char* const CAL_WEEKDAYS[] = {"日", "一", "二", "三", "四", "五", "六"};

void drawCalendarHeader(const String& text) {
  M5.Display.fillScreen(calTheme.bg);
  useUIMediumFont();
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(calTheme.title, calTheme.bg);
  // Device time zone (city + UTC offset) just left of the sync button.
  time_t now = time(nullptr);
  struct tm utc; gmtime_r(&now, &utc); utc.tm_isdst = -1;
  long offsetMin = (long)difftime(now, mktime(&utc)) / 60;
  struct tm local; localtime_r(&now, &local);
  if (local.tm_isdst > 0) offsetMin += 60;
  char offset[12];
  if (offsetMin % 60) snprintf(offset, sizeof(offset), "%+ld:%02ld", offsetMin / 60, labs(offsetMin % 60));
  else snprintf(offset, sizeof(offset), "%+ld", offsetMin / 60);
  String zone = timeZoneIndex == 0 ? String("UTC") : String(TIME_ZONES[timeZoneIndex].city) + " " + offset;
  useUIFont(1);
  int zoneWidth = M5.Display.textWidth(zone);
  M5.Display.setTextDatum(middle_right);
  M5.Display.setTextColor(calTheme.muted, calTheme.bg);
  M5.Display.drawString(zone, 256, 15);
  useUIMediumFont();
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(calTheme.title, calTheme.bg);
  M5.Display.setClipRect(0, 0, max(40, 256 - zoneWidth - 8), 30);
  M5.Display.drawString(text, 10, 3);
  M5.Display.clearClipRect();
  // Top-right "同步" button: tap to download the calendar right away.
  M5.Display.fillRoundRect(262, 3, 54, 24, 6, calTheme.panel);
  M5.Display.drawRoundRect(262, 3, 54, 24, 6, calTheme.accent);
  useUIFont(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(calTheme.accent, calTheme.panel);
  M5.Display.drawString("同步", 289, 15);
  M5.Display.drawFastHLine(8, 30, 304, calTheme.border);
}

void drawCalendarMessage(const String& text) {
  useUIFont(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(calTheme.muted, calTheme.bg);
  M5.Display.drawString(text, 160, 120);
}

String calTimeText(time_t t) {
  struct tm tm; localtime_r(&t, &tm);
  char b[8]; snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return String(b);
}

void drawCalendarDay() {
  struct tm d; localtime_r(&calViewDay, &d);
  time_t today = calLocalMidnight(time(nullptr));
  char head[40];
  snprintf(head, sizeof(head), "%d月%d日 週%s%s", d.tm_mon + 1, d.tm_mday, CAL_WEEKDAYS[d.tm_wday], calViewDay == today ? " · 今天" : "");
  drawCalendarHeader(head);
  int shown = 0, total = 0;
  for (int i = 0; i < calEventCount; ++i) if (calEventOnDay(calEvents[i], calViewDay)) ++total;
  const int maxRows = total > 5 ? 4 : 5;  // keep the last slot for "還有 N 項"
  for (int i = 0; i < calEventCount; ++i) {
    const CalEvent& e = calEvents[i];
    if (!calEventOnDay(e, calViewDay)) continue;
    if (shown >= maxRows) continue;
    int y = 34 + shown * 35;
    const bool isToday = false;  // day rows use the normal text colour
    uint16_t fill = shown & 1 ? calTheme.panelAlt : calTheme.panel;
    M5.Display.fillRoundRect(6, y, 308, 32, 6, fill);
    useUIFont(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(calTheme.accent, fill);
    M5.Display.drawString(e.allDay ? "全天" : calTimeText(e.start), 12, y + 7);
    M5.Display.setTextColor(isToday ? calTheme.todayText : calTheme.text, fill);
    M5.Display.setClipRect(62, y, 248, 32);
    String line = e.title;
    if (e.location[0]) line += String(" · ") + e.location;
    M5.Display.drawString(line, 62, y + 7);
    M5.Display.clearClipRect();
    ++shown;
  }
  if (!total) drawCalendarMessage(calError.length() ? calError : "今天沒有行程");
  else if (total > shown) {
    useUIFont(1); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(calTheme.accent, calTheme.bg);
    M5.Display.drawString("還有 " + String(total - shown) + " 項", 160, 34 + shown * 35 + 16);
  }
  drawCalendarBottomBar("前一天", "今天", "後一天");
}

void drawCalendarWeek() {
  struct tm d; localtime_r(&calViewDay, &d);
  time_t weekStart = calAddDays(calViewDay, -d.tm_wday);
  struct tm ws; localtime_r(&weekStart, &ws);
  time_t today = calLocalMidnight(time(nullptr));
  char head[40]; snprintf(head, sizeof(head), "%d月%d日 起一週", ws.tm_mon + 1, ws.tm_mday);
  drawCalendarHeader(head);
  for (int i = 0; i < 7; ++i) {
    time_t day = calAddDays(weekStart, i);
    struct tm t; localtime_r(&day, &t);
    int y = 33 + i * 26;
    const bool isToday = day == today;
    uint16_t fill = day == today ? calTheme.today : (i & 1 ? calTheme.panelAlt : calTheme.panel);
    M5.Display.fillRoundRect(6, y, 308, 24, 5, fill);
    useUIFont(1);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(calTheme.accent, fill);
    char label[16]; snprintf(label, sizeof(label), "%s %d/%d", CAL_WEEKDAYS[t.tm_wday], t.tm_mon + 1, t.tm_mday);
    M5.Display.drawString(label, 12, y + 12);
    String summary; int count = 0;
    for (int k = 0; k < calEventCount; ++k) {
      if (!calEventOnDay(calEvents[k], day)) continue;
      if (!count) summary = String(calEvents[k].allDay ? "" : calTimeText(calEvents[k].start) + " ") + calEvents[k].title;
      ++count;
    }
    if (count > 1) summary += " +" + String(count - 1);
    M5.Display.setTextColor(isToday ? calTheme.todayText : calTheme.text, fill);
    M5.Display.setClipRect(78, y, 232, 24);
    M5.Display.drawString(summary, 78, y + 12);
    M5.Display.clearClipRect();
  }
  drawCalendarBottomBar("上週", "本週", "下週");
}

void drawCalendarMonth() {
  struct tm d; localtime_r(&calViewDay, &d);
  time_t first = calLocalTime(d.tm_year + 1900, d.tm_mon + 1, 1);
  struct tm f; localtime_r(&first, &f);
  time_t today = calLocalMidnight(time(nullptr));
  char head[24]; snprintf(head, sizeof(head), "%d年%d月", d.tm_year + 1900, d.tm_mon + 1);
  drawCalendarHeader(head);
  useUIFont(1);
  M5.Display.setTextDatum(middle_center);
  for (int c = 0; c < 7; ++c) {
    M5.Display.setTextColor(c == 0 || c == 6 ? calTheme.weekend : calTheme.muted, calTheme.bg);
    M5.Display.drawString(CAL_WEEKDAYS[c], 23 + c * 45, 40);
  }
  time_t gridStart = calAddDays(first, -f.tm_wday);
  for (int cell = 0; cell < 42; ++cell) {
    time_t day = calAddDays(gridStart, cell);
    struct tm t; localtime_r(&day, &t);
    int x = 1 + (cell % 7) * 45, y = 50 + (cell / 7) * 27;
    bool inMonth = t.tm_mon == d.tm_mon;
    bool selected = day == calViewDay;
    uint16_t fill = day == today ? calTheme.today : (selected ? calTheme.panelAlt : calTheme.bg);
    if (fill != calTheme.bg) M5.Display.fillRoundRect(x + 1, y + 1, 43, 25, 5, fill);
    M5.Display.setTextColor(day == today ? calTheme.todayText : (inMonth ? calTheme.text : calTheme.outside), fill);
    M5.Display.drawString(String(t.tm_mday), x + 22, y + 11);
    int count = 0;
    for (int k = 0; k < calEventCount && count < 3; ++k) if (calEventOnDay(calEvents[k], day)) ++count;
    for (int i = 0; i < count; ++i) M5.Display.fillCircle(x + 22 - (count - 1) * 4 + i * 8, y + 22, 2, day == today ? calTheme.todayText : calTheme.dot);
  }
  drawCalendarBottomBar("上月", "本月", "下月");
}

void drawCalendar() {
  if (screenNow != Screen::Calendar) return;
  applyCalendarTheme();
  if (calView == CalView::Day) drawCalendarDay();
  else if (calView == CalView::Week) drawCalendarWeek();
  else drawCalendarMonth();
}

void refreshCalendarIfNeeded(bool force = false) {
  bool outside = calViewDay < calWindowStart || calAddDays(calViewDay, 1) > calWindowEnd;
  if (!force && calFetchedAt && !outside && millis() - calFetchedAt < 900000UL) return;
  drawCalendar();
  useUIFont(1); M5.Display.setTextDatum(top_right); M5.Display.setTextColor(calTheme.accent, calTheme.bg);
  M5.Display.fillRoundRect(262, 3, 54, 24, 6, calTheme.panel);
  M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(calTheme.accent, calTheme.panel);
  M5.Display.drawString("同步中", 289, 15);
  fetchCalendar(calViewDay);
}

void showCalendar() {
  screenNow = Screen::Calendar;
  calView = CalView::Day;
  calViewDay = calLocalMidnight(time(nullptr));
  refreshCalendarIfNeeded();
  drawCalendar();
}

void calendarNavigate(int direction) {
  if (direction == 0) calViewDay = calLocalMidnight(time(nullptr));
  else if (calView == CalView::Day) calViewDay = calAddDays(calViewDay, direction);
  else if (calView == CalView::Week) calViewDay = calAddDays(calViewDay, 7 * direction);
  else {
    struct tm d; localtime_r(&calViewDay, &d);
    calViewDay = calLocalTime(d.tm_year + 1900, d.tm_mon + 1 + direction, 1);
  }
  refreshCalendarIfNeeded();
  drawCalendar();
}

void handleCalendarTap(int x, int y) {
  if (y < 30 && x >= 256) {
    haptic(15);
    refreshCalendarIfNeeded(true);
    drawCalendar();
    if (calError.length()) drawCalendarMessage(calError);
    return;
  }
  if (y >= 210) {
    haptic(12);
    calendarNavigate(x < 107 ? -1 : (x < 214 ? 0 : 1));
    return;
  }
  if (calView == CalView::Month && y >= 50) {
    struct tm d; localtime_r(&calViewDay, &d);
    time_t first = calLocalTime(d.tm_year + 1900, d.tm_mon + 1, 1);
    struct tm f; localtime_r(&first, &f);
    int cell = constrain(x / 45, 0, 6) + constrain((y - 50) / 27, 0, 5) * 7;
    calViewDay = calAddDays(first, cell - f.tm_wday);
    calView = CalView::Day;
    haptic(12);
    drawCalendar();
  } else if (calView == CalView::Week && y >= 33) {
    struct tm d; localtime_r(&calViewDay, &d);
    calViewDay = calAddDays(calViewDay, constrain((y - 33) / 26, 0, 6) - d.tm_wday);
    calView = CalView::Day;
    haptic(12);
    drawCalendar();
  }
}

void handleCalendarSwipe(bool left) {
  haptic(12);
  if (calView == CalView::Day) calView = left ? CalView::Month : CalView::Week;
  else if (calView == CalView::Month && !left) calView = CalView::Day;
  else if (calView == CalView::Week && left) calView = CalView::Day;
  else return;
  drawCalendar();
}

void fillNightLightScreen() {
  M5.Display.fillScreen(M5.Display.color565((nightLightColor >> 16) & 255, (nightLightColor >> 8) & 255, nightLightColor & 255));
}

void enterNightLightScreen() {
  screenSleeping = false;
  screenNow = Screen::NightLight;
  screenNightBrightness = nightLightBrightness;
  fillNightLightScreen();
  M5.Display.setBrightness((uint8_t)max(1, screenNightBrightness * 255 / 100));
  updateAlarmBaseLights(millis() + 1000);
}

void exitNightLightScreen() {
  // Double tap ends both the screen night light and the LED night light.
  manualNightLightOverride = true;
  manualNightLightActive = false;
  screenNow = Screen::Clock;
  lastUserActivity = millis();
  m5::rtc_datetime_t now; getClockDateTime(&now);
  applyDisplayBrightness(now);
  updateAlarmBaseLights(millis() + 1000);
  drawClock(true); drawAstronaut();
}

void showNightBrightnessLabel() {
  fillNightLightScreen();
  uint16_t bg = M5.Display.color565((nightLightColor >> 16) & 255, (nightLightColor >> 8) & 255, nightLightColor & 255);
  uint32_t luma = ((nightLightColor >> 16) & 255) * 3 + ((nightLightColor >> 8) & 255) * 6 + (nightLightColor & 255);
  M5.Display.setTextColor(luma > 1200 ? TFT_BLACK : TFT_WHITE, bg);
  M5.Display.setTextDatum(middle_center);
  useUIMediumFont();
  M5.Display.drawString(String(screenNightBrightness) + "%", 160, 120);
  screenNightLabelUntil = millis() + 1200UL;
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  // Screen night light: long-press the screen to enter (while the manual LED
  // night light is on) and long-press again to leave. Sliding left/right
  // changes brightness. One press can only trigger one action, so the press
  // that enters never immediately leaves again.
  static uint32_t pressStartedAt = 0;
  static int16_t pressX = 0;
  static uint8_t pressBrightness = 0;
  static bool sliding = false;
  static bool pressHandled = false;
  if (t.wasPressed()) {
    pressStartedAt = millis(); pressX = t.x; pressBrightness = screenNightBrightness;
    sliding = false; pressHandled = false;
  }
  if (t.isPressed() && abs((int)t.x - pressX) > 12) sliding = true;
  bool longHeld = t.isPressed() && !sliding && !pressHandled && pressStartedAt
    && millis() - pressStartedAt >= 800UL;
  if (t.wasPressed() || t.wasReleased()) {
    char line[96];
    snprintf(line, sizeof(line), "%lu %s x=%d y=%d night=%d screen=%d\n", (unsigned long)millis(),
             t.wasPressed() ? "down" : "up", t.x, t.y, nightLedShowing ? 1 : 0, (int)screenNow);
    touchDebugLog += line;
    if (touchDebugLog.length() > 1500) touchDebugLog.remove(0, touchDebugLog.length() - 1500);
  }

  if (screenNow == Screen::NightLight) {
    if (sliding && t.isPressed() && !pressHandled) {
      int next = constrain((int)pressBrightness + ((int)t.x - pressX) * 100 / 280, 1, 100);
      if (next != screenNightBrightness) {
        screenNightBrightness = next;
        M5.Display.setBrightness((uint8_t)max(1, screenNightBrightness * 255 / 100));
        showNightBrightnessLabel();
      }
    }
    if (longHeld) {
      pressHandled = true;
      haptic(20);
      exitNightLightScreen();
      wakeTouchConsumed = true;  // ignore the rest of this press on the clock
      return;
    }
    if (screenNightLabelUntil && (int32_t)(millis() - screenNightLabelUntil) >= 0) {
      screenNightLabelUntil = 0;
      fillNightLightScreen();
    }
    return;
  }
  if (longHeld && nightLedShowing && screenNow == Screen::Clock && t.y < 210) {
    pressHandled = true;
    haptic(20);
    enterNightLightScreen();
    return;
  }
  // Calendar: long-press the clock face to open, long-press to go back.
  if (longHeld && !screenSleeping && alarmActive < 0 && screenNow == Screen::Clock && t.y < 210) {
    pressHandled = true;
    haptic(20);
    showCalendar();
    return;
  }
  if (screenNow == Screen::Calendar && !screenSleeping && !wakeTouchConsumed) {
    if (longHeld) {
      pressHandled = true;
      haptic(20);
      screenNow = Screen::Clock;
      drawClock(true); drawAstronaut();
      wakeTouchConsumed = true;
      return;
    }
    if (t.wasReleased() && !pressHandled) {
      int dx = (int)t.x - pressX;
      if (abs(dx) >= 60) handleCalendarSwipe(dx < 0);
      else if (!sliding) handleCalendarTap(t.x, t.y);
    }
    return;
  }
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
  if (screenNow == Screen::EmotionObservation || screenNow == Screen::EmotionRecords || screenNow == Screen::EmotionSettings || screenNow == Screen::EmotionReminder) { handleEmotionTouch(t); return; }
  if (screenNow == Screen::HassAssist) {
    if (t.wasPressed() && hassAssistReplyActive()) {
      // Any touch while waiting for / playing the reply stops it at once.
      haptic(15);
      interruptHassAssistReply();
      hassAssistTouchActive = false;
      if (t.y >= 210 && t.x >= 214) { hassAssistReturnToClock = false; screenNow = Screen::Clock; drawClock(true); drawAstronaut(); }
      return;
    }
    bool onMic = t.x > 105 && t.x < 215 && t.y < 175
      && sq((int)t.x - 160) + sq((int)t.y - 117) <= 60 * 60;
    if (t.wasPressed() && onMic) {
      hassAssistTouchActive = true;
      hassAssistTouchStartedAt = millis();
      hassAssistTouchLongStarted = false;
    }
    if (hassAssistTouchActive && t.isPressed() && !hassAssistTouchLongStarted
        && hassAssistVoiceMode == HASS_MODE_HOLD
        && millis() - hassAssistTouchStartedAt >= HASS_ASSIST_HOLD_START_MS) {
      // Mode 2: hold to talk, release to send.
      hassAssistTouchLongStarted = true;
      haptic(12);
      hassAssistHolding = true;
      startHassAssistPipeline();
      if (!hassAssistPipelineActive) hassAssistHolding = false;
    }
    if (t.wasReleased() && hassAssistTouchActive) {
      hassAssistTouchActive = false;
      if (hassAssistVoiceMode == HASS_MODE_HOLD) {
        if (hassAssistTouchLongStarted) {
          hassAssistHolding = false;
          hassAssistStopRequested = true;
          drawHassAssist();
        }
      } else if (hassAssistVoiceMode == HASS_MODE_WAKE) {
        // Mode 3: tap pauses / resumes always-on wake-word listening.
        hassAssistWakeWordPaused = !hassAssistWakeWordPaused;
        hassAssistWakeEmptyStreak = 0;
        hassAssistError = "";
        if (hassAssistWakeWordPaused) {
          hassAssistRestartAt = 0;
          if (hassAssistMicRunning) {
            hassAssistHolding = false;
            hassAssistStopRequested = true;
          } else if (hassAssistPipelineActive && hassAssistAudioHandlerId >= 0) {
            uint8_t endMarker = (uint8_t)hassAssistAudioHandlerId;
            hassAssistWebSocket.sendBIN(&endMarker, 1);
          }
          hassAssistState = HassAssistState::Paused;
        }
        else if (hassAssistSocketConnected) startHassAssistPipeline(true);
        drawHassAssist();
      } else if (hassAssistToggleListen) {
        // Mode 1: second tap sends. Ignore touch bounce right after the
        // first tap, which otherwise ended the recording after ~0.2 s.
        if (millis() - hassAssistTapStartedAt < 1000UL || !hassAssistMicRunning) {
          Serial.println("[assist] ignored tap (debounce)");
          return;
        }
        Serial.printf("[assist] tap stop after %lu ms\n", (unsigned long)(millis() - hassAssistTapStartedAt));
        hassAssistToggleListen = false;
        hassAssistHolding = false;
        hassAssistStopRequested = true;
        drawHassAssist();
      } else {
        // Mode 1: first tap starts listening.
        hassAssistTapStartedAt = millis();
        Serial.println("[assist] tap start");
        hassAssistToggleListen = true;
        hassAssistHolding = true;
        haptic(12);
        startHassAssistPipeline();
        if (!hassAssistPipelineActive) {
          hassAssistToggleListen = false;
          hassAssistHolding = false;
        }
      }
    } else if (t.wasReleased() && t.y >= 210 && t.x >= 214) {
      haptic(15);
      screenNow = Screen::Clock;
      drawClock(true); drawAstronaut();
    }
    return;
  }
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
  if (screenNow == Screen::MeditationSettings && meditationSettingsPage == 0) {
    // Preset times: drag the slider (1-60 min); save when released.
    static int8_t sliderRow = -1;
    if (t.wasPressed()) {
      int row = settingsRowAt(t.y);
      sliderRow = (row == 0 || row == 1) ? row : -1;
    }
    if (sliderRow >= 0 && (t.isPressed() || t.wasReleased())) {
      int next = sliderValueAt(t.x, 1, 60);
      if (next != meditationPresetMinutes[sliderRow]) {
        meditationPresetMinutes[sliderRow] = next;
        drawSettingsSliderRow(sliderRow, sliderRow ? "Preset time 2" : "Preset time 1", next, 1, 60, "min");
      }
      if (t.wasReleased()) { sliderRow = -1; saveSettings(); }
      return;
    }
  }
  if (screenNow == Screen::MeditationSettings && t.wasReleased()) {
    haptic(15);
    if (t.y >= 210) {
      if (t.x < 107 && meditationSettingsPage) { --meditationSettingsPage; showMeditationSettings(); }
      else if (t.x < 214 && meditationSettingsPage < 2) { ++meditationSettingsPage; showMeditationSettings(); }
      else if (t.x >= 214) showMeditation();
      return;
    }
    if (settingsRowAt(t.y) >= 0) {
      int row = settingsRowAt(t.y);
      if (!meditationSettingsPage) {
        if (row < 2) return;  // preset times are handled by the sliders above
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
    if (t.y >= 210) {
      if (t.x < 107 && menuPage) { menuPage = 0; showMenu(); }
      else if (t.x >= 107 && t.x < 214 && !menuPage) { menuPage = 1; showMenu(); }
      return;
    }
    int row = settingsRowAt(t.y);
    int item = row < 0 ? -1 : menuPage * 4 + row;
    if (item == 0) runWifiPortal();
    else if (item == 1) showFaces();
    else if (item == 2) { clockSettingsPage = 0; showSettings(); }
    else if (item == 3) showAlarms();
    else if (item == 4) { meditationSettingsPage = 0; showMeditationSettings(); }
    else if (item == 5) showFirmwareUpdate();
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
    int row = settingsRowAt(t.y);
    if (t.y >= 210) {
      if (t.x < 107) {
        if (clockSettingsPage) --clockSettingsPage; else syncTime();
      } else if (t.x < 214 && clockSettingsPage < 2) {
        ++clockSettingsPage;
      }
    } else if (row < 0) {
      return;
    } else if (clockSettingsPage == 0) {
      if (row == 0) { timeZoneIndex = (timeZoneIndex + 1) % TIME_ZONE_COUNT; syncTime(); }
      else if (row == 1) adaptiveBrightness = !adaptiveBrightness;
      else if (row == 2) dayBrightness = dayBrightness >= 100 ? 20 : dayBrightness + 10;
      else nightBrightness = nightBrightness >= 100 ? 5 : nightBrightness + 5;
    } else if (clockSettingsPage == 1) {
      if (row == 0) alarmVolume = alarmVolume >= 100 ? 10 : alarmVolume + 10;
      else if (row == 1) cycleScreenOffTime();
      else if (row == 2) use24HourTime = !use24HourTime;
      else flatVirtualButtonsEnabled = !flatVirtualButtonsEnabled;
    } else {
      if (row == 0) nightLightEnabled = !nightLightEnabled;
      else if (row == 1) {
        const uint32_t colors[] = {0xFFF0C8, 0xFFFFFF, 0xFFD080, 0x80B8FF, 0xFF9090};
        int next = 0; for (int i=0;i<5;++i) if (nightLightColor==colors[i]) next=(i+1)%5;
        nightLightColor=colors[next];
      } else if (row == 2) nightLightBrightness = nightLightBrightness >= 100 ? 5 : nightLightBrightness + 5;
      else nightLightMode = (nightLightMode + 1) % 3;
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
  device["name"] = deviceName;
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
  doc["weekday"] = weekdayText[(uint8_t)now.date.weekDay % 7];
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
  device["name"] = deviceName;
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
  JsonObject emotionReminder = doc["emotion_reminder"].to<JsonObject>();
  emotionReminder["mode"] = emotionReminderMode;
  emotionReminder["interval_minutes"] = emotionReminderIntervalMinutes;
  emotionReminder["window_start_minutes"] = emotionReminderWindowStart;
  emotionReminder["window_end_minutes"] = emotionReminderWindowEnd;
  emotionReminder["vibration"] = emotionReminderVibration;
  emotionReminder["sound_enabled"] = emotionReminderSound;
  emotionReminder["duration_seconds"] = emotionReminderDurationSeconds;
  emotionReminder["sound"] = emotionReminderSoundChoice;
  emotionReminder["volume"] = emotionReminderVolume;
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
    if (presets.size() > 0) meditationPresetMinutes[0] = constrain(presets[0].as<int>(), 1, 60);
    if (presets.size() > 1) meditationPresetMinutes[1] = constrain(presets[1].as<int>(), 1, 60);
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
  JsonObjectConst emotionReminder = root["emotion_reminder"];
  if (!emotionReminder.isNull()) {
    if (emotionReminder["mode"].is<int>()) emotionReminderMode = constrain(emotionReminder["mode"].as<int>(), 0, 2);
    if (emotionReminder["interval_minutes"].is<int>()) {
      int requested = emotionReminder["interval_minutes"].as<int>();
      const uint16_t validIntervals[] = {10, 15, 30, 60, 120, 180, 240};
      for (uint16_t interval : validIntervals) if (requested == interval) emotionReminderIntervalMinutes = interval;
    }
    if (emotionReminder["window_start_minutes"].is<int>()) emotionReminderWindowStart = constrain(emotionReminder["window_start_minutes"].as<int>(), 0, 1439);
    if (emotionReminder["window_end_minutes"].is<int>()) emotionReminderWindowEnd = constrain(emotionReminder["window_end_minutes"].as<int>(), 0, 1439);
    if (emotionReminderWindowEnd < emotionReminderWindowStart) emotionReminderWindowEnd = emotionReminderWindowStart;
    if (emotionReminder["vibration"].is<bool>()) emotionReminderVibration = emotionReminder["vibration"];
    if (emotionReminder["sound_enabled"].is<bool>()) emotionReminderSound = emotionReminder["sound_enabled"];
    if (emotionReminder["duration_seconds"].is<int>()) {
      int duration = emotionReminder["duration_seconds"].as<int>();
      if (duration == 10 || duration == 30 || duration == 60 || duration == 120) emotionReminderDurationSeconds = duration;
    }
    if (emotionReminder["sound"].is<int>()) emotionReminderSoundChoice = constrain(emotionReminder["sound"].as<int>(), 0, 3);
    if (emotionReminder["volume"].is<int>()) emotionReminderVolume = constrain(emotionReminder["volume"].as<int>(), 5, 100);
    emotionLastReminderMinuteKey = -1;
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

void sendSettingsPage(const String& message = "", const String& requestedPage = "", const String& requestedLanguage = "") {
  String pageId = requestedPage.length() ? requestedPage : settingsServer.arg("page");
  String language = requestedLanguage.length() ? requestedLanguage : settingsServer.arg("lang");
  if (pageId != "wifi" && pageId != "clock" && pageId != "alarms" && pageId != "meditation" && pageId != "emotion" && pageId != "mqtt" && pageId != "hass" && pageId != "companion" && pageId != "calendar" && pageId != "firmware") pageId = "clock";
  bool zh = language == "zh" || (!language.length());
  auto tr = [zh](const char* en, const char* zhText) -> String { return zh ? String(zhText) : String(en); };
  String page;
  page.reserve(24000);
  page = "<!doctype html><html lang='" + String(zh ? "zh-Hant" : "en") + "'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Space Clock</title><style>html{color-scheme:dark}*{box-sizing:border-box}body{font-family:system-ui,-apple-system,sans-serif;background:#08111f;color:#eef4ff;max-width:760px;margin:auto;padding:18px}"
         "header{display:flex;align-items:center;justify-content:space-between;gap:12px}h1{color:#65b9ff;font-size:25px;margin:8px 0}h2{font-size:19px;margin:24px 0 8px}.muted{color:#aabbd0;font-size:14px}.tabs{display:flex;flex-wrap:wrap;gap:8px;margin:18px 0}.tabs a,.lang{border:1px solid #344b63;border-radius:999px;padding:8px 12px;color:#c8d9ee;text-decoration:none;font-size:14px}.tabs a.active{background:#1688e5;border-color:#1688e5;color:white}.lang{white-space:nowrap}.panel{background:#101d2e;border:1px solid #263b52;border-radius:16px;padding:16px}.field{display:block;margin-top:15px;font-size:15px}.field input:not([type=checkbox]):not([type=radio]),.field select{display:block;width:100%;padding:11px;margin-top:6px;border-radius:9px;border:1px solid #52657a;background:#142236;color:white;font-size:16px}.field input[type=range]{padding:0}.field input[type=color]{height:48px;padding:5px}.check{display:flex;align-items:center;gap:9px;margin:15px 0}.check input{flex:none;width:20px;height:20px;margin:0;accent-color:#1688e5}fieldset.field{border:1px solid #52657a;border-radius:12px;padding:4px 14px 2px}fieldset.field legend{padding:0 6px}.card{background:#0b1727;border:1px solid #344b63;border-radius:12px;padding:12px;margin:12px 0}.card summary{cursor:pointer;font-weight:700}.days{display:flex;flex-wrap:wrap;gap:9px;margin-top:12px}.days label{white-space:nowrap}.btn,button{display:block;width:100%;padding:13px;margin-top:18px;border:0;border-radius:10px;background:#1688e5;color:white;font-size:16px;text-align:center;text-decoration:none;cursor:pointer}.btn.secondary{background:#20354e}.ok{color:#70e39a}.warn{color:#ffb454;background:#2a1f0e;border:1px solid #6b4b1a;border-radius:9px;padding:10px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}@media(max-width:520px){body{padding:12px}.panel{padding:13px}.grid{grid-template-columns:1fr}}</style></head><body>";
  m5::rtc_datetime_t webNow; getClockDateTime(&webNow);
  char webTime[24]; snprintf(webTime, sizeof(webTime), "%04d-%02d-%02d %02d:%02d:%02d", webNow.date.year, webNow.date.month, webNow.date.date, webNow.time.hours, webNow.time.minutes, webNow.time.seconds);
  page += "<header><div><h1>" + tr("Space Clock settings", "太空時鐘設定") + "</h1><div class='muted'>" + tr("Device", "設備") + ": <b>" + htmlEscape(deviceName) + "</b> · " + tr("Network name", "網路名稱") + ": <b>" + networkHostname() + "</b><br>" + tr("IP", "設備 IP") + ": <b>" + WiFi.localIP().toString() + "</b> · " + tr("Device time", "裝置時間") + ": <b>" + webTime + "</b> (" + TIME_ZONES[timeZoneIndex].city + ")</div></div>";
  page += "<a class='lang' href='/?page=" + pageId + "&lang=" + String(zh ? "en" : "zh") + "'>" + tr("中文", "English") + "</a></header>";
  const char* pageIds[] = {"wifi", "clock", "alarms", "meditation", "emotion", "mqtt", "hass", "companion", "calendar", "firmware"};
  const char* tabEn[] = {"Wi-Fi", "Clock", "Alarms", "Meditation", "Emotion journal", "MQTT", "HASS Assist", "Companion", "Calendar", "Firmware"};
  const char* tabZh[] = {"Wi-Fi 網路", "時鐘與小夜燈", "鬧鐘", "靜心時鐘", "情緒觀察", "MQTT", "HASS 語音助理", "Companion", "日曆", "韌體更新"};
  page += "<nav class='tabs'>";
  for (int i = 0; i < 10; ++i) page += "<a class='" + String(pageId == pageIds[i] ? "active" : "") + "' href='/?page=" + pageIds[i] + "&lang=" + (zh ? "zh" : "en") + "'>" + tr(tabEn[i], tabZh[i]) + "</a>";
  page += "</nav>";
  if (message.length()) page += "<p class='ok'>" + htmlEscape(message) + "</p>";
  if (pageId == "emotion") {
    page += "<section class='panel'><h2>" + tr("Emotion journal API account", "情緒觀察 API 帳號") + "</h2>";
    page += "<p class='muted'>" + tr("Save the API URL below before signing in. The password is sent to the configured HTTPS API for login and is not saved. The returned bearer token is stored in this Core2's Preferences so entries can be submitted later. This device's settings page uses local HTTP, so only sign in on a trusted Wi-Fi network; use a dedicated account.", "請先儲存下方 API 網址再登入。密碼只會透過 HTTPS 傳給 API 登入，不會儲存；Bearer token 會保存在這台 Core2 供稍後送出紀錄。設備設定頁使用區域網路 HTTP，請只在可信任的 Wi-Fi 登入，並使用專用帳號。") + "</p>";
    if (emotionApiToken.length()) {
      bool apiOnline = emotionApiConnected();
      page += "<p class='" + String(apiOnline ? "ok" : "muted") + "'>● " + (apiOnline ? tr("Database connected", "資料庫已連線") : tr("Saved login; connection not verified", "已儲存登入；尚未確認連線")) + "</p>";
      page += "<p class='ok'>" + tr("Signed in as ", "目前登入帳號：") + htmlEscape(emotionApiIdentity) + "</p>";
      page += "<form method='post' action='/emotion-check'><input type='hidden' name='lang' value='" + String(zh ? "zh" : "en") + "'><button class='btn secondary' type='submit'>" + tr("Check database connection", "檢查資料庫連線") + "</button></form>";
      page += "<form method='post' action='/emotion-logout'><input type='hidden' name='lang' value='" + String(zh ? "zh" : "en") + "'><button class='btn secondary' type='submit'>" + tr("Forget this device's API login", "清除此設備的 API 登入") + "</button></form>";
    } else {
      page += "<form method='post' action='/emotion-login'><input type='hidden' name='lang' value='" + String(zh ? "zh" : "en") + "'><label class='field'>" + tr("Account email", "帳號 Email") + "<input type='email' name='emotionIdentity' autocomplete='username' required></label><label class='field'>" + tr("Password", "密碼") + "<input type='password' name='emotionPassword' autocomplete='current-password' required></label><button type='submit'>" + tr("Sign in to emotion API", "登入情緒觀察 API") + "</button></form>";
    }
    page += "</section>";
  }
  page += "<form method='post' action='/save'><input type='hidden' name='page' value='" + pageId + "'><input type='hidden' name='lang' value='" + String(zh ? "zh" : "en") + "'><section class='panel'>";

  if (pageId == "wifi") {
    page += "<h2>" + tr("Device identity", "設備識別") + "</h2><label class='field'>" + tr("Device name", "設備名稱") + "<input name='deviceName' maxlength='32' value='" + htmlEscape(deviceName) + "'></label><p class='muted'>" + tr("The network hostname is derived from this name and a unique chip ID. Saving a changed name restarts Wi-Fi so your router can update its device list.", "網路主機名稱會由此名稱加上晶片唯一編號產生。變更名稱並儲存後，設備會重新啟動 Wi-Fi，讓路由器更新設備清單。") + "</p>";
    page += "<h2>" + tr("Saved Wi-Fi networks", "已儲存的 Wi-Fi 網路") + "</h2><p class='muted'>" + tr("Up to 10 networks. Passwords stay on this Core2 and are never displayed. Leave a password blank to keep it unchanged.", "最多儲存 10 組網路。密碼只保存在 Core2，不會顯示；密碼留白即可保留原密碼。") + "</p>";
    for (int i = 0; i < SAVED_WIFI_COUNT; ++i) {
      page += "<div class='card'><b>Wi-Fi " + String(i + 1) + "</b><label class='field'>SSID<input name='wifiS" + String(i) + "' value='" + htmlEscape(savedWifiSsids[i]) + "'></label>";
      page += "<label class='field'>" + tr("New password", "新密碼") + "<input type='password' name='wifiP" + String(i) + "' placeholder='" + tr("Leave blank to keep current", "留白以保留目前密碼") + "'></label><label class='check'><input type='checkbox' name='wifiD" + String(i) + "'>" + tr("Remove this network", "移除此網路") + "</label></div>";
    }
  } else if (pageId == "clock") {
    page += "<h2>" + tr("Clock display", "時鐘顯示") + "</h2>";
    page += "<label class='field'>" + tr("Time zone (major city)", "時區（主要城市）") + "<select name='timeZone'>";
    for (int i = 0; i < TIME_ZONE_COUNT; ++i) page += "<option value='" + String(i) + "'" + (i == timeZoneIndex ? " selected" : "") + ">" + TIME_ZONES[i].city + "</option>";
    page += "</select></label><label class='field'>" + tr("Clock face", "表盤") + "<select name='face'>";
    const char* faceEn[] = {"Space", "Flip clock", "Matrix rain"}; const char* faceZh[] = {"太空漫遊", "翻頁時鐘", "Matrix code rain"};
    for (int i = 0; i < 3; ++i) page += "<option value='" + String(i) + "'" + (i == (int)clockFace ? " selected" : "") + ">" + tr(faceEn[i], faceZh[i]) + "</option>";
    page += "</select></label>";
    page += "<h2>" + tr("Matrix rain appearance", "Matrix 雨幕外觀") + "</h2><p class='muted'>" + tr("Lower speed and density create a calmer background. Glass opacity controls how much code is visible behind the clock.", "降低速度與密度可讓背景更平靜；時鐘框不透明度控制背景字元的透出程度。") + "</p>";
    page += "<label class='field'>" + tr("Rain speed", "雨滴速度") + ": <output id='matrixSpeedOut'>" + String(matrixRainSpeed) + "</output><input type='range' min='10' max='100' step='5' name='matrixSpeed' value='" + String(matrixRainSpeed) + "' oninput='matrixSpeedOut.value=this.value'></label>";
    page += "<label class='field'>" + tr("Rain density", "雨幕密度") + ": <output id='matrixDensityOut'>" + String(matrixRainDensity) + "%</output><input type='range' min='10' max='100' step='5' name='matrixDensity' value='" + String(matrixRainDensity) + "' oninput='matrixDensityOut.value=this.value+\"%\"'></label>";
    page += "<label class='field'>" + tr("Glyph size", "字元大小") + "<select name='matrixSize'><option value='1'" + String(matrixGlyphScale == 1 ? " selected" : "") + ">" + tr("Small", "小") + "</option><option value='2'" + String(matrixGlyphScale == 2 ? " selected" : "") + ">" + tr("Large", "大") + "</option></select></label>";
    page += "<label class='field'>" + tr("Rain color", "雨幕顏色") + "<input type='color' name='matrixColor' value='" + colorHex(matrixRainColor) + "'></label>";
    page += "<label class='field'>" + tr("Clock glass opacity", "時鐘框半透明度") + ": <output id='matrixGlassOut'>" + String(matrixGlassOpacity) + "%</output><input type='range' min='15' max='90' step='5' name='matrixGlass' value='" + String(matrixGlassOpacity) + "' oninput='matrixGlassOut.value=this.value+\"%\"'></label>";
    page += "<h2>" + tr("Screen and brightness", "螢幕與亮度") + "</h2>";
    page += "<label class='field'>" + tr("Time format", "時間格式") + "<select name='time24'><option value='1'" + String(use24HourTime ? " selected" : "") + ">24-hour</option><option value='0'" + String(!use24HourTime ? " selected" : "") + ">12-hour</option></select></label>";
    page += "<label class='check'><input type='checkbox' name='flatButtons'" + String(flatVirtualButtonsEnabled ? " checked" : "") + ">" + tr("Enable the three virtual buttons while the device is lying flat", "裝置平放時啟用三顆虛擬按鈕") + "</label>";
    page += "<label class='check'><input type='checkbox' name='autoBrightness'" + String(adaptiveBrightness ? " checked" : "") + ">" + tr("Automatic brightness (day 07:00–20:59)", "自動亮度（白天 07:00–20:59）") + "</label>";
    page += "<div class='grid'><label class='field'>" + tr("Day brightness", "白天亮度") + ": <output id='dayOut'>" + String(dayBrightness) + "%</output><input type='range' min='10' max='100' step='5' name='dayBrightness' value='" + String(dayBrightness) + "' oninput='dayOut.value=this.value+\"%\"'></label>";
    page += "<label class='field'>" + tr("Night brightness", "夜間亮度") + ": <output id='nightOut'>" + String(nightBrightness) + "%</output><input type='range' min='5' max='100' step='5' name='nightBrightness' value='" + String(nightBrightness) + "' oninput='nightOut.value=this.value+\"%\"'></label></div>";
    page += "<label class='field'>" + tr("Screen automatically turns off after", "螢幕自動關閉時間") + "<select name='screenOff'>";
    const uint16_t offValues[] = {0,30,60,300,600,1800}; const char* offEn[] = {"Never","30 seconds","1 minute","5 minutes","10 minutes","30 minutes"}; const char* offZh[] = {"永不","30 秒","1 分鐘","5 分鐘","10 分鐘","30 分鐘"};
    for (int i = 0; i < 6; ++i) page += "<option value='" + String(offValues[i]) + "'" + (screenOffSeconds == offValues[i] ? " selected" : "") + ">" + tr(offEn[i], offZh[i]) + "</option>";
    page += "</select></label>";
    page += "<h2>" + tr("Night light", "小夜燈") + "</h2><p class='muted'>" + tr("Long-press the gear icon on the clock to manually toggle the light. This setting controls automatic lighting while the display is off.", "在時鐘首頁長按齒輪可手動開關小夜燈。以下設定控制螢幕關閉時的自動亮燈。") + "</p>";
    page += "<label class='check'><input type='checkbox' name='nightLight'" + String(nightLightEnabled ? " checked" : "") + ">" + tr("Enable automatic Bottom2 night light while screen is off", "螢幕關閉時自動開啟 Bottom2 小夜燈") + "</label>";
    page += "<div class='grid'><label class='field'>" + tr("LED color", "LED 顏色") + "<input type='color' name='nightColor' value='" + colorHex(nightLightColor) + "'></label>";
    page += "<label class='field'>" + tr("LED brightness", "LED 亮度") + ": <output id='nightLedOut'>" + String(nightLightBrightness) + "%</output><input type='range' min='1' max='100' name='nightLedBrightness' value='" + String(nightLightBrightness) + "' oninput='nightLedOut.value=this.value+\"%\"'></label></div>";
    page += "<label class='field'>" + tr("Automatic mode", "自動模式") + "<select name='nightLightMode'><option value='0'" + String(nightLightMode == 0 ? " selected" : "") + ">" + tr("Stay on while screen is off", "螢幕關閉期間持續亮起") + "</option><option value='1'" + String(nightLightMode == 1 ? " selected" : "") + ">" + tr("Turn off after set time", "指定時間後關閉") + "</option><option value='2'" + String(nightLightMode == 2 ? " selected" : "") + ">" + tr("Fade out after set time", "指定時間後逐漸熄滅") + "</option></select></label>";
    page += "<label class='field'>" + tr("On time (seconds)", "持續時間（秒）") + "<input type='number' min='5' max='3600' name='nightLightSeconds' value='" + String(nightLightSeconds) + "'></label>";
  } else if (pageId == "alarms") {
    page += "<h2>" + tr("Alarm sound", "鬧鐘音效") + "</h2><label class='field'>" + tr("Volume", "音量") + ": <output id='volumeOut'>" + String(alarmVolume) + "%</output><input type='range' min='10' max='100' step='5' name='alarmVolume' value='" + String(alarmVolume) + "' oninput='volumeOut.value=this.value+\"%\"'></label>";
    const char* soundsEn[] = {"Da Ban", "Chime", "Stream", "Water drop"}; const char* soundsZh[] = {"打版", "磬聲", "流水聲", "水滴聲"};
    page += "<label class='field'>" + tr("Alarm sound", "鬧鐘音效") + "<select id='alarmSound' name='alarmSound'>";
    for (int i = 0; i < 4; ++i) page += "<option value='" + String(i) + "'" + (alarmSound == i ? " selected" : "") + ">" + tr(soundsEn[i], soundsZh[i]) + "</option>";
    page += "</select></label><button class='btn secondary' type='button' onclick='fetch(\"/preview?sound=\"+document.getElementById(\"alarmSound\").value+\"&volume=\"+document.querySelector(\"[name=alarmVolume]\").value)'>" + tr("Preview alarm sound on Core2", "在 Core2 預聽鬧鐘音效") + "</button>";
    page += "<h2>" + tr("Alarm light reminder", "鬧鐘燈光提醒") + "</h2><label class='check'><input type='checkbox' name='alarmLight'" + String(alarmLightEnabled ? " checked" : "") + ">" + tr("Enable Bottom2 alarm lighting", "啟用 Bottom2 鬧鐘燈光") + "</label>";
    page += "<div class='grid'><label class='field'>" + tr("Color", "顏色") + "<input type='color' name='alarmLightColor' value='" + colorHex(alarmLightColor) + "'></label><label class='field'>" + tr("Brightness", "亮度") + ": <output id='alarmLedOut'>" + String(alarmLightBrightness) + "%</output><input type='range' min='1' max='100' name='alarmLightBrightness' value='" + String(alarmLightBrightness) + "' oninput='alarmLedOut.value=this.value+\"%\"'></label></div>";
    const char* alarmModesEn[] = {"Continuous breathing", "Continuous fast flash", "Three breathing cycles", "Three fast-flash cycles"}; const char* alarmModesZh[] = {"持續呼吸", "持續快閃", "三輪呼吸", "三輪快閃"};
    page += "<label class='field'>" + tr("Light mode", "燈光模式") + "<select name='alarmLightMode'>";
    for (int i = 0; i < 4; ++i) page += "<option value='" + String(i) + "'" + (alarmLightMode == i ? " selected" : "") + ">" + tr(alarmModesEn[i], alarmModesZh[i]) + "</option>";
    page += "</select></label><h2>" + tr("Alarm schedules", "鬧鐘排程") + "</h2><p class='muted'>" + tr("Select a time and repeat days. No selected day means a one-time alarm.", "設定時間及重複星期；未選星期代表單次鬧鐘。") + "</p>";
    const char* daysEn[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}; const char* daysZh[] = {"日","一","二","三","四","五","六"};
    for (int i = 0; i < ALARM_COUNT; ++i) {
      char tv[6]; snprintf(tv, sizeof(tv), "%02u:%02u", alarms[i].hour, alarms[i].minute);
      page += "<details class='card'" + String(i < 4 ? " open" : "") + "><summary>" + tr("Alarm ", "鬧鐘 ") + String(i + 1) + " — " + tv + (alarms[i].enabled ? " (ON)" : " (OFF)") + "</summary>";
      page += "<label class='field'>" + tr("Time", "時間") + "<input type='time' name='a" + String(i) + "_time' value='" + tv + "'></label><label class='check'><input type='checkbox' name='a" + String(i) + "_on'" + String(alarms[i].enabled ? " checked" : "") + ">" + tr("Enabled", "啟用") + "</label><div class='days'>";
      for (int d = 0; d < 7; ++d) page += "<label><input type='checkbox' name='a" + String(i) + "_d" + String(d) + "'" + String((alarms[i].weekdays & (1 << d)) ? " checked" : "") + ">" + tr(daysEn[d], daysZh[d]) + "</label>";
      page += "</div></details>";
    }
  } else if (pageId == "meditation") {
    page += "<h2>" + tr("Meditation timer", "靜心時鐘") + "</h2><div class='grid'><label class='field'>" + tr("Preset time 1 (minutes)", "預設時間 1（分鐘）") + "<input type='number' min='1' max='60' name='medPreset1' value='" + String(meditationPresetMinutes[0]) + "'></label><label class='field'>" + tr("Preset time 2 (minutes)", "預設時間 2（分鐘）") + "<input type='number' min='1' max='60' name='medPreset2' value='" + String(meditationPresetMinutes[1]) + "'></label></div>";
    page += "<label class='check'><input type='checkbox' name='medSoundEnabled'" + String(meditationSoundEnabled ? " checked" : "") + ">" + tr("Enable sound reminders", "啟用聲音提醒") + "</label>";
    const char* soundsEn[] = {"Da Ban", "Chime", "Stream", "Water drop"}; const char* soundsZh[] = {"打版", "磬聲", "流水聲", "水滴聲"};
    page += "<label class='field'>" + tr("Start sound", "開始計時音效") + "<select id='medStartSound' name='medStartSound'>";
    for (int i = 0; i < 4; ++i) page += "<option value='" + String(i) + "'" + (meditationStartSound == i ? " selected" : "") + ">" + tr(soundsEn[i], soundsZh[i]) + "</option>";
    page += "</select></label><label class='field'>" + tr("Start volume", "開始音量") + ": <output id='medStartOut'>" + String(meditationStartVolume) + "%</output><input id='medStartVolume' type='range' min='5' max='100' step='5' name='medStartVolume' value='" + String(meditationStartVolume) + "' oninput='medStartOut.value=this.value+\"%\"'></label><button class='btn secondary' type='button' onclick='previewSound(\"start\")'>" + tr("Preview start sound on Core2", "在 Core2 預聽開始音效") + "</button>";
    page += "<label class='field'>" + tr("Time-up sound", "時間到音效") + "<select id='medEndSound' name='medEndSound'>";
    for (int i = 0; i < 4; ++i) page += "<option value='" + String(i) + "'" + (meditationEndSound == i ? " selected" : "") + ">" + tr(soundsEn[i], soundsZh[i]) + "</option>";
    page += "</select></label><label class='field'>" + tr("Time-up volume", "時間到音量") + ": <output id='medEndOut'>" + String(meditationEndVolume) + "%</output><input id='medEndVolume' type='range' min='5' max='100' step='5' name='medEndVolume' value='" + String(meditationEndVolume) + "' oninput='medEndOut.value=this.value+\"%\"'></label><button class='btn secondary' type='button' onclick='previewSound(\"end\")'>" + tr("Preview time-up sound on Core2", "在 Core2 預聽時間到音效") + "</button>";
    page += "<label class='check'><input type='checkbox' name='medLightEnabled'" + String(meditationLightEnabled ? " checked" : "") + ">" + tr("Enable lighting effects", "啟用燈光效果") + "</label><label class='check'><input type='checkbox' name='medNoiseEnabled'" + String(meditationNoiseEnabled ? " checked" : "") + ">" + tr("Play background sound during countdown", "倒數期間播放白底燥音效") + "</label>";
    const char* noiseEn[] = {"Stream", "Rain (original)", "Summer night insects (original)"}; const char* noiseZh[] = {"流水聲", "雨聲（原創）", "夏夜蟲鳴（原創）"};
    page += "<label class='field'>" + tr("Background sound", "白底燥音效") + "<select id='medNoise' name='medNoise'>";
    for (int i = 0; i < 3; ++i) page += "<option value='" + String(i) + "'" + (meditationNoise == i ? " selected" : "") + ">" + tr(noiseEn[i], noiseZh[i]) + "</option>";
    page += "</select></label><label class='field'>" + tr("Background volume", "白底燥音量") + ": <output id='medNoiseOut'>" + String(meditationNoiseVolume) + "%</output><input id='medNoiseVolume' type='range' min='5' max='80' step='5' name='medNoiseVolume' value='" + String(meditationNoiseVolume) + "' oninput='medNoiseOut.value=this.value+\"%\"'></label><button class='btn secondary' type='button' onclick='fetch(\"/preview-ambient?sound=\"+document.getElementById(\"medNoise\").value+\"&volume=\"+document.getElementById(\"medNoiseVolume\").value)'>" + tr("Preview background sound on Core2", "在 Core2 預聽白底燥音效") + "</button>";
  } else if (pageId == "emotion") {
    page += "<h2>" + tr("API connection", "API 連線") + "</h2><p class='muted'>" + tr("PocketBase API used by the provided emotion journal specification. HTTPS is required.", "依照情緒觀察規格使用 PocketBase API，必須使用 HTTPS。") + "</p>";
    page += "<label class='field'>" + tr("API base URL", "API 基礎網址") + "<input type='url' name='emotionApiBase' value='" + htmlEscape(emotionApiBase) + "' placeholder='https://emotion.theoakhouse.org' required></label>";
    page += "<h2>" + tr("Fill-in reminders", "填寫提醒") + "</h2><label class='field'>" + tr("Reminder schedule", "提醒排程") + "<select name='emotionReminderMode'>";
    const char* reminderModesEn[] = {"Disabled", "Scheduled interval", "At fixed alarm times"};
    const char* reminderModesZh[] = {"關閉", "整點提醒", "固定時間提醒"};
    for (int i = 0; i < 3; ++i) page += "<option value='" + String(i) + "'" + String(emotionReminderMode == i ? " selected" : "") + ">" + tr(reminderModesEn[i], reminderModesZh[i]) + "</option>";
    page += "</select></label><h3>" + tr("Scheduled interval window", "整點提醒區間") + "</h3><div class='grid'>";
    page += "<label class='field'>" + tr("Start time", "開始時間") + "<input type='time' name='emotionWindowStart' value='" + emotionReminderTimeText(emotionReminderWindowStart) + "'></label>";
    page += "<label class='field'>" + tr("End time", "結束時間") + "<input type='time' name='emotionWindowEnd' value='" + emotionReminderTimeText(emotionReminderWindowEnd) + "'></label></div>";
    page += "<label class='field'>" + tr("Reminder interval", "提醒間隔") + "<select name='emotionInterval'>";
    const uint16_t reminderIntervals[] = {10, 15, 30, 60, 120, 180, 240};
    const char* reminderIntervalsEn[] = {"10 minutes", "15 minutes", "30 minutes", "1 hour", "2 hours", "3 hours", "4 hours"};
    const char* reminderIntervalsZh[] = {"10 分鐘", "15 分鐘", "30 分鐘", "1 小時", "2 小時", "3 小時", "4 小時"};
    for (int i = 0; i < 7; ++i) page += "<option value='" + String(reminderIntervals[i]) + "'" + String(emotionReminderIntervalMinutes == reminderIntervals[i] ? " selected" : "") + ">" + tr(reminderIntervalsEn[i], reminderIntervalsZh[i]) + "</option>";
    page += "</select></label><p class='muted'>" + tr("The schedule starts at the selected start time and repeats by the chosen interval until the end time, inclusive.", "從開始時間起，依選定間隔提醒，直到結束時間（含）為止。") + "</p>";
    page += "<h3>" + tr("Fixed alarm times", "固定時間提醒") + "</h3><p class='muted'>" + tr("Fixed-time reminders support up to three times per day. Leave a time unchecked to disable that slot.", "固定時間每天最多三個時段；取消勾選即可停用該時段。") + "</p>";
    for (int i = 0; i < 3; ++i) {
      bool enabled = emotionReminderTimes[i] != 0xFFFF;
      uint16_t minutes = enabled ? emotionReminderTimes[i] : (i == 0 ? 600 : (i == 1 ? 900 : 1200));
      char timeValue[6]; snprintf(timeValue, sizeof(timeValue), "%02u:%02u", minutes / 60, minutes % 60);
      page += "<div class='grid'><label class='field'>" + tr("Reminder ", "時段 ") + String(i + 1) + "<input type='time' name='emotionTime" + String(i) + "' value='" + timeValue + "'></label><label class='check'><input type='checkbox' name='emotionTimeOn" + String(i) + "'" + String(enabled ? " checked" : "") + ">" + tr("Enabled", "啟用") + "</label></div>";
    }
    page += "<h2>" + tr("Reminder behavior", "提醒方式") + "</h2><label class='check'><input type='checkbox' name='emotionVibration'" + String(emotionReminderVibration ? " checked" : "") + ">" + tr("Vibration", "振動提醒") + "</label><label class='check'><input type='checkbox' name='emotionSoundEnabled'" + String(emotionReminderSound ? " checked" : "") + ">" + tr("Alarm sound", "鬧鐘聲音提醒") + "</label>";
    page += "<label class='field'>" + tr("Reminder duration", "提醒持續時間") + "<select name='emotionDuration'>";
    const uint8_t reminderDurations[] = {10, 30, 60, 120}; const char* durationEn[] = {"10 seconds", "30 seconds", "1 minute", "2 minutes"}; const char* durationZh[] = {"10 秒", "30 秒", "1 分鐘", "2 分鐘"};
    for (int i = 0; i < 4; ++i) page += "<option value='" + String(reminderDurations[i]) + "'" + String(emotionReminderDurationSeconds == reminderDurations[i] ? " selected" : "") + ">" + tr(durationEn[i], durationZh[i]) + "</option>";
    page += "</select></label>";
    const char* soundsEn[] = {"Da Ban", "Chime", "Stream", "Water drop"}; const char* soundsZh[] = {"打版", "磬聲", "流水聲", "水滴聲"};
    page += "<label class='field'>" + tr("Reminder sound", "提醒鈴聲") + "<select id='emotionSoundChoice' name='emotionSoundChoice'>";
    for (int i = 0; i < 4; ++i) page += "<option value='" + String(i) + "'" + String(emotionReminderSoundChoice == i ? " selected" : "") + ">" + tr(soundsEn[i], soundsZh[i]) + "</option>";
    page += "</select></label><label class='field'>" + tr("Reminder volume", "提醒音量") + ": <output id='emotionVolumeOut'>" + String(emotionReminderVolume) + "%</output><input id='emotionVolume' type='range' min='5' max='100' step='5' name='emotionVolume' value='" + String(emotionReminderVolume) + "' oninput='emotionVolumeOut.value=this.value+\"%\"'></label>";
    page += "<button class='btn secondary' type='button' onclick='fetch(\"/preview?sound=\"+document.getElementById(\"emotionSoundChoice\").value+\"&volume=\"+document.getElementById(\"emotionVolume\").value)'>" + tr("Preview reminder sound on Core2", "在 Core2 預聽提醒鈴聲") + "</button>";
  } else if (pageId == "mqtt") {
    page += "<h2>MQTT</h2><p class='muted'>" + tr("Publish current time, meditation state and sensor values; receive commands and settings updates. Home Assistant auto-discovery is supported.", "發佈目前時間、靜心狀態與感測值；接收控制指令與設定更新，並支援 Home Assistant 自動探索。") + "</p><a class='btn secondary' href='/mqtt-guide'>" + tr("Open complete MQTT guide", "開啟完整 MQTT 設定指南") + "</a>";
    page += "<label class='check'><input type='checkbox' name='mqttEnabled'" + String(mqttEnabled ? " checked" : "") + ">" + tr("Enable MQTT", "啟用 MQTT") + "</label><label class='field'>" + tr("Broker host/IP", "Broker 主機／IP") + "<input name='mqttHost' value='" + htmlEscape(mqttHost) + "'></label>";
    page += "<div class='grid'><label class='field'>" + tr("Port", "連接埠") + "<input type='number' min='1' max='65535' name='mqttPort' value='" + String(mqttPort) + "'></label><label class='field'>" + tr("Base topic", "基礎 Topic") + "<input name='mqttBaseTopic' value='" + htmlEscape(mqttBaseTopic) + "'></label></div>";
    page += "<label class='field'>" + tr("Username", "使用者名稱") + "<input name='mqttUsername' value='" + htmlEscape(mqttUsername) + "'></label><label class='field'>" + tr("Password", "密碼") + "<input type='password' name='mqttPassword' placeholder='" + tr("Leave blank to keep current", "留白以保留目前密碼") + "'></label>";
  } else if (pageId == "hass") {
    page += "<h2>Home Assistant Assist</h2><p class='muted'>" + tr("Use this Core2's microphone and speaker as a push-to-talk Home Assistant voice terminal. On the clock, long-press the middle Meditation icon, then hold the microphone while speaking and release it to send.", "使用 Core2 的麥克風與喇叭作為 Home Assistant 按住說話語音終端。在時鐘首頁長按中間的靜心圖示進入；按住麥克風說話，放開後送出。") + "</p>";
    page += "<div class='card'><b>Sherpa ONNX TTS/STT · Wyoming</b><p class='muted'>" + tr("Supported through Home Assistant's Assist Pipeline. In Home Assistant, finish adding the automatically discovered Wyoming service, then create or edit a Voice Assistant pipeline that uses Sherpa for both speech-to-text and text-to-speech. The Core2 must still use the Home Assistant URL below; do not enter ports 10400 or 10500 here.", "已透過 Home Assistant Assist Pipeline 支援。請先在 Home Assistant 完成加入自動探索到的 Wyoming 服務，再建立或編輯語音助理 Pipeline，將語音轉文字與文字轉語音都選為 Sherpa。Core2 下方仍應填 Home Assistant 網址；請勿在這裡填入 10400 或 10500 連接埠。") + "</p><p id='hassLiveStatus' class='muted'>" + tr("Checking Home Assistant connection...", "正在檢查 Home Assistant 連線……") + "</p><button class='btn secondary' type='button' onclick='refreshHassStatus(true)'>" + tr("Refresh connection and pipelines", "重新偵測連線與 Pipeline") + "</button></div>";
    page += "<label class='check'><input type='checkbox' name='hassEnabled'" + String(hassAssistEnabled ? " checked" : "") + ">" + tr("Enable HASS Assist", "啟用 HASS Assist") + "</label>";
    page += "<label class='field'>" + tr("Home Assistant base URL", "Home Assistant 基礎網址") + "<input name='hassBaseUrl' inputmode='url' placeholder='http://homeassistant.local:8123' value='" + htmlEscape(hassAssistBaseUrl) + "'></label>";
    page += "<label class='field'>" + tr("Long-lived access token", "長期存取權杖") + "<input type='password' name='hassToken' autocomplete='new-password' placeholder='" + tr(hassAssistToken.length() ? "Saved — leave blank to keep current" : "Paste a Home Assistant long-lived token", hassAssistToken.length() ? "已儲存—留白即可保留目前權杖" : "貼上 Home Assistant 長期存取權杖") + "'></label>";
    page += "<label class='check'><input type='checkbox' name='hassClearToken'>" + tr("Forget the saved token", "清除已儲存的權杖") + "</label>";
    page += "<fieldset class='field'><legend>" + tr("Voice mode", "發話方式") + "</legend>";
    {
      const char* modeEn[] = {"Tap to talk, tap again to send (no wake word)", "Hold to talk, release to send (no wake word)", "Always listening for the wake word"};
      const char* modeZh[] = {"按一下發話，再按一下結束（不需要喚醒詞）", "長按發話，放開結束（不需要喚醒詞）", "常駐收音偵測發話（需要喚醒詞）"};
      for (uint8_t m = 0; m < 3; ++m) {
        page += "<label class='check'><input type='radio' name='hassMode' value='" + String(m) + "'" + String(hassAssistVoiceMode == m ? " checked" : "") + ">" + tr(modeEn[m], modeZh[m]) + "</label>";
      }
    }
    page += "</fieldset><p class='muted'>" + tr("Wake-word mode requires a wake-word engine and model in the selected Home Assistant pipeline. Sherpa ONNX supplies STT/TTS only. The microphone pauses automatically for alarms, meditation audio and firmware updates.", "常駐收音模式需要所選 Home Assistant Pipeline 另有喚醒詞引擎與模型；Sherpa ONNX 本身只提供 STT/TTS。鬧鐘、靜心音訊及韌體更新期間會自動暫停麥克風。") + "</p>";
    page += "<label class='field'>" + tr("Assist pipeline", "Assist Pipeline") + "<select id='hassPipelineSelect' name='hassPipeline'><option value=''" + String(hassAssistPipeline.length() ? "" : " selected") + ">" + tr("Home Assistant preferred pipeline", "使用 Home Assistant 偏好 Pipeline") + "</option>";
    bool savedHassPipelineFound = !hassAssistPipeline.length();
    for (uint8_t i = 0; i < hassAssistPipelineCount; ++i) {
      bool selected = hassAssistPipeline == hassAssistPipelineIds[i];
      if (selected) savedHassPipelineFound = true;
      String optionLabel = hassAssistPipelineNames[i];
      if (hassAssistPipelineIds[i] == hassAssistPreferredPipeline) optionLabel += tr(" (preferred)", "（偏好）");
      page += "<option value='" + htmlEscape(hassAssistPipelineIds[i]) + "'" + String(selected ? " selected" : "") + ">" + htmlEscape(optionLabel) + "</option>";
    }
    if (!savedHassPipelineFound) page += "<option value='" + htmlEscape(hassAssistPipeline) + "' selected>" + htmlEscape(hassAssistPipeline) + "</option>";
    page += "</select></label>";
    page += "<label class='field'>" + tr("Voice reply volume", "語音回覆音量") + ": <output id='hassVolumeOut'>" + String(hassAssistVolume) + "%</output><input type='range' min='5' max='100' step='5' name='hassVolume' value='" + String(hassAssistVolume) + "' oninput='hassVolumeOut.value=this.value+\"%\"'></label>";
    page += "<p class='muted'>" + tr("The token is stored only in this Core2's Preferences and is never published to GitHub or MQTT. Because this settings page is local HTTP, configure it only on a trusted Wi-Fi network. MP3 and WAV voice replies are supported.", "權杖只會保存在這台 Core2 的偏好設定，不會上傳 GitHub 或 MQTT。因本設定頁是區域網路 HTTP，請只在可信任的 Wi-Fi 設定。支援 MP3 與 WAV 語音回覆。") + "</p>";
  } else if (pageId == "calendar") {
    page += "<h2>" + tr("Calendar", "日曆") + "</h2><p class='muted'>" + tr("Paste an iCal (.ics) subscription URL, e.g. Google Calendar's \"Secret address in iCal format\". On the clock, long-press the screen to open the calendar; swipe left for month, right for week; long-press again to return.", "貼上 iCal（.ics）訂閱網址，例如 Google 日曆的「iCal 格式的私人網址」。在時鐘畫面長按進入日曆；往左滑為月模式、往右滑為週模式；再長按回到時鐘。") + "</p>";
    page += "<label class='field'>" + tr("iCal URL", "iCal 網址") + "<input name='icalUrl' inputmode='url' placeholder='https://calendar.google.com/calendar/ical/.../basic.ics' value='" + htmlEscape(calendarIcalUrl) + "'></label>";
    if (calendarIcalUrl.indexOf("calendar.google.com") >= 0 && calendarIcalUrl.indexOf("/public/") >= 0) {
      page += "<p class='warn'>" + tr("This is Google's public address. Unless the calendar is public, it only shows \"busy\". Use \"Secret address in iCal format\" (contains private-...) to see event titles.", "這是 Google 的「公開網址」。日曆若未公開，只會顯示「busy」。請改用「iCal 格式的私人網址」（網址含 private-...）才能看到每筆標題。") + "</p>";
    }
    page += "<p class='muted'>" + tr("Events loaded: ", "已載入行程：") + String(calEventCount) + (calError.length() ? " · " + htmlEscape(calError) : String("")) + "</p>";
    page += "<button class='btn secondary' type='submit' formaction='/calendar-sync' formmethod='post'>" + tr("Sync now", "立刻同步") + "</button>";
  } else if (pageId == "companion") {
    page += "<h2>Companion</h2><p class='muted'>" + tr("Local host is preferred automatically; the Internet URL is used when the local connection is unavailable. You may fill either or both.", "優先連接區域網路主機；連不上時改用網際網路網址。可填其中一種，也可兩者都填。") + "</p>";
    for (int i = 0; i < COMPANION_PAGE_COUNT; ++i) {
      page += "<div class='card'><b>" + tr("Page ", "頁面 ") + String(i + 1) + "</b><label class='field'>" + tr("Host name", "主機名稱") + "<input name='compName" + String(i) + "' maxlength='24' value='" + htmlEscape(companionNames[i]) + "'></label>";
      page += "<label class='field'>" + tr("Local host/IP", "區域網路主機／IP") + "<input name='compHost" + String(i) + "' placeholder='10.43.50.145' value='" + htmlEscape(companionHosts[i]) + "'></label><label class='field'>" + tr("Local TCP port", "區域網路 TCP 連接埠") + "<input type='number' min='1' max='65535' name='compPort" + String(i) + "' value='" + String(companionPorts[i]) + "'></label>";
      page += "<label class='field'>" + tr("Internet WebSocket URL", "網際網路 WebSocket 網址") + "<input name='compRemote" + String(i) + "' placeholder='wss://example.com/satellite' value='" + htmlEscape(companionInternetUrls[i]) + "'></label></div>";
    }
  } else if (pageId == "firmware") {
    page += "<h2>" + tr("Firmware update", "韌體更新") + "</h2><p class='muted'>" + tr("Current version", "目前版本") + ": <b>" SPACE_CLOCK_VERSION "</b></p><a class='btn secondary' href='/update'>" + tr("Open wireless firmware update", "開啟無線韌體更新") + "</a>";
    page += "<h2>" + tr("Automatic update", "自動更新") + "</h2><label class='check'><input type='checkbox' name='fwAuto'" + String(automaticFirmwareUpdate ? " checked" : "") + ">" + tr("Automatically install new firmware from GitHub", "從 GitHub 自動安裝新韌體") + "</label><label class='field'>" + tr("Daily check hour", "每日檢查時間") + "<select name='fwHour'>";
    for (int hour = 0; hour < 24; ++hour) { char label[7]; snprintf(label, sizeof(label), "%02d:00", hour); page += "<option value='" + String(hour) + "'" + (firmwareCheckHour == hour ? " selected" : "") + ">" + label + "</option>"; }
    page += "</select></label><p class='muted'>" + tr("If an update is available, the device downloads it and restarts automatically. Keep it powered and connected to Wi-Fi.", "發現更新時裝置會自動下載並重新啟動。請保持供電並連接 Wi-Fi。") + "</p>";
  }

  page += "</section><button type='submit'>" + tr("Save this page", "儲存本頁設定") + "</button></form>";
  page += "<script>function previewSound(k){const s=document.getElementById(k==='start'?'medStartSound':'medEndSound').value,v=document.getElementById(k==='start'?'medStartVolume':'medEndVolume').value;fetch('/preview?sound='+s+'&volume='+v);}";
  if (pageId == "hass") {
    page += "async function refreshHassStatus(force=false){const status=document.getElementById('hassLiveStatus'),select=document.getElementById('hassPipelineSelect');try{const response=await fetch('/hass-status'+(force?'?refresh=1':''),{cache:'no-store'}),data=await response.json();status.textContent=data.authenticated?'" + tr("Connected · ", "已連線 · ") + "'+data.pipelines.length+' " + tr("pipeline(s) detected", "個 Pipeline") + "':(data.error||'" + tr("Connecting to Home Assistant...", "正在連線至 Home Assistant……") + "');const current=select.value,preferred=data.preferred||'';select.innerHTML='';const automatic=document.createElement('option');automatic.value='';automatic.textContent='" + tr("Home Assistant preferred pipeline", "使用 Home Assistant 偏好 Pipeline") + "';select.appendChild(automatic);for(const pipeline of data.pipelines){const option=document.createElement('option');option.value=pipeline.id;option.textContent=pipeline.name+(pipeline.id===preferred?' " + tr("(preferred)", "（偏好）") + "':'');select.appendChild(option);}if(current&&!Array.from(select.options).some(option=>option.value===current)){const option=document.createElement('option');option.value=current;option.textContent=current;select.appendChild(option);}select.value=current;}catch(error){status.textContent='" + tr("Unable to read Home Assistant status", "無法讀取 Home Assistant 狀態") + "';}}refreshHassStatus();setInterval(refreshHassStatus,5000);";
  }
  page += "</script></body></html>";
  settingsServer.send(200, "text/html; charset=utf-8", page);
}

void sendFirmwareUpdatePage(const String& error = "") {
  String page;
  page.reserve(3500);
  page = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Firmware update</title><style>html{color-scheme:dark}body{font-family:system-ui;background:#08111f;color:#eef4ff;max-width:620px;margin:auto;padding:20px}"
         "h1{color:#65b9ff}.box{background:#101d2e;border:1px solid #344b63;border-radius:10px;padding:16px}input{box-sizing:border-box;width:100%;padding:12px;margin-top:12px;background:#142236;color:#fff;border:1px solid #52657a}"
         "button,a{box-sizing:border-box;display:block;width:100%;padding:13px;margin-top:18px;border:0;border-radius:9px;background:#1688e5;color:white;font-size:17px;text-align:center;text-decoration:none}.error{color:#ff7d7d}</style></head><body>"
         "<h1>Wireless firmware update</h1><div class='box'><p>Keep the Core2 powered and connected to Wi-Fi until it restarts.</p>";
  if (error.length()) page += "<p class='error'>" + htmlEscape(error) + "</p>";
  page += "<form method='post' action='/update' enctype='multipart/form-data' onsubmit='document.getElementById(\"status\").textContent=\"Uploading and installing... Do not turn off power.\"'>"
          "<input type='file' name='firmware' accept='.bin,application/octet-stream' required><button type='submit'>Install firmware</button></form>"
          "<p id='status'></p></div><a href='/'>Back to settings</a></body></html>";
  settingsServer.send(200, "text/html; charset=utf-8", page);
}

void sendHassAssistStatus() {
  if (settingsServer.hasArg("refresh")) {
    if (hassAssistAuthenticated) requestHassAssistPipelines();
    else if (hassAssistEnabled && !hassAssistSocketStarted) connectHassAssist();
  }
  JsonDocument status;
  status["enabled"] = hassAssistEnabled;
  status["connected"] = hassAssistSocketConnected;
  status["authenticated"] = hassAssistAuthenticated;
  status["state"] = hassAssistStateText();
  status["last_event"] = hassAssistLastEvent;
  status["pipeline_active"] = hassAssistPipelineActive;
  status["wake_session"] = hassAssistWakeSessionActive;
  status["wake_detected"] = hassAssistWakeDetected;
  status["mic_running"] = hassAssistMicRunning;
  status["mic_chunks"] = hassAssistMicChunksSent;
  status["mic_bytes"] = hassAssistMicBytesSent;
  status["mic_peak"] = hassAssistMicPeak;
  status["audio_handler"] = hassAssistAudioHandlerId;
  status["preferred"] = hassAssistPreferredPipeline;
  String statusError = hassAssistDiscoveryError.length() ? hassAssistDiscoveryError : hassAssistError;
  if (!hassAssistEnabled) statusError = "HASS Assist is disabled";
  status["error"] = statusError;
  JsonArray pipelines = status["pipelines"].to<JsonArray>();
  for (uint8_t i = 0; i < hassAssistPipelineCount; ++i) {
    JsonObject pipeline = pipelines.add<JsonObject>();
    pipeline["id"] = hassAssistPipelineIds[i];
    pipeline["name"] = hassAssistPipelineNames[i];
  }
  String payload;
  serializeJson(status, payload);
  settingsServer.sendHeader("Cache-Control", "no-store");
  settingsServer.send(200, "application/json; charset=utf-8", payload);
}

void setupSettingsServer() {
  settingsServer.on("/", HTTP_GET, []() { sendSettingsPage(); });
  settingsServer.on("/hass-status", HTTP_GET, []() { sendHassAssistStatus(); });
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
  settingsServer.on("/emotion-login", HTTP_POST, []() {
    String language = settingsServer.arg("lang");
    String identity = settingsServer.arg("emotionIdentity"); identity.trim();
    String password = settingsServer.arg("emotionPassword");
    String message;
    emotionApiLogin(identity, password, message);
    password = "";
    sendSettingsPage(message, "emotion", language);
  });
  settingsServer.on("/emotion-check", HTTP_POST, []() {
    String language = settingsServer.arg("lang");
    bool connected = verifyEmotionApiConnection(true);
    String message = connected
      ? (language == "zh" ? "資料庫連線正常，可以開始填寫。" : "The database connection is ready; entries can be started.")
      : (language == "zh" ? "資料庫連線失敗，請重新登入。" : "The database connection failed; please sign in again.");
    sendSettingsPage(message, "emotion", language);
  });
  settingsServer.on("/emotion-logout", HTTP_POST, []() {
    String language = settingsServer.arg("lang");
    emotionApiToken = ""; emotionApiUserId = ""; emotionApiIdentity = "";
    emotionApiState = EmotionApiState::Disconnected; emotionApiLastChecked = millis();
    prefs.begin("spaceclock", false); prefs.remove("emoToken"); prefs.remove("emoUser"); prefs.remove("emoIdent"); prefs.end();
    sendSettingsPage(language == "zh" ? "已清除此設備上的 API 登入資訊。" : "The API login was forgotten on this device.", "emotion", language);
  });
  settingsServer.on("/update", HTTP_GET, []() { sendFirmwareUpdatePage(); });
  settingsServer.on("/update", HTTP_POST,
    []() {
      bool success = webFirmwareUploadOk && !Update.hasError();
      settingsServer.sendHeader("Connection", "close");
      firmwareUpdateMessage = success ? "Installed. Restarting..."
        : "Web update failed (error " + String(Update.getError()) + ").";
      showFirmwareUpdate();
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
        webFirmwareUploadOk = false;
        webFirmwareLastPercent = -1;
        // The upload runs inside one handleClient() call, so loop() cannot
        // stop Assist for us. Release the mic/socket before flashing.
        stopHassAssist();
        firmwareUpdateMessage = "Receiving firmware from web page...";
        showFirmwareUpdate();
        drawFirmwareDownloadProgress(0, 0, 0, "Uploading");
        Serial.printf("[ota] web upload start: %s, request length %d\n",
                      upload.filename.c_str(), settingsServer.clientContentLength());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        lastUserActivity = millis();
        if (!Update.hasError() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
        int total = settingsServer.clientContentLength();
        int percent = total > 0 ? (int)min<uint64_t>(99, (uint64_t)upload.totalSize * 100ULL / (uint64_t)total) : 0;
        if (percent >= webFirmwareLastPercent + 2) {
          webFirmwareLastPercent = percent;
          drawFirmwareDownloadProgress((uint8_t)percent, 0, 0, "Uploading");
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("[ota] web upload received %u bytes\n", (unsigned)upload.totalSize);
        drawFirmwareVerificationStatus(0, 0);
        webFirmwareUploadOk = !Update.hasError() && finalizeFirmwareUpdateSafely(false);
        if (!webFirmwareUploadOk) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        webFirmwareUploadOk = false;
        firmwareUpdateMessage = "Web upload was interrupted.";
        showFirmwareUpdate();
      }
    });
  settingsServer.on("/save", HTTP_POST, []() {
    String pageId = settingsServer.arg("page");
    String language = settingsServer.arg("lang");
    if (pageId != "wifi" && pageId != "clock" && pageId != "alarms" && pageId != "meditation" && pageId != "emotion" && pageId != "mqtt" && pageId != "hass" && pageId != "companion" && pageId != "calendar" && pageId != "firmware") pageId = "clock";
    bool wifiChanged = false;
    bool deviceNameChanged = false;
    bool emotionBaseRejected = false;
    bool hassBaseRejected = false;
    bool reconnectHassAssist = false;
    bool reconnectCompanion = false;
    if (pageId == "wifi") {
      String nextDeviceName = settingsServer.arg("deviceName"); nextDeviceName.trim();
      if (!nextDeviceName.length()) nextDeviceName = "Space Clock";
      deviceNameChanged = nextDeviceName != deviceName;
      deviceName = nextDeviceName;
      for (int i = 0; i < SAVED_WIFI_COUNT; ++i) {
        String nextSsid = settingsServer.arg("wifiS" + String(i)); nextSsid.trim();
        String nextPassword = settingsServer.arg("wifiP" + String(i));
        if (settingsServer.hasArg("wifiD" + String(i))) { nextSsid = ""; nextPassword = ""; }
        else if (!nextPassword.length() && nextSsid == savedWifiSsids[i]) nextPassword = savedWifiPasswords[i];
        wifiChanged |= nextSsid != savedWifiSsids[i] || nextPassword != savedWifiPasswords[i];
        savedWifiSsids[i] = nextSsid; savedWifiPasswords[i] = nextPassword;
      }
    } else if (pageId == "clock") {
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
      screenOffSeconds = constrain(settingsServer.arg("screenOff").toInt(), 0, 1800);
      nightLightEnabled = settingsServer.hasArg("nightLight");
      nightLightColor = parseWebColor(settingsServer.arg("nightColor"), nightLightColor);
      nightLightBrightness = constrain(settingsServer.arg("nightLedBrightness").toInt(), 1, 100);
      nightLightMode = constrain(settingsServer.arg("nightLightMode").toInt(), 0, 2);
      nightLightSeconds = constrain(settingsServer.arg("nightLightSeconds").toInt(), 5, 3600);
    } else if (pageId == "alarms") {
      alarmVolume = constrain(settingsServer.arg("alarmVolume").toInt(), 10, 100);
      alarmSound = constrain(settingsServer.arg("alarmSound").toInt(), 0, 3);
      alarmLightEnabled = settingsServer.hasArg("alarmLight");
      alarmLightColor = parseWebColor(settingsServer.arg("alarmLightColor"), alarmLightColor);
      alarmLightBrightness = constrain(settingsServer.arg("alarmLightBrightness").toInt(), 1, 100);
      alarmLightMode = constrain(settingsServer.arg("alarmLightMode").toInt(), 0, 3);
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
        if (oldHour != alarms[i].hour || oldMinute != alarms[i].minute || oldWeekdays != alarms[i].weekdays || (!oldEnabled && alarms[i].enabled)) alarms[i].lastDay = -1;
      }
    } else if (pageId == "meditation") {
      meditationPresetMinutes[0] = constrain(settingsServer.arg("medPreset1").toInt(), 1, 60);
      meditationPresetMinutes[1] = constrain(settingsServer.arg("medPreset2").toInt(), 1, 60);
      meditationSoundEnabled = settingsServer.hasArg("medSoundEnabled");
      meditationStartSound = constrain(settingsServer.arg("medStartSound").toInt(), 0, 3);
      meditationStartVolume = constrain(settingsServer.arg("medStartVolume").toInt(), 5, 100);
      meditationEndSound = constrain(settingsServer.arg("medEndSound").toInt(), 0, 3);
      meditationEndVolume = constrain(settingsServer.arg("medEndVolume").toInt(), 5, 100);
      meditationLightEnabled = settingsServer.hasArg("medLightEnabled");
      meditationNoiseEnabled = settingsServer.hasArg("medNoiseEnabled");
      meditationNoise = constrain(settingsServer.arg("medNoise").toInt(), 0, 2);
      meditationNoiseVolume = constrain(settingsServer.arg("medNoiseVolume").toInt(), 5, 80);
    } else if (pageId == "emotion") {
      String requestedBase = normalizeEmotionApiBase(settingsServer.arg("emotionApiBase"));
      if (requestedBase.length()) {
        if (requestedBase != emotionApiBase) {
          emotionApiToken = ""; emotionApiUserId = ""; emotionApiIdentity = "";
          emotionApiState = EmotionApiState::Disconnected; emotionApiLastChecked = millis();
        }
        emotionApiBase = requestedBase;
      } else if (settingsServer.arg("emotionApiBase") != emotionApiBase) emotionBaseRejected = true;
      emotionReminderMode = constrain(settingsServer.arg("emotionReminderMode").toInt(), 0, 2);
      int requestedEmotionInterval = settingsServer.arg("emotionInterval").toInt();
      const uint16_t validIntervals[] = {10, 15, 30, 60, 120, 180, 240};
      bool intervalAccepted = false;
      for (uint16_t interval : validIntervals) if (requestedEmotionInterval == interval) intervalAccepted = true;
      emotionReminderIntervalMinutes = intervalAccepted ? requestedEmotionInterval : 60;
      String windowStart = settingsServer.arg("emotionWindowStart");
      String windowEnd = settingsServer.arg("emotionWindowEnd");
      if (windowStart.length() >= 5) emotionReminderWindowStart = constrain(windowStart.substring(0, 2).toInt(), 0, 23) * 60 + constrain(windowStart.substring(3, 5).toInt(), 0, 59);
      if (windowEnd.length() >= 5) emotionReminderWindowEnd = constrain(windowEnd.substring(0, 2).toInt(), 0, 23) * 60 + constrain(windowEnd.substring(3, 5).toInt(), 0, 59);
      if (emotionReminderWindowEnd < emotionReminderWindowStart) emotionReminderWindowEnd = emotionReminderWindowStart;
      for (int i = 0; i < 3; ++i) {
        if (!settingsServer.hasArg("emotionTimeOn" + String(i))) emotionReminderTimes[i] = 0xFFFF;
        else {
          String timeValue = settingsServer.arg("emotionTime" + String(i));
          if (timeValue.length() >= 5) emotionReminderTimes[i] = constrain(timeValue.substring(0, 2).toInt(), 0, 23) * 60 + constrain(timeValue.substring(3, 5).toInt(), 0, 59);
        }
      }
      emotionReminderVibration = settingsServer.hasArg("emotionVibration");
      emotionReminderSound = settingsServer.hasArg("emotionSoundEnabled");
      int duration = settingsServer.arg("emotionDuration").toInt();
      emotionReminderDurationSeconds = (duration == 10 || duration == 30 || duration == 60 || duration == 120) ? duration : 30;
      emotionReminderSoundChoice = constrain(settingsServer.arg("emotionSoundChoice").toInt(), 0, 3);
      emotionReminderVolume = constrain(settingsServer.arg("emotionVolume").toInt(), 5, 100);
      emotionLastReminderMinuteKey = -1;
    } else if (pageId == "mqtt") {
      mqttEnabled = settingsServer.hasArg("mqttEnabled");
      mqttHost = settingsServer.arg("mqttHost");
      mqttPort = constrain(settingsServer.arg("mqttPort").toInt(), 1, 65535);
      mqttUsername = settingsServer.arg("mqttUsername");
      if (settingsServer.arg("mqttPassword").length()) mqttPassword = settingsServer.arg("mqttPassword");
      mqttBaseTopic = settingsServer.arg("mqttBaseTopic"); mqttBaseTopic.trim();
      while (mqttBaseTopic.endsWith("/")) mqttBaseTopic.remove(mqttBaseTopic.length() - 1);
      mqttClient.disconnect();
    } else if (pageId == "hass") {
      bool nextEnabled = settingsServer.hasArg("hassEnabled");
      String nextBase = settingsServer.arg("hassBaseUrl"); nextBase.trim();
      while (nextBase.endsWith("/")) nextBase.remove(nextBase.length() - 1);
      if (nextBase.length() && !nextBase.startsWith("http://") && !nextBase.startsWith("https://")) {
        hassBaseRejected = true;
        nextBase = hassAssistBaseUrl;
      }
      String nextPipeline = settingsServer.arg("hassPipeline"); nextPipeline.trim();
      uint8_t nextVoiceMode = settingsServer.hasArg("hassMode")
        ? (uint8_t)constrain(settingsServer.arg("hassMode").toInt(), 0, 2) : hassAssistVoiceMode;
      bool nextWakeWordEnabled = nextVoiceMode == HASS_MODE_WAKE;
      hassAssistVoiceMode = nextVoiceMode;
      reconnectHassAssist = nextEnabled != hassAssistEnabled || nextBase != hassAssistBaseUrl
        || nextPipeline != hassAssistPipeline || nextWakeWordEnabled != hassAssistWakeWordEnabled;
      if (reconnectHassAssist) hassAssistWakeWordPaused = false;
      hassAssistEnabled = nextEnabled;
      hassAssistBaseUrl = nextBase;
      hassAssistPipeline = nextPipeline;
      hassAssistWakeWordEnabled = nextWakeWordEnabled;
      hassAssistVolume = constrain(settingsServer.arg("hassVolume").toInt(), 5, 100);
      if (settingsServer.hasArg("hassClearToken")) {
        reconnectHassAssist |= hassAssistToken.length();
        hassAssistToken = "";
      } else if (settingsServer.arg("hassToken").length()) {
        reconnectHassAssist = true;
        hassAssistToken = settingsServer.arg("hassToken");
        hassAssistToken.trim();
      }
    } else if (pageId == "calendar") {
      String nextUrl = settingsServer.arg("icalUrl"); nextUrl.trim();
      if (nextUrl != calendarIcalUrl) { calendarIcalUrl = nextUrl; calFetchedAt = 0; calEventCount = 0; }
      // Load right away so the page can report how many events were found.
      if (calendarIcalUrl.length()) fetchCalendar(calLocalMidnight(time(nullptr)));
    } else if (pageId == "companion") {
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
    } else if (pageId == "firmware") {
      automaticFirmwareUpdate = settingsServer.hasArg("fwAuto");
      firmwareCheckHour = constrain(settingsServer.arg("fwHour").toInt(), 0, 23);
    }
    if (reconnectCompanion) {
      stopCompanion();
      clearCompanionPageData();
    }
    if (reconnectHassAssist) stopHassAssist();
    saveSettings();
    String savedMessage = language == "zh" ? "本頁設定已儲存並套用。" : "This page was saved and applied.";
    if (emotionBaseRejected) savedMessage = language == "zh" ? "API 網址無效；必須以 https:// 開頭，原網址已保留。其餘設定已儲存。" : "Invalid API URL; HTTPS is required. The previous URL was kept, and other settings were saved.";
    if (hassBaseRejected) savedMessage = language == "zh" ? "Home Assistant 網址無效；必須以 http:// 或 https:// 開頭，原網址已保留。其餘設定已儲存。" : "Invalid Home Assistant URL; it must begin with http:// or https://. The previous URL was kept, and other settings were saved.";
    if (deviceNameChanged) savedMessage = language == "zh" ? "設備名稱已儲存，設備即將重新連線以套用新的網路名稱。" : "Device name saved. The device is restarting Wi-Fi to apply its new network name.";
    sendSettingsPage(savedMessage, pageId, language);
    if (deviceNameChanged) {
      delay(800);
      ESP.restart();
      return;
    }
    if (wifiChanged) {
      wifiWasConnected = false;
      wifiDefaultFallbackAttempted = false;
      if (hasSavedWifiProfiles()) startWifiProfileScan(millis());
      else beginLegacyWifiRetry(millis());
    } else {
      if (pageId == "clock") syncTime();
      if (pageId == "clock" || pageId == "alarms") {
        m5::rtc_datetime_t brightnessNow; getClockDateTime(&brightnessNow); applyDisplayBrightness(brightnessNow);
        updateAlarmBaseLights(millis()); drawClock(true); drawAstronaut();
      }
    }
  });
  settingsServer.on("/calendar-sync", HTTP_POST, []() {
    String language = settingsServer.arg("lang");
    time_t around = calViewDay ? calViewDay : calLocalMidnight(time(nullptr));
    bool ok = fetchCalendar(around);
    if (screenNow == Screen::Calendar) drawCalendar();
    sendSettingsPage(ok ? (language == "zh" ? "已同步，載入 " + String(calEventCount) + " 筆行程。" : "Synced: " + String(calEventCount) + " events.")
                        : (language == "zh" ? "同步失敗：" : "Sync failed: ") + calError, "calendar", language);
  });
  settingsServer.on("/debug", HTTP_GET, []() {
    String out = "night_led=" + String(nightLedShowing) + " manual_override=" + String(manualNightLightOverride)
      + " manual_active=" + String(manualNightLightActive) + " sleeping=" + String(screenSleeping)
      + " screen=" + String((int)screenNow) + "\n" + touchDebugLog;
    settingsServer.send(200, "text/plain; charset=utf-8", out);
  });
  settingsServer.onNotFound([]() { settingsServer.sendHeader("Location", "/"); settingsServer.send(302); });
  settingsServer.begin();
  settingsServerReady = true;
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true; cfg.internal_mic = true; cfg.internal_rtc = true; cfg.internal_imu = true;
  M5.begin(cfg);
  Serial.begin(115200);
  // The SPIFFS partition is never formatted on a new Core2 (or after a USB
  // flash that erased it), so mounting without formatting always failed and
  // silently disabled the offline emotion queue. Format once when needed.
  if (SPIFFS.begin(true)) {
    emotionStorageReady = true;
    emotionPendingCount = countEmotionQueue();
    Serial.printf("[emotion] local storage ready; queued records=%u\n", emotionPendingCount);
  } else {
    Serial.println("[emotion] local storage unavailable; offline queue disabled");
  }
  setCpuFrequencyMhz(160);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Display.setColorDepth(16);
  M5.Display.setBrightness(100);
  M5.setTouchButtonHeight(0);
  bottomLeds.begin();
  bottomLeds.clear();
  bottomLeds.show();
  Serial.printf("[display] PSRAM: %u bytes, free: %u bytes\n", ESP.getPsramSize(), ESP.getFreePsram());
  astronautCanvas.setColorDepth(16);
  astronautCanvas.createSprite(105, 130);
  companionButtonCanvas.setColorDepth(16);
  companionButtonCanvas.createSprite(96, 96);
  meditationCardCanvas.setColorDepth(16);
  meditationCardCanvas.createSprite(98, 96);
  firmwareProgressCanvas.setColorDepth(16);
  firmwareProgressCanvasReady = firmwareProgressCanvas.createSprite(292, 44) != nullptr;
  spaceTimeCanvas.setPsram(true); spaceTimeCanvas.setColorDepth(16);
  spaceTimeCanvasReady = spaceTimeCanvas.createSprite(204, 92) != nullptr;
  clockSecondsCanvas.setColorDepth(16);
  clockSecondsCanvasReady = clockSecondsCanvas.createSprite(64, 32) != nullptr;
  matrixCanvas.setPsram(true);
  matrixCanvas.setColorDepth(16);
  matrixCanvasReady = matrixCanvas.createSprite(320, 240) != nullptr;
  if (!matrixCanvasReady) {
    // A lower-memory fallback still composites a complete frame (no flicker).
    matrixCanvas.setColorDepth(8);
    matrixCanvasReady = matrixCanvas.createSprite(320, 240) != nullptr;
  }
  Serial.printf("[display] Matrix canvas: %s (%d bpp)\n", matrixCanvasReady ? "ready" : "FAILED", matrixCanvas.getColorDepth());
  loadSettings();
  sanitizeRtcOnBoot();
  lastUserActivity = millis();
  m5::rtc_datetime_t startupTime; getClockDateTime(&startupTime); applyDisplayBrightness(startupTime);
  WiFi.mode(WIFI_STA); applyNetworkHostname(); WiFi.setAutoReconnect(true); WiFi.setSleep(true); WiFi.begin();
  wifiRecoveryPhase = WifiRecoveryPhase::Primary;
  wifiRecoveryPhaseStartedAt = millis();
  drawClock(true); drawAstronaut();
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
  maintainHassAssist(nowMs);
  if (alarmActive >= 0 && screenNow == Screen::Clock && clockFace == ClockFace::Matrix
      && nowMs - lastAlarmChallengeDraw >= 65UL) {
    lastAlarmChallengeDraw = nowMs;
    drawMatrixAlarmChallenge(nowMs);
    if (alarmPillHolding && nowMs - alarmPillHoldStarted >= 2400UL) {
      haptic(45);
      dismissAlarm();
    }
  }
  maintainSavedWifi(nowMs);
  // Upload the offline emotion queue in the background on every screen.
  // Skip only while audio must not stutter (alarm ringing, Assist recording
  // or speaking), and back off after failures so a dead network does not
  // keep freezing the UI with 8 s HTTPS timeouts.
  static uint8_t emotionSyncFailures = 0;
  uint32_t emotionSyncInterval = emotionSyncFailures ? min(300000UL, 15000UL << min<uint8_t>(emotionSyncFailures - 1, 5)) : 5000UL;
  if (WiFi.status() == WL_CONNECTED && emotionPendingCount && alarmActive < 0
      && !hassAssistMicRunning && !hassAssistAudioData && !hassAssistMp3Decoder
      && nowMs - emotionLastQueueSyncAt >= emotionSyncInterval) {
    if (flushOneEmotionRecord()) {
      emotionSyncFailures = 0;
      Serial.printf("[emotion] queued record synced; remaining=%u\n", emotionPendingCount);
    } else {
      emotionLastQueueSyncAt = millis();
      if (emotionSyncFailures < 250) ++emotionSyncFailures;
      Serial.printf("[emotion] background sync failed (%u); retry in %lu s\n", emotionSyncFailures,
                    (unsigned long)(min(300000UL, 15000UL << min<uint8_t>(emotionSyncFailures - 1, 5)) / 1000));
    }
  }
  if (screenNow == Screen::EmotionObservation || screenNow == Screen::EmotionRecords || screenNow == Screen::EmotionSettings) {
    bool redrawConnection = false;
    if (WiFi.status() != WL_CONNECTED && emotionApiState != EmotionApiState::Disconnected) {
      emotionApiState = EmotionApiState::Disconnected;
      emotionApiLastChecked = nowMs;
      redrawConnection = true;
    } else if (screenNow != Screen::EmotionObservation && WiFi.status() == WL_CONNECTED
               && emotionApiToken.length() && emotionApiUserId.length()) {
      bool retryDisconnected = emotionApiState == EmotionApiState::Disconnected && nowMs - emotionApiLastChecked >= 15000UL;
      bool periodicCheck = emotionApiState == EmotionApiState::Connected && nowMs - emotionApiLastChecked >= 300000UL;
      if (emotionApiState == EmotionApiState::Unknown || retryDisconnected || periodicCheck) {
        verifyEmotionApiConnection(true);
        redrawConnection = true;
      }
    }
    if (redrawConnection) {
      if (screenNow == Screen::EmotionObservation) drawEmotionObservation();
      else if (screenNow == Screen::EmotionRecords) drawEmotionRecords();
      else showEmotionSettings();
    }
  }
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
  if (!screenSleeping && alarmActive < 0 && screenNow != Screen::NightLight && screenOffSeconds > 0 && nowMs - lastUserActivity >= (uint32_t)screenOffSeconds * 1000UL) {
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
        checkEmotionReminder(nowMs, alarmNow);
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
    if (alarmActive < 0) drawClock(false);  // every face now shows seconds
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
