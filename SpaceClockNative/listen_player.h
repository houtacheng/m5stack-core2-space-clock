// Listening mode engine: browses folders / playlists on the SD card and plays
// MP3 files on the built-in speaker. A background task decodes with Helix,
// optionally changes speed (pitch preserved, see wsola.h) and feeds M5.Speaker.
#pragma once
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <map>
#include <algorithm>
#include <mp3dec.h>
#include "wsola.h"

namespace listen {

struct Track { String path, name; uint32_t size; };
struct Entry { String name, path; bool isDir; uint32_t size; };

// ---- playback state -------------------------------------------------------
std::vector<Track> queue;           // what is being played (a folder or playlist)
int current = -1;
String ctxKey;                      // "D:/folder" or "P:playlist"
uint8_t mode = 0;                   // 0 once through, 1 repeat track, 2 repeat all, 3 shuffle
std::vector<uint16_t> order;        // shuffle permutation
int orderPos = 0;
volatile uint8_t speedCode = 2;     // index into SPEEDS
volatile bool playing = false;      // decode task alive
volatile bool paused = false;
volatile bool stopReq = false;
volatile bool finished = false;     // track ended by itself
volatile bool seekReq = false;
volatile float seekFrac = 0;
volatile uint32_t posBytes = 0, totalBytes = 0, dataStart = 0;
volatile int bitrate = 0;
volatile uint32_t decodeErrors = 0, underruns = 0;
volatile uint32_t busyPermille = 0;   // decode task CPU share, for diagnostics
uint8_t volume = 60;                // percent
bool sdMounted = false;
const int CH = 6;                   // M5.Speaker channel used by the player

static const float SPEEDS[5] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
static const char* const SPEED_LABELS[5] = {"0.5X", "0.75X", "1X", "1.5X", "2X"};
static const char* const MODE_LABELS[4] = {"不循環", "單曲循環", "全部循環", "亂序播放"};

// ---- remembered settings (SD card, so they follow the files) ---------------
std::map<String, uint8_t> speeds;   // file path -> speed code
std::map<String, uint8_t> modes;    // folder/playlist key -> mode
static const char* SETTINGS_FILE = "/.listen.txt";
static const char* PLAYLIST_DIR = "/.playlists";

bool mountSd() {
  if (sdMounted) return true;
  SPI.begin(18, 38, 23, -1);
  sdMounted = SD.begin(4, SPI, 25000000);
  return sdMounted;
}

static void loadSettings() {
  speeds.clear(); modes.clear();
  File f = SD.open(SETTINGS_FILE);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    int a = line.indexOf('\t'), b = line.lastIndexOf('\t');
    if (line.length() < 5 || a < 0 || b <= a) continue;
    String key = line.substring(a + 1, b);
    int v = line.substring(b + 1).toInt();
    if (line[0] == 'S' && v >= 0 && v < 5) speeds[key] = v;
    else if (line[0] == 'M' && v >= 0 && v < 4) modes[key] = v;
  }
  f.close();
}

static void saveSettings() {
  if (!sdMounted) return;
  SD.remove(SETTINGS_FILE);
  File f = SD.open(SETTINGS_FILE, FILE_WRITE);
  if (!f) return;
  for (auto& kv : speeds) if (kv.second != 2) f.print("S\t" + kv.first + "\t" + String(kv.second) + "\n");
  for (auto& kv : modes) if (kv.second != 0) f.print("M\t" + kv.first + "\t" + String(kv.second) + "\n");
  f.close();
}

bool begin() {
  if (!mountSd()) return false;
  static bool loaded = false;
  if (!loaded) { loadSettings(); loaded = true; }
  return true;
}

uint8_t speedFor(const String& path) { auto it = speeds.find(path); return it == speeds.end() ? 2 : it->second; }
uint8_t modeFor(const String& key) { auto it = modes.find(key); return it == modes.end() ? 0 : it->second; }

// ---- browsing ---------------------------------------------------------------
static bool isMp3(const String& name) {
  String l = name; l.toLowerCase();
  return l.endsWith(".mp3");
}

// Sub-folders first, then MP3 files, both sorted by name. Hidden entries
// (macOS "._" files, our own ".playlists") are skipped.
void listDir(const String& dir, std::vector<Entry>& out) {
  out.clear();
  File d = SD.open(dir);
  if (!d) return;
  for (File e = d.openNextFile(); e && out.size() < 400; e = d.openNextFile()) {
    String name = e.name();
    if (name.startsWith(".")) continue;
    Entry en;
    en.name = name; en.path = dir == "/" ? "/" + name : dir + "/" + name;
    en.isDir = e.isDirectory(); en.size = e.size();
    if (en.isDir) { out.push_back(en); continue; }
    if (!isMp3(name)) continue;
    en.name = name.substring(0, name.length() - 4);
    out.push_back(en);
  }
  d.close();
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.isDir != b.isDir) return a.isDir;
    return a.name < b.name;
  });
}

String parentOf(const String& dir) {
  int slash = dir.lastIndexOf('/');
  return slash <= 0 ? String("/") : dir.substring(0, slash);
}

// ---- playlists ------------------------------------------------------------
static String playlistFile(const String& name) { return String(PLAYLIST_DIR) + "/" + name + ".txt"; }

void playlistNames(std::vector<String>& out) {
  out.clear();
  File d = SD.open(PLAYLIST_DIR);
  if (!d) return;
  for (File e = d.openNextFile(); e && out.size() < 100; e = d.openNextFile()) {
    String n = e.name();
    if (e.isDirectory() || n.startsWith(".") || !n.endsWith(".txt")) continue;
    out.push_back(n.substring(0, n.length() - 4));
  }
  d.close();
  std::sort(out.begin(), out.end());
}

void playlistPaths(const String& name, std::vector<String>& out) {
  out.clear();
  File f = SD.open(playlistFile(name));
  if (!f) return;
  while (f.available() && out.size() < 300) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length()) out.push_back(line);
  }
  f.close();
}

static void playlistWrite(const String& name, const std::vector<String>& paths) {
  SD.remove(playlistFile(name));
  File f = SD.open(playlistFile(name), FILE_WRITE);
  if (!f) return;
  for (auto& p : paths) f.print(p + "\n");
  f.close();
}

// Creates "播放清單 N" with the lowest unused number.
String playlistCreate() {
  SD.mkdir(PLAYLIST_DIR);
  std::vector<String> names; playlistNames(names);
  for (int n = 1; n < 100; ++n) {
    String name = "播放清單" + String(n);
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      playlistWrite(name, {});
      return name;
    }
  }
  return "";
}

// 1 added, 0 already present, -1 error
int playlistAdd(const String& name, const String& path) {
  std::vector<String> paths; playlistPaths(name, paths);
  if (std::find(paths.begin(), paths.end(), path) != paths.end()) return 0;
  paths.push_back(path);
  playlistWrite(name, paths);
  return 1;
}

void playlistRemoveAt(const String& name, int index) {
  std::vector<String> paths; playlistPaths(name, paths);
  if (index < 0 || index >= (int)paths.size()) return;
  paths.erase(paths.begin() + index);
  playlistWrite(name, paths);
}

void playlistDelete(const String& name) {
  SD.remove(playlistFile(name));
  modes.erase("P:" + name);
  saveSettings();
}

// ---- queue / modes ----------------------------------------------------------
static void shuffleOrder(int keepFirst) {
  order.clear();
  for (int i = 0; i < (int)queue.size(); ++i) if (i != keepFirst) order.push_back(i);
  for (int i = (int)order.size() - 1; i > 0; --i) std::swap(order[i], order[esp_random() % (i + 1)]);
  if (keepFirst >= 0) order.insert(order.begin(), keepFirst);
  orderPos = 0;
}

static int nextShuffle() {
  if (queue.empty()) return -1;
  if (orderPos + 1 >= (int)order.size()) shuffleOrder(-1);   // new round
  else ++orderPos;
  if (queue.size() > 1 && order[orderPos] == current) { if (orderPos + 1 < (int)order.size()) ++orderPos; }
  return order[orderPos];
}

// Next track when the current one ended by itself (-1 = stop).
int indexAfterFinish() {
  int n = (int)queue.size();
  if (n == 0) return -1;
  if (mode == 1) return current;
  if (mode == 3) return nextShuffle();
  if (current + 1 < n) return current + 1;
  return mode == 2 ? 0 : -1;
}

int indexForNext() {
  int n = (int)queue.size();
  if (n == 0) return -1;
  if (mode == 3) return nextShuffle();
  if (current + 1 < n) return current + 1;
  return mode == 2 ? 0 : -1;
}

int indexForPrev() {
  int n = (int)queue.size();
  if (n == 0) return -1;
  if (mode == 3) return orderPos > 0 ? order[--orderPos] : current;
  if (current > 0) return current - 1;
  return mode == 2 ? n - 1 : -1;
}

void setMode(const String& key, uint8_t m) {
  modes[key] = m % 4;
  saveSettings();
  if (key == ctxKey) {
    mode = m % 4;
    if (mode == 3) shuffleOrder(current);
  }
}

void setSpeedCode(uint8_t code) {
  if (current < 0 || current >= (int)queue.size()) return;
  speedCode = code % 5;
  speeds[queue[current].path] = speedCode;
  saveSettings();
}

// ---- decode task ------------------------------------------------------------
static size_t id3Size(File& f) {
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) { f.seek(0); return 0; }
  return (((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F)) + 10;
}

static void decodeTask(void* arg) {
  Track t = queue[(int)(intptr_t)arg];
  File f = SD.open(t.path);
  HMP3Decoder dec = f ? MP3InitDecoder() : nullptr;
  const size_t INBUF = 6 * 1024, SLOT = 2816;   // 64 ms per slot at 44.1 kHz: cushion against SD / Wi-Fi hiccups
  uint8_t* in = (uint8_t*)heap_caps_malloc(INBUF, MALLOC_CAP_SPIRAM);
  int16_t* pcm = (int16_t*)heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
  int16_t* mono = (int16_t*)heap_caps_malloc(1152 * sizeof(int16_t), MALLOC_CAP_8BIT);
  int16_t* out = (int16_t*)heap_caps_malloc(4 * SLOT * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  Wsola* ws = new Wsola();
  bool ok = f && dec && in && pcm && mono && out && ws;
  if (!ok) Serial.printf("[listen] cannot start %s\n", t.path.c_str());
  size_t have = 0;
  bool eof = false, flushed = false, wsActive = false, wsFinished = false, started = false;
  int slot = 0, acc = 0, rate = 44100;
  int64_t startUs = esp_timer_get_time(), busyUs = 0, waitUs = 0;
  uint8_t usedSpeed = 2;
  if (ok) {
    size_t skip = id3Size(f);
    f.seek(skip);
    dataStart = skip; totalBytes = f.size(); posBytes = skip;
    Serial.printf("[listen] playing %s (%u bytes, speed %s)\n", t.path.c_str(), (unsigned)totalBytes, SPEED_LABELS[speedCode]);
  }

  // Send accumulated samples to the speaker (blocks while its queue is full).
  auto emit = [&](int count) {
    int64_t w0 = esp_timer_get_time();
    while (!stopReq && !paused && !seekReq && M5.Speaker.isPlaying(CH) >= 2) vTaskDelay(pdMS_TO_TICKS(3));
    waitUs += esp_timer_get_time() - w0;
    if (stopReq || paused || seekReq || count <= 0) return;
    if (started && M5.Speaker.isPlaying(CH) == 0) ++underruns;
    M5.Speaker.playRaw(out + slot * SLOT, count, rate, false, 1, CH, false);
    started = true;
    slot = (slot + 1) & 3; acc = 0;
  };
  // Move whatever the time stretcher can produce into the output slots.
  auto drainStretch = [&]() {
    int16_t* dst;
    int hop = ws->hop();
    while (!stopReq && !paused && !seekReq) {
      if (acc + hop > (int)SLOT) emit(acc);
      if (stopReq || paused || seekReq) return;
      dst = out + slot * SLOT + acc;
      if (!ws->pull(dst)) break;
      acc += hop;
      if (acc + hop > (int)SLOT) emit(acc);
    }
  };

  while (ok && !stopReq) {
    int64_t iterStart = esp_timer_get_time(), waitBefore = waitUs;
    if (paused) {
      if (!flushed) { M5.Speaker.stop(CH); acc = 0; flushed = true; }
      vTaskDelay(pdMS_TO_TICKS(60));
      continue;
    }
    flushed = false;
    if (seekReq) {
      float frac = constrain((float)seekFrac, 0.0f, 0.995f);
      f.seek(dataStart + (size_t)((totalBytes - dataStart) * frac));
      have = 0; eof = false; acc = 0; wsFinished = false;
      if (wsActive) ws->reset();
      seekReq = false;
      M5.Speaker.stop(CH);
    }
    uint8_t want = speedCode;
    if (want != usedSpeed) {          // speed button pressed while playing
      usedSpeed = want;
      bool need = SPEEDS[want] != 1.0f;
      if (need && wsActive) ws->setSpeed(SPEEDS[want]);
      else { wsActive = false; acc = 0; wsFinished = false; }   // re-initialised below
    }

    // Time-stretched output first (it only produces when it has enough input).
    if (wsActive) {
      drainStretch();
      if (wsFinished) break;
      if (stopReq || paused || seekReq) continue;
    }

    if (have < 2048 && !eof) {
      size_t n = f.read(in + have, INBUF - have);
      if (n == 0) eof = true; else have += n;
    }
    posBytes = f.position() - have;
    if (have == 0) {
      if (wsActive && !wsFinished) { ws->finish(); wsFinished = true; drainStretch(); }
      if (acc) emit(acc);
      break;
    }
    int sync = MP3FindSyncWord(in, (int)have);
    if (sync < 0) { have = 0; continue; }   // next pass reads more or flushes at the end
    if (sync > 0) { memmove(in, in + sync, have - sync); have -= sync; }
    unsigned char* ptr = in;
    int bytesLeft = (int)have;
    int r = MP3Decode(dec, &ptr, &bytesLeft, pcm, 0);
    size_t used = have - bytesLeft;
    if (used == 0) used = 1;
    memmove(in, in + used, have - used);
    have -= used;
    if (r != ERR_MP3_NONE) { ++decodeErrors; if (eof && have < 4) have = 0; continue; }
    MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
    bitrate = info.bitrate;
    int ch = max(1, info.nChans);
    size_t n = min<size_t>(1152, info.outputSamps / ch);
    for (size_t i = 0; i < n; ++i)
      mono[i] = ch > 1 ? (int16_t)(((int32_t)pcm[i * ch] + pcm[i * ch + 1]) / 2) : pcm[i];
    int newRate = info.samprate > 0 ? info.samprate : 44100;

    if (SPEEDS[usedSpeed] == 1.0f) {
      if (newRate != rate && acc) emit(acc);
      rate = newRate;
      memcpy(out + slot * SLOT + acc, mono, n * sizeof(int16_t));
      acc += n;
      if (acc + 1152 > (int)SLOT) emit(acc);
    } else {
      if (!wsActive || newRate != rate) {
        rate = newRate;
        if (!ws->begin(rate, SPEEDS[usedSpeed])) { Serial.println("[listen] stretcher out of memory"); break; }
        wsActive = true; wsFinished = false; acc = 0;
      }
      ws->push(mono, n);
    }
    // Never hog the core: the main loop, touch and networking share it.
    busyUs += esp_timer_get_time() - iterStart - (waitUs - waitBefore);
    int64_t span = esp_timer_get_time() - startUs;
    if (span > 0) busyPermille = (uint32_t)(busyUs * 1000 / span);
    vTaskDelay(1);
  }
  bool natural = ok && !stopReq;
  if (natural) {  // let the last frames play out
    for (int i = 0; i < 150 && !stopReq && M5.Speaker.isPlaying(CH); ++i) vTaskDelay(pdMS_TO_TICKS(10));
  }
  delete ws;
  if (in) free(in);
  if (pcm) free(pcm);
  if (mono) free(mono);
  if (out) free(out);
  if (dec) MP3FreeDecoder(dec);
  if (f) f.close();
  Serial.printf("[listen] track ended (%s), decode errors %u\n", natural ? "finished" : "stopped", (unsigned)decodeErrors);
  playing = false;
  if (natural) finished = true;
  vTaskDelete(nullptr);
}

void stop() {
  stopReq = true;
  for (int i = 0; i < 150 && playing; ++i) delay(10);
  M5.Speaker.stop(CH);
  playing = false; paused = false; finished = false;
}

// Play queue[index] (the queue must already be set).
bool startIndex(int index) {
  if (index < 0 || index >= (int)queue.size()) return false;
  stop();
  current = index;
  speedCode = speedFor(queue[index].path);
  stopReq = false; paused = false; finished = false; seekReq = false;
  posBytes = 0; dataStart = 0; totalBytes = queue[index].size; decodeErrors = 0; underruns = 0;
  if (!M5.Speaker.isRunning()) M5.Speaker.begin();
  M5.Speaker.setVolume((uint8_t)(volume * 255 / 100));
  playing = true;
  if (xTaskCreatePinnedToCore(decodeTask, "listen", 12288, (void*)(intptr_t)index, 2, nullptr, 1) != pdPASS) {
    playing = false;
    return false;
  }
  return true;
}

// Replace the queue by `tracks` (a folder or a playlist) and start at `index`.
bool startQueue(const std::vector<Track>& tracks, const String& key, int index) {
  stop();
  queue = tracks;
  ctxKey = key;
  mode = modeFor(key);
  current = index;
  if (mode == 3) shuffleOrder(index);
  return startIndex(index);
}

void setVolume(int percent) {
  volume = (uint8_t)constrain(percent, 0, 100);
  M5.Speaker.setVolume((uint8_t)(volume * 255 / 100));
}

}  // namespace listen
