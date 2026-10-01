// Bluetooth headphones for listening mode (A2DP source). Only used while
// listening mode is open: the stack is started on entry (if a headphone is
// remembered) or from the pairing page, and shut down when listening ends.
#pragma once
#include <vector>
#include <Preferences.h>
#include <freertos/stream_buffer.h>
#include "BluetoothA2DPSource.h"

namespace lbt {

struct Device { String name; uint8_t addr[6]; int rssi; };

BluetoothA2DPSource a2dp;
std::vector<Device> found;          // discovered while scanning
Device saved;                       // remembered headphone
bool hasSaved = false;
volatile bool running = false;      // stack started (or starting)
volatile bool starting = false;
volatile bool linkUp = false;       // A2DP connected
volatile bool connecting = false;   // a connection attempt is in flight (never tear the stack down meanwhile)
volatile uint32_t version = 0;      // bumps when the UI should redraw
String status;                      // short text for the pairing page
String connectName;                 // name we are trying to reach
uint8_t connectAddr[6] = {0};
bool connectByAddr = false;
bool scanOnly = false;

StreamBufferHandle_t sb = nullptr;
StaticStreamBuffer_t sbStruct;
uint8_t* sbStorage = nullptr;
static const size_t SB_SIZE = 24 * 1024;   // ~140 ms of 44.1 kHz stereo
uint8_t volume127 = 76;
volatile int8_t playRequest = 0;    // from the headphone buttons / in-ear sensor: 1 play, -1 pause
volatile int8_t navRequest = 0;     // +1 next track, -1 previous track

static String addrText(const uint8_t* a) {
  char b[20]; snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return String(b);
}

void loadSaved() {
  Preferences p;
  if (!p.begin("listenbt", true)) return;
  String name = p.getString("name", "");
  uint8_t addr[6] = {0};
  size_t n = p.getBytes("addr", addr, 6);
  p.end();
  hasSaved = name.length() && n == 6;
  if (hasSaved) { saved.name = name; memcpy(saved.addr, addr, 6); saved.rssi = 0; }
}

void saveDevice(const Device& d) {
  Preferences p;
  if (!p.begin("listenbt", false)) return;
  p.putString("name", d.name);
  p.putBytes("addr", d.addr, 6);
  p.end();
  saved = d; hasSaved = true;
}

void forgetSaved() {
  Preferences p;
  if (p.begin("listenbt", false)) { p.clear(); p.end(); }
  hasSaved = false;
}

static bool onDiscovered(const char* name, esp_bd_addr_t addr, int rssi) {
  String n = name ? name : "";
  bool known = false;
  for (auto& d : found) if (!memcmp(d.addr, addr, 6)) { d.rssi = rssi; known = true; break; }
  if (!known && found.size() < 16) {
    Device d; d.name = n; memcpy(d.addr, addr, 6); d.rssi = rssi;
    found.push_back(d);
    Serial.printf("[bt] found \"%s\" %s rssi %d\n", n.c_str(), addrText(addr).c_str(), rssi);
    ++version;
  }
  if (scanOnly) return false;
  if (connectByAddr && !memcmp(connectAddr, addr, 6)) return true;
  if (!connectByAddr && connectName.length() && n == connectName) return true;
  return false;
}

static void onConnection(esp_a2d_connection_state_t state, void*) {
  linkUp = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
  connecting = state == ESP_A2D_CONNECTION_STATE_CONNECTING || state == ESP_A2D_CONNECTION_STATE_DISCONNECTING;
  status = linkUp ? "已連線" : (state == ESP_A2D_CONNECTION_STATE_CONNECTING ? "連線中…" : "未連線");
  Serial.printf("[bt] connection state %d\n", (int)state);
  ++version;
}

// Buttons and the in-ear sensor of the headphones arrive as AVRCP key presses.
static void onKey(uint8_t key, bool released) {
  Serial.printf("[bt] key 0x%02X %s\n", key, released ? "up" : "down");
  if (released) return;
  switch (key) {
    case ESP_AVRC_PT_CMD_PLAY: playRequest = 1; break;
    case ESP_AVRC_PT_CMD_PAUSE: case ESP_AVRC_PT_CMD_STOP: playRequest = -1; break;
    case ESP_AVRC_PT_CMD_FORWARD: navRequest = 1; break;
    case ESP_AVRC_PT_CMD_BACKWARD: navRequest = -1; break;
    default: break;
  }
  ++version;
}

static int32_t dataCb(Frame* frames, int32_t count) {
  if (!frames || count <= 0) return 0;   // the stack calls with no buffer when the stream stops
  size_t got = sb ? xStreamBufferReceive(sb, frames, (size_t)count * 4, 0) : 0;
  size_t gotFrames = got / 4;
  if ((int32_t)gotFrames < count) memset(frames + gotFrames, 0, (count - gotFrames) * 4);
  return count;
}

static void startTask(void*) {
  Serial.printf("[bt] starting (free internal %u, largest %u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  if (!sb) {
    sbStorage = (uint8_t*)heap_caps_malloc(SB_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (sbStorage) sb = xStreamBufferCreateStatic(SB_SIZE, 1, sbStorage, &sbStruct);
  }
  a2dp.set_ssp_enabled(true);               // AirPods need Secure Simple Pairing
  a2dp.set_local_name("SpaceClock");
  a2dp.set_ssid_callback(onDiscovered);
  a2dp.set_on_connection_state_changed(onConnection);
  a2dp.set_data_callback_in_frames(dataCb);
  a2dp.set_avrc_passthru_command_callback(onKey);
  if (connectByAddr) a2dp.set_auto_reconnect(connectAddr, 3);   // page the device directly
  else a2dp.set_auto_reconnect(false);
  a2dp.start();
  a2dp.set_volume(volume127);
  starting = false;
  status = scanOnly ? "掃描中…" : "連線中…";
  Serial.printf("[bt] started (free internal %u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ++version;
  vTaskDelete(nullptr);
}

static uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

// Start the stack. addr == nullptr: scan only (pairing page); otherwise connect to that device.
bool begin(const uint8_t* addr, const String& name) {
  if (running) return true;
  if (freeInternal() < 70000) { status = "記憶體不足"; Serial.printf("[bt] refusing to start: only %u bytes free\n", (unsigned)freeInternal()); ++version; return false; }
  found.clear();
  connectByAddr = addr != nullptr;
  if (addr) memcpy(connectAddr, addr, 6);
  connectName = name;
  scanOnly = addr == nullptr;
  running = true; starting = true; linkUp = false;
  status = "啟動中…";
  ++version;
  xTaskCreatePinnedToCore(startTask, "btstart", 8192, nullptr, 3, nullptr, 0);
  return true;
}

void end() {
  if (!running) return;
  for (int i = 0; i < 100 && starting; ++i) delay(50);
  // Events of a half-finished connection would hit a stack that is gone.
  for (int i = 0; i < 100 && connecting; ++i) delay(50);
  delay(300);
  a2dp.end(false);
  // The library keeps the host stack and controller up unless it is told to
  // release memory (which would forbid a restart). Take them down ourselves,
  // without releasing, so the RAM returns and the stack can start again.
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_disable();
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) esp_bluedroid_deinit();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
  for (int i = 0; i < 40 && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED; ++i) delay(50);
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) esp_bt_controller_deinit();
  running = false; linkUp = false; connecting = false;
  delay(300);                                    // let the stack settle before a restart
  if (sb) xStreamBufferReset(sb);
  status = "";
  Serial.printf("[bt] stopped (free internal %u)\n", (unsigned)freeInternal());
  ++version;
}

inline bool connected() { return running && !starting && linkUp; }
// A remembered headphone is being (re)connected: playback waits for it a few seconds.
inline bool linking() { return hasSaved && running && !scanOnly && !linkUp; }

void setVolumePercent(int percent) {
  volume127 = (uint8_t)constrain(percent * 127 / 100, 0, 127);
  if (running && !starting) a2dp.set_volume(volume127);
}

void flushAudio() { if (sb) xStreamBufferReset(sb); }

// Queue interleaved stereo frames; waits up to `waitMs` for space. Returns frames queued.
size_t write(const int16_t* stereo, size_t frames, uint32_t waitMs) {
  if (!sb) return 0;
  size_t sent = xStreamBufferSend(sb, stereo, frames * 4, pdMS_TO_TICKS(waitMs));
  return sent / 4;
}

}  // namespace lbt
