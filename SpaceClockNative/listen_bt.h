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

// The library sleeps 10 s inside its stack-up handler before it connects (and
// polls every 10 s afterwards). This copy of that handler starts connecting at
// once and polls every 2 s.
class LinkSource : public BluetoothA2DPSource {
 protected:
  void av_hdl_stack_evt(uint16_t event, void* p_param) override {
    if (event != 0 /* BT_APP_EVT_STACK_UP */) { BluetoothA2DPSource::av_hdl_stack_evt(event, p_param); return; }
    esp_bt_gap_set_device_name(dev_name);
    esp_bt_gap_register_callback(ccall_app_gap_callback);
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(ccall_app_rc_ct_callback);
    if (is_passthru_active) {
      esp_avrc_tg_init();
      esp_avrc_tg_register_callback(ccall_app_rc_tg_callback);
    }
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    for (auto ev : avrc_rn_events) esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ev);
    if (esp_avrc_tg_set_rn_evt_cap(&evt_set) != ESP_OK) Serial.println("[bt] esp_avrc_tg_set_rn_evt_cap failed");
    esp_a2d_source_init();
    esp_a2d_register_callback(&ccall_app_a2d_callback);
    esp_a2d_source_register_data_callback(&ccall_bt_app_a2d_data_cb);
    set_scan_mode_connectable(false);
    if (reconnect_status == AutoReconnect && has_last_connection()) {
      memcpy(peer_bd_addr, last_connection, ESP_BD_ADDR_LEN);
      connect_to(last_connection);
      s_a2d_state = APP_AV_STATE_CONNECTING;
      Serial.println("[bt] connecting right away");
    } else {
      s_a2d_state = APP_AV_STATE_DISCOVERING;
      esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    }
    int tmr_id = 0;
    s_tmr = xTimerCreate("connTmr", (2000 / portTICK_PERIOD_MS), pdTRUE, (void*)&tmr_id, ccall_a2d_app_heart_beat);
    xTimerStart(s_tmr, portMAX_DELAY);
  }
};
LinkSource a2dp;
std::vector<Device> found;          // discovered while scanning
Device saved;                       // remembered headphone
bool hasSaved = false;
bool speakerMode = false;     // the user chose the built-in speaker; the remembered headphones are kept
inline bool preferred() { return hasSaved && !speakerMode; }   // sound goes to the headphones (waits for them)
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

// Frame-aligned single-producer / single-consumer ring (decoder -> Bluetooth
// callback). A byte-oriented stream buffer can split a stereo frame between two
// transfers, which shifts every later sample and sounds like heavy static.
static const uint32_t RING_FRAMES = 16384;     // ~370 ms of 44.1 kHz stereo (absorbs decoder stalls)
uint32_t* ring = nullptr;                      // PSRAM; one frame = left | right << 16
volatile uint32_t rHead = 0, rTail = 0;        // monotonic counters
volatile bool flushReq = false;
volatile float fadeTarget = 1.0f;            // 0 = fade the output out, 1 = full level (set by the player)
static float cbGain = 0.0f;                  // gain currently applied by the callback
static bool cbWasSilent = true;              // the previous callback delivered silence
inline uint32_t buffered() { return rHead - rTail; }
uint8_t volume127 = 76;
volatile int8_t playRequest = 0;    // from the headphone buttons / in-ear sensor: 1 play, -1 pause
volatile int8_t navRequest = 0;     // +1 next track, -1 previous track
volatile int audioState = -1;       // esp_a2d_audio_state_t of the stream (0 remote suspend, 1 stopped, 2 started)
volatile bool remoteSuspend = false;   // the headphone suspended the stream itself (taken off)
volatile uint32_t framesAudio = 0, framesSilence = 0;   // diagnostics: what the headphone pulled
volatile bool wantAudio = false;              // the player is playing (set by the player): silence then is an underrun
volatile uint32_t starveEvents = 0, starveFrames = 0;   // underruns while playing
uint32_t startedAtMs = 0;
uint32_t lastMediaCmdMs = 0;

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
  bool spk = p.getBool("spk", false);
  p.end();
  hasSaved = name.length() && n == 6;
  speakerMode = hasSaved && spk;
  if (hasSaved) { saved.name = name; memcpy(saved.addr, addr, 6); saved.rssi = 0; }
}

void saveDevice(const Device& d) {
  Preferences p;
  if (!p.begin("listenbt", false)) return;
  p.putString("name", d.name);
  p.putBytes("addr", d.addr, 6);
  p.putBool("spk", false);
  p.end();
  saved = d; hasSaved = true; speakerMode = false;
}

void setSpeakerMode(bool on) {
  speakerMode = on;
  Preferences p;
  if (p.begin("listenbt", false)) { p.putBool("spk", on); p.end(); }
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

static void onAudioState(esp_a2d_audio_state_t state, void*) {
  audioState = (int)state;
  if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) remoteSuspend = true;
  Serial.printf("[bt] audio state %d\n", (int)state);
  ++version;
}

static int32_t dataCb(Frame* frames, int32_t count) {
  if (!frames || count <= 0) return 0;   // the stack calls with no buffer when the stream stops
  if (flushReq) { rTail = rHead; flushReq = false; }
  uint32_t avail = rHead - rTail;
  size_t gotFrames = avail < (uint32_t)count ? avail : (uint32_t)count;
  for (size_t i = 0; i < gotFrames; ++i) memcpy(&frames[i], &ring[(rTail + i) % RING_FRAMES], 4);
  __sync_synchronize();
  rTail += gotFrames;
  // Audio that starts, stops or jumps abruptly makes the headphone DAC pop.
  // Ramp the level (about 30 ms) after every silence and towards fadeTarget.
  if (gotFrames) {
    if (cbWasSilent) cbGain = 0.0f;
    cbWasSilent = false;
    const float step = 1.0f / 1300.0f, target = fadeTarget;
    for (size_t i = 0; i < gotFrames; ++i) {
      if (cbGain < target) { cbGain += step; if (cbGain > target) cbGain = target; }
      else if (cbGain > target) { cbGain -= step; if (cbGain < target) cbGain = target; }
      if (cbGain < 0.999f) { frames[i].channel1 = (int16_t)(frames[i].channel1 * cbGain); frames[i].channel2 = (int16_t)(frames[i].channel2 * cbGain); }
    }
  }
  if (gotFrames < (size_t)count) {
    if (wantAudio) { ++starveEvents; starveFrames += count - gotFrames; }
    cbWasSilent = true;
  }
  framesAudio += gotFrames; framesSilence += count - gotFrames;
  if ((int32_t)gotFrames < count) memset(frames + gotFrames, 0, (count - gotFrames) * 4);
  return count;
}

static void startTask(void*) {
  Serial.printf("[bt] starting (free internal %u, largest %u)\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  if (!ring) ring = (uint32_t*)heap_caps_malloc(RING_FRAMES * 4, MALLOC_CAP_SPIRAM);
  rHead = rTail = 0;
  a2dp.set_ssp_enabled(true);               // AirPods need Secure Simple Pairing
  a2dp.set_local_name("SpaceClock");
  a2dp.set_ssid_callback(onDiscovered);
  a2dp.set_on_connection_state_changed(onConnection);
  a2dp.set_data_callback_in_frames(dataCb);
  a2dp.set_avrc_passthru_command_callback(onKey);
  a2dp.set_on_audio_state_changed(onAudioState);
  if (connectByAddr) a2dp.set_auto_reconnect(connectAddr, 100000);   // keep paging the remembered device (never fall back to scanning)
  else a2dp.set_auto_reconnect(false);
  a2dp.start();
  a2dp.set_volume(volume127);
  starting = false;
  startedAtMs = millis();
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
  // Hang up first and let the stack drain its queue: tearing the profile down while
  // events are still queued crashes it.
  if (linkUp) {
    a2dp.disconnect();
    for (int i = 0; i < 60 && linkUp; ++i) delay(50);
    delay(1200);
  }
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
  flushReq = true;
  status = "";
  Serial.printf("[bt] stopped (free internal %u)\n", (unsigned)freeInternal());
  ++version;
}

inline bool connected() { return running && !starting && linkUp; }

// A remembered headphone that dropped (taken off, out of range) is paged again
// every few seconds; it cannot be found by scanning unless it is in pairing mode.
void retryConnect(uint32_t nowMs) {
  static uint32_t last = 0;
  if (last < startedAtMs) last = 0;
  if (!preferred() || !running || starting || linkUp || connecting || scanOnly) return;
  // Right after start the profile is not ready yet: try every 2 s for the first
  // 20 s (a connect that is refused costs nothing), then every 12 s.
  if (nowMs - last < 20000UL) return;   // the library keeps paging itself; this is only a safety net
  if (nowMs - startedAtMs < 8000UL) return;        // the library already pages on its own at start
  last = nowMs;
  bool ok = a2dp.connect_to(connectAddr);
  Serial.printf("[bt] paging the remembered headphones (%s)\n", ok ? "request accepted" : "refused");
}
// A remembered headphone is being (re)connected: playback waits for it a few seconds.
inline bool linking() { return preferred() && running && !scanOnly && !linkUp; }

void setVolumePercent(int percent) {
  volume127 = (uint8_t)constrain((int)lroundf(127.0f * sqrtf(percent / 100.0f)), 0, 127);   // perceptual curve: the stream gain is linear
  if (running && !starting) a2dp.set_volume(volume127);
}

void flushAudio() { flushReq = true; }

// Keep the AVDTP stream in step with playback: suspended while paused (so the
// headphone knows the next button press means "play"), started while playing.
void maintainMedia(bool wantPlaying, uint32_t nowMs) {
  if (!connected() || nowMs - lastMediaCmdMs < 1200UL) return;
  if (!wantPlaying && audioState == ESP_A2D_AUDIO_STATE_STARTED) {
    lastMediaCmdMs = nowMs;
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    Serial.println("[bt] stream suspend");
  } else if (wantPlaying && audioState != ESP_A2D_AUDIO_STATE_STARTED) {
    lastMediaCmdMs = nowMs;
    remoteSuspend = false;
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    Serial.println("[bt] stream start");
  }
}

// Queue interleaved stereo frames; waits up to `waitMs` for space. Returns frames queued.
size_t write(const int16_t* stereo, size_t frames, uint32_t waitMs) {
  if (!ring) return 0;
  uint32_t deadline = millis() + waitMs;
  size_t done = 0;
  while (done < frames) {
    uint32_t freeFrames = RING_FRAMES - (rHead - rTail);
    if (freeFrames == 0) {
      if ((int32_t)(millis() - deadline) >= 0) break;
      vTaskDelay(1);
      continue;
    }
    size_t n = frames - done < freeFrames ? frames - done : freeFrames;
    for (size_t i = 0; i < n; ++i) memcpy(&ring[(rHead + i) % RING_FRAMES], stereo + (done + i) * 2, 4);
    __sync_synchronize();
    rHead += n;
    done += n;
  }
  return done;
}

}  // namespace lbt
