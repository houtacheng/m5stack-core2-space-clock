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
#include <esp_heap_caps_init.h>
#include "wsola.h"
#include "listen_bt.h"

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
volatile bool pausedByLink = false;  // paused because the Bluetooth link dropped (resumes when it returns)
volatile bool finished = false;     // track ended by itself
volatile bool seekReq = false;
volatile float seekFrac = 0;
volatile uint32_t posBytes = 0, totalBytes = 0, dataStart = 0;
uint32_t startRequestedMs = 0;     // for the start-up timing log
volatile uint32_t durationSec = 0;   // from the Xing/Info/VBRI header when the file has one (0 = unknown)
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

// The SD card sits on one SPI bus: the decode task, the touch UI and the web
// page must never talk to it at the same time.
static SemaphoreHandle_t sdMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
  return m;
}
struct SdLock {
  SdLock() { xSemaphoreTakeRecursive(sdMutex(), portMAX_DELAY); }
  ~SdLock() { xSemaphoreGiveRecursive(sdMutex()); }
};

bool mountSd() {
  SdLock lock;
  if (sdMounted) return true;
  SPI.begin(18, 38, 23, -1);
  sdMounted = SD.begin(4, SPI, 25000000);
  return sdMounted;
}

static void loadSettings() {
  SdLock lock;
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
  SdLock lock;
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
  SdLock lock;
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
  SdLock lock;
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
  SdLock lock;
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
  SdLock lock;
  SD.remove(playlistFile(name));
  File f = SD.open(playlistFile(name), FILE_WRITE);
  if (!f) return;
  for (auto& p : paths) f.print(p + "\n");
  f.close();
}

// Creates "播放清單 N" with the lowest unused number.
String playlistCreate() {
  SdLock lock;
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
  SdLock lock;
  SD.remove(playlistFile(name));
  modes.erase("P:" + name);
  saveSettings();
}

// ---- renaming (web page) -----------------------------------------------------
static bool validName(const String& n) {
  if (!n.length() || n.length() > 80 || n.startsWith(".")) return false;
  for (size_t i = 0; i < n.length(); ++i) {
    char c = n[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
  }
  return true;
}

// Moves settings keys and playlist entries that pointed below oldPath.
static void retarget(const String& oldPath, const String& newPath) {
  std::map<String, uint8_t> sp, mo;
  for (auto& kv : speeds) sp[kv.first == oldPath || kv.first.startsWith(oldPath + "/") ? newPath + kv.first.substring(oldPath.length()) : kv.first] = kv.second;
  for (auto& kv : modes) {
    String k = kv.first;
    if (k.startsWith("D:" + oldPath) && (k.length() == oldPath.length() + 2 || k[oldPath.length() + 2] == '/'))
      k = "D:" + newPath + k.substring(oldPath.length() + 2);
    mo[k] = kv.second;
  }
  speeds = sp; modes = mo;
  std::vector<String> names; playlistNames(names);
  for (auto& n : names) {
    std::vector<String> paths; playlistPaths(n, paths);
    bool changed = false;
    for (auto& p : paths) if (p == oldPath || p.startsWith(oldPath + "/")) { p = newPath + p.substring(oldPath.length()); changed = true; }
    if (changed) playlistWrite(n, paths);
  }
  saveSettings();
}

// Renames a folder (path "/a/b") or a playlist (path "@pl:NAME"). Returns "" or an error text.
String renameItem(const String& path, const String& newName) {
  SdLock lock;
  if (!validName(newName)) return "名稱不可用";
  if (path.startsWith("@pl:")) {
    String old = path.substring(4);
    if (old == newName) return "";
    if (SD.exists(playlistFile(newName))) return "已有同名的播放清單";
    if (!SD.rename(playlistFile(old), playlistFile(newName))) return "重新命名失敗";
    auto it = modes.find("P:" + old);
    if (it != modes.end()) { modes["P:" + newName] = it->second; modes.erase(it); saveSettings(); }
    return "";
  }
  String parent = parentOf(path);
  String target = parent == "/" ? "/" + newName : parent + "/" + newName;
  if (target == path) return "";
  if (SD.exists(target)) return "已有同名的資料夾";
  if (!SD.rename(path, target)) return "重新命名失敗";
  retarget(path, target);
  return "";
}

// ---- file operations for the web file browser -----------------------------------
static bool removeTree(const String& path) {
  File f = SD.open(path);
  if (!f) return false;
  bool isDir = f.isDirectory();
  if (isDir) {
    std::vector<String> children;
    for (File e = f.openNextFile(); e; e = f.openNextFile()) children.push_back(path == "/" ? "/" + String(e.name()) : path + "/" + String(e.name()));
    f.close();
    for (auto& c : children) removeTree(c);
    return SD.rmdir(path);
  }
  f.close();
  return SD.remove(path);
}

// Deletes a file or a whole folder. Returns "" or an error text.
String deleteItem(const String& path) {
  SdLock lock;
  if (path.length() < 2 || path.indexOf("..") >= 0) return "不可刪除這個路徑";
  if (!SD.exists(path)) return "找不到這個檔案";
  if (!removeTree(path)) return "刪除失敗";
  for (auto it = speeds.begin(); it != speeds.end();) it = (it->first == path || it->first.startsWith(path + "/")) ? speeds.erase(it) : std::next(it);
  for (auto it = modes.begin(); it != modes.end();) it = (it->first == "D:" + path || it->first.startsWith("D:" + path + "/")) ? modes.erase(it) : std::next(it);
  saveSettings();
  return "";
}

// Moves a file or folder into destDir ("/" = top level). Returns "" or an error text.
String moveItem(const String& path, const String& destDir) {
  SdLock lock;
  if (path.length() < 2 || path.indexOf("..") >= 0 || destDir.indexOf("..") >= 0) return "不可移動這個路徑";
  if (!SD.exists(path)) return "找不到這個檔案";
  String base = path.substring(path.lastIndexOf('/') + 1);
  String target = destDir == "/" ? "/" + base : destDir + "/" + base;
  if (target == path) return "";
  if (destDir != "/" && !SD.exists(destDir)) return "目的資料夾不存在";
  if (destDir == path || destDir.startsWith(path + "/")) return "不能把資料夾移進它自己裡面";
  if (SD.exists(target)) return "目的地已有同名項目";
  if (!SD.rename(path, target)) return "移動失敗";
  retarget(path, target);
  return "";
}

String makeDir(const String& dir, const String& name) {
  SdLock lock;
  if (!validName(name)) return "資料夾名稱不可用";
  String target = dir == "/" ? "/" + name : dir + "/" + name;
  if (SD.exists(target)) return "已有同名項目";
  return SD.mkdir(target) ? "" : "建立失敗";
}

// All folders (up to three levels) for the web page.
static void walkFolders(const String& dir, int depth, std::vector<String>& out) {
  File d = SD.open(dir);
  if (!d) return;
  for (File e = d.openNextFile(); e && out.size() < 80; e = d.openNextFile()) {
    String name = e.name();
    if (!e.isDirectory() || name.startsWith(".")) continue;
    String full = dir == "/" ? "/" + name : dir + "/" + name;
    out.push_back(full);
    if (depth > 0) walkFolders(full, depth - 1, out);
  }
  d.close();
}
void allFolders(std::vector<String>& out) { SdLock lock; out.clear(); walkFolders("/", 2, out); std::sort(out.begin(), out.end()); }

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
// The MP3 decoder keeps ~20 KB of state. When internal RAM is scarce (Bluetooth
// is running) it is placed in PSRAM instead, at the price of some speed.
size_t mallocThreshold = 16384;   // size from which malloc() prefers PSRAM (lowered while listening)
static HMP3Decoder newDecoder() {
  bool scarce = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 70000;
  if (scarce) heap_caps_malloc_extmem_enable(1);
  HMP3Decoder d = MP3InitDecoder();
  if (scarce) heap_caps_malloc_extmem_enable(mallocThreshold);
  return d;
}
static size_t id3Size(File& f) {
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) { f.seek(0); return 0; }
  return (((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F)) + 10;
}

// Duration from a Xing/Info/VBRI header in the first frame (variable bit rate files).
static uint32_t headerDuration(File& f, size_t skip) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);   // not on the small task stack
  if (!buf) return 0;
  struct Free { uint8_t* p; ~Free() { free(p); } } guard{buf};
  f.seek(skip);
  int n = f.read(buf, 2048);
  if (n < 64) return 0;
  int sync = MP3FindSyncWord(buf, n);
  if (sync < 0 || sync + 48 > n) return 0;
  const uint8_t* h = buf + sync;
  int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
  if (ver == 1 || layer != 1) return 0;                    // reserved version / not layer III
  bool mpeg1 = ver == 3, mono = ((h[3] >> 6) & 3) == 3;
  int idx = (h[2] >> 2) & 3;
  static const int rates[3][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}};
  int rate = ver == 3 ? (idx == 0 ? 44100 : idx == 1 ? 48000 : 32000) : rates[ver][idx];
  if (!rate || idx == 3) return 0;
  int spf = mpeg1 ? 1152 : 576;
  int base = sync + 4 + ((h[1] & 1) ? 0 : 2);                // header + optional CRC
  int xing = base + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
  if (xing + 12 < n && (!memcmp(buf + xing, "Xing", 4) || !memcmp(buf + xing, "Info", 4)) && (buf[xing + 7] & 1)) {
    uint32_t frames = ((uint32_t)buf[xing + 8] << 24) | (buf[xing + 9] << 16) | (buf[xing + 10] << 8) | buf[xing + 11];
    return (uint32_t)((uint64_t)frames * spf / rate);
  }
  int vbri = base + 32;
  if (vbri + 18 < n && !memcmp(buf + vbri, "VBRI", 4)) {
    uint32_t frames = ((uint32_t)buf[vbri + 14] << 24) | (buf[vbri + 15] << 16) | (buf[vbri + 16] << 8) | buf[vbri + 17];
    return (uint32_t)((uint64_t)frames * spf / rate);
  }
  return 0;
}

static void decodeTask(void* arg) {
  Track t = queue[(int)(intptr_t)arg];
  File f;
  { SdLock lock; f = SD.open(t.path); }
  Serial.printf("[listen] file opened %u ms after the request\n", (unsigned)(millis() - startRequestedMs));
  bool firstSound = false;
  HMP3Decoder dec = f ? newDecoder() : nullptr;
  const size_t INBUF = 6 * 1024, SLOT = 2816;   // 64 ms per slot at 44.1 kHz: cushion against SD / Wi-Fi hiccups
  uint8_t* in = (uint8_t*)heap_caps_malloc(INBUF, MALLOC_CAP_SPIRAM);
  // Internal RAM is scarce while Bluetooth runs: keep the buffers in PSRAM.
  int16_t* pcm = (int16_t*)heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  int16_t* mono = (int16_t*)heap_caps_malloc(1152 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
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
    size_t skip;
    { SdLock lock; skip = id3Size(f); durationSec = headerDuration(f, skip); f.seek(skip); }
    dataStart = skip; totalBytes = f.size(); posBytes = skip;
    Serial.printf("[listen] playing %s (%u bytes, speed %s), header read after %u ms (id3 %u bytes, duration %u s)\n", t.path.c_str(), (unsigned)totalBytes, SPEED_LABELS[speedCode], (unsigned)(millis() - startRequestedMs), (unsigned)skip, (unsigned)durationSec);
  }

  // Bluetooth path: mono samples at `rate` become 44.1 kHz stereo frames.
  bool onBt = false;
  double btPos = 0; int16_t btPrev = 0;
  auto emitBt = [&](const int16_t* mono, int n) {
    static int16_t stereo[512];
    const double step = rate / 44100.0;
    int i = 0;
    while (i < n && !stopReq && !paused && !seekReq && lbt::connected()) {
      int outFrames = 0;
      if (rate == 44100) {
        while (outFrames < 256 && i < n) { stereo[outFrames * 2] = stereo[outFrames * 2 + 1] = mono[i++]; ++outFrames; }
      } else {
        while (outFrames < 256) {
          int idx = (int)floor(btPos);
          if (idx + 1 >= n) break;
          double frac = btPos - idx;
          int16_t a = idx < 0 ? btPrev : mono[idx], b = mono[idx + 1];
          int16_t v = (int16_t)(a + (b - a) * frac);
          stereo[outFrames * 2] = stereo[outFrames * 2 + 1] = v;
          ++outFrames; btPos += step;
        }
        if (outFrames == 0 || btPos + 1 >= n) { i = n; }
      }
      size_t sent = 0;
      while (sent < (size_t)outFrames && !stopReq && !paused && !seekReq && lbt::connected()) sent += lbt::write(stereo + sent * 2, outFrames - sent, 50);   // a lost link must not block the decoder
    }
    if (rate != 44100) { btPos -= n; if (n) btPrev = mono[n - 1]; }
  };
  // Send accumulated samples to the speaker (blocks while its queue is full).
  auto emit = [&](int count) {
    if (!firstSound && count > 0) { firstSound = true; Serial.printf("[listen] first audio %u ms after the request\n", (unsigned)(millis() - startRequestedMs)); }
    if (count > 0 && lbt::connected()) {
      if (!onBt) { M5.Speaker.stop(CH); M5.Speaker.end(); onBt = true; btPos = 0; btPrev = 0; }   // frees its RAM for the stack
      emitBt(out + slot * SLOT, count);
      acc = 0;
      return;
    }
    if (lbt::preferred()) {              // headphones are set up: no speaker; wait for them
      lbt::flushAudio(); onBt = false; acc = 0;
      paused = true; pausedByLink = true;
      return;
    }
    if (onBt) { lbt::flushAudio(); onBt = false; }
    if (!M5.Speaker.isRunning()) { M5.Speaker.begin(); M5.Speaker.setVolume((uint8_t)(volume * 255 / 100)); }
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
    lbt::wantAudio = !paused && !seekReq;
    if (paused && pausedByLink && lbt::connected()) { paused = false; pausedByLink = false; }   // the headphones are back
    if (paused) {
      if (!flushed) {
        lbt::fadeTarget = 0.0f;                       // let the headphone output fade out first
        if (lbt::connected()) vTaskDelay(pdMS_TO_TICKS(60));
        M5.Speaker.stop(CH); lbt::flushAudio(); acc = 0; flushed = true;
      }
      vTaskDelay(pdMS_TO_TICKS(60));
      continue;
    }
    if (flushed) lbt::fadeTarget = 1.0f;               // resumed: the output fades in with the first audio
    flushed = false;
    if (seekReq) {
      float frac = constrain((float)seekFrac, 0.0f, 0.995f);
      { SdLock lock; f.seek(dataStart + (size_t)((totalBytes - dataStart) * frac)); }
      have = 0; eof = false; acc = 0; wsFinished = false;
      if (wsActive) ws->reset();
      seekReq = false;
      lbt::fadeTarget = 0.0f; if (lbt::connected()) vTaskDelay(pdMS_TO_TICKS(50));
      M5.Speaker.stop(CH); lbt::flushAudio();
      lbt::fadeTarget = 1.0f;
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
      size_t n;
      { SdLock lock; n = f.read(in + have, INBUF - have); }
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
      // The time stretcher's cost grows with the cube of the sample rate: run it at
      // 24 kHz or less (average pairs of samples above 32 kHz).
      int ds = newRate > 32000 ? 2 : 1;
      int wsRate = newRate / ds;
      if (!wsActive || wsRate != rate) {
        rate = wsRate;
        if (!ws->begin(rate, SPEEDS[usedSpeed])) { Serial.println("[listen] stretcher out of memory"); break; }
        wsActive = true; wsFinished = false; acc = 0;
      }
      if (ds == 2) { n /= 2; for (size_t i = 0; i < n; ++i) mono[i] = (int16_t)(((int32_t)mono[2 * i] + mono[2 * i + 1]) / 2); }
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
    for (int i = 0; i < 150 && !stopReq && (M5.Speaker.isPlaying(CH) || (onBt && lbt::buffered() > 1000)); ++i) vTaskDelay(pdMS_TO_TICKS(10));
  }
  Serial.printf("[listen] decode task stack: %u bytes never used\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  delete ws;
  if (in) free(in);
  if (pcm) free(pcm);
  if (mono) free(mono);
  if (out) free(out);
  if (dec) MP3FreeDecoder(dec);
  if (f) { SdLock lock; f.close(); }
  Serial.printf("[listen] track ended (%s), decode errors %u\n", natural ? "finished" : "stopped", (unsigned)decodeErrors);
  playing = false;
  if (natural) finished = true;
  vTaskDelete(nullptr);
}

// Decoder + time-stretch speed on the CPU without pacing (diagnostics).
void bench(const String& path, int code, int seconds) {
  File f; { SdLock lock; f = SD.open(path); }
  if (!f) { Serial.println("[bench] cannot open file"); return; }
  bool scarce = lbt::running;   // Helix lives in PSRAM while Bluetooth runs
  if (scarce) heap_caps_malloc_extmem_enable(1);
  HMP3Decoder dec = MP3InitDecoder();
  if (scarce) heap_caps_malloc_extmem_enable(mallocThreshold);
  uint8_t* in = (uint8_t*)heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
  int16_t* pcm = (int16_t*)heap_caps_malloc(1152 * 2 * 2, MALLOC_CAP_SPIRAM);
  int16_t* mono = (int16_t*)heap_caps_malloc(1152 * 2, MALLOC_CAP_SPIRAM);
  int16_t hop[1024];
  Wsola* ws = new Wsola();
  size_t have = 0; bool eof = false, wsOn = false;
  uint32_t t0 = millis(), outSamples = 0, rate = 0;
  size_t skip; { SdLock lock; skip = id3Size(f); f.seek(skip); }
  while (dec && in && pcm && mono && ws && (millis() - t0 < 30000UL) && (!rate || outSamples < rate * (uint32_t)seconds)) {
    if (have < 2048 && !eof) { size_t n; { SdLock lock; n = f.read(in + have, 6144 - have); } if (!n) eof = true; else have += n; }
    if (!have) break;
    int sync = MP3FindSyncWord(in, (int)have);
    if (sync < 0) { have = 0; continue; }
    if (sync > 0) { memmove(in, in + sync, have - sync); have -= sync; }
    unsigned char* ptr = in; int left = (int)have;
    int r = MP3Decode(dec, &ptr, &left, pcm, 0);
    size_t used = have - left; if (!used) used = 1;
    memmove(in, in + used, have - used); have -= used;
    if (r != ERR_MP3_NONE) continue;
    MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
    int ch = max(1, info.nChans); size_t n = min<size_t>(1152, info.outputSamps / ch);
    for (size_t i = 0; i < n; ++i) mono[i] = ch > 1 ? (int16_t)(((int32_t)pcm[i * ch] + pcm[i * ch + 1]) / 2) : pcm[i];
    if (!rate) rate = info.samprate;
    if (SPEEDS[code] == 1.0f) { outSamples += n; continue; }
    int ds = info.samprate > 32000 ? 2 : 1;
    if (!wsOn) { rate = info.samprate / ds; ws->begin(rate, SPEEDS[code]); wsOn = true; }
    if (ds == 2) { n /= 2; for (size_t i = 0; i < n; ++i) mono[i] = (int16_t)(((int32_t)mono[2 * i] + mono[2 * i + 1]) / 2); }
    ws->push(mono, n);
    while (ws->pull(hop)) outSamples += ws->hop();
  }
  uint32_t ms = max<uint32_t>(1, millis() - t0);
  Serial.printf("[bench] %s speed %s: %u output samples at %u Hz in %u ms -> %.2fx real time (Helix %s, %u MHz)\n", path.c_str(), SPEED_LABELS[code],
                (unsigned)outSamples, (unsigned)rate, (unsigned)ms, outSamples * 1000.0 / ms / (rate ? rate : 1), scarce ? "in PSRAM" : "in internal RAM", (unsigned)getCpuFrequencyMhz());
  delete ws; if (in) free(in); if (pcm) free(pcm); if (mono) free(mono); if (dec) MP3FreeDecoder(dec);
  f.close();
}

void stop() {
  if (playing && lbt::connected()) { lbt::fadeTarget = 0.0f; delay(60); }   // fade out instead of a click
  stopReq = true;
  for (int i = 0; i < 150 && playing; ++i) delay(10);
  M5.Speaker.stop(CH);
  lbt::flushAudio();
  playing = false; paused = false; finished = false;
}

// Play queue[index] (the queue must already be set).
bool startIndex(int index) {
  if (index < 0 || index >= (int)queue.size()) return false;
  startRequestedMs = millis();
  stop();
  current = index;
  speedCode = speedFor(queue[index].path);
  stopReq = false; paused = false; finished = false; seekReq = false;
  lbt::fadeTarget = 1.0f;
  posBytes = 0; dataStart = 0; totalBytes = queue[index].size; decodeErrors = 0; underruns = 0; durationSec = 0;
  pausedByLink = false;
  if (lbt::preferred() && !lbt::connected()) { paused = true; pausedByLink = true; }   // wait for the headphones, silently
  if (lbt::preferred() || lbt::connected()) M5.Speaker.end();   // sound goes to the headphones: give the RAM back
  else {
    if (!M5.Speaker.isRunning()) M5.Speaker.begin();
    M5.Speaker.setVolume((uint8_t)(volume * 255 / 100));
  }
  lbt::setVolumePercent(volume);
  playing = true;
  if (xTaskCreatePinnedToCore(decodeTask, "listen", 5120, (void*)(intptr_t)index, 2, nullptr, 1) != pdPASS) {
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
  lbt::setVolumePercent(volume);
}

}  // namespace listen
