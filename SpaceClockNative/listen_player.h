// Listening mode engine: plays MP3 files from the SD card on the built-in
// speaker. A background task decodes with Helix and feeds M5.Speaker.
#pragma once
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <algorithm>
#include <mp3dec.h>

namespace listen {

struct Track { String path, name, dir; uint32_t size; };

std::vector<Track> tracks;
int current = -1;
volatile bool playing = false;      // decode task alive
volatile bool paused = false;
volatile bool stopReq = false;
volatile bool finished = false;     // track ended by itself (main loop advances)
volatile bool seekReq = false;
volatile float seekFrac = 0;
volatile uint32_t posBytes = 0, totalBytes = 0, dataStart = 0;
volatile int bitrate = 0;
volatile uint32_t decodeErrors = 0;
uint8_t volume = 60;                // percent
bool sdMounted = false;
const int CH = 6;                   // M5.Speaker channel used by the player

bool mountSd() {
  if (sdMounted) return true;
  SPI.begin(18, 38, 23, -1);
  sdMounted = SD.begin(4, SPI, 25000000);
  return sdMounted;
}

static void walk(const String& dir, int depth) {
  File d = SD.open(dir);
  if (!d) return;
  for (File e = d.openNextFile(); e && tracks.size() < 300; e = d.openNextFile()) {
    String name = e.name();
    if (name.startsWith(".")) continue;                 // macOS ._ resource files
    String full = dir == "/" ? "/" + name : dir + "/" + name;
    if (e.isDirectory()) { if (depth > 0) walk(full, depth - 1); continue; }
    String lower = name; lower.toLowerCase();
    if (!lower.endsWith(".mp3")) continue;
    Track t;
    t.path = full;
    t.name = name.substring(0, name.length() - 4);
    int slash = dir.lastIndexOf('/');
    t.dir = dir == "/" ? "" : dir.substring(slash + 1);
    t.size = e.size();
    tracks.push_back(t);
  }
}

// Returns the number of tracks found (0 with sdMounted=false means no card).
int scan() {
  tracks.clear();
  if (!mountSd()) return 0;
  walk("/", 2);
  std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.path < b.path; });
  return (int)tracks.size();
}

static size_t id3Size(File& f) {
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) { f.seek(0); return 0; }
  return (((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F)) + 10;
}

static void decodeTask(void* arg) {
  Track t = tracks[(int)(intptr_t)arg];
  File f = SD.open(t.path);
  HMP3Decoder dec = f ? MP3InitDecoder() : nullptr;
  const size_t INBUF = 6 * 1024, OUTSAMPLES = 1152;
  uint8_t* in = (uint8_t*)heap_caps_malloc(INBUF, MALLOC_CAP_SPIRAM);
  int16_t* pcm = (int16_t*)heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
  int16_t* out = (int16_t*)heap_caps_malloc(4 * OUTSAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  bool ok = f && dec && in && pcm && out;
  if (!ok) Serial.printf("[listen] cannot start %s (file=%d dec=%d bufs=%d)\n", t.path.c_str(), (bool)f, dec != nullptr, in && pcm && out);
  size_t have = 0, startOffset = 0;
  bool eof = false, flushed = false;
  int slot = 0;
  if (ok) {
    startOffset = id3Size(f);
    f.seek(startOffset);
    dataStart = startOffset; totalBytes = f.size(); posBytes = startOffset;
    Serial.printf("[listen] playing %s (%u bytes)\n", t.path.c_str(), (unsigned)totalBytes);
  }
  while (ok && !stopReq) {
    if (paused) {
      if (!flushed) { M5.Speaker.stop(CH); flushed = true; }
      vTaskDelay(pdMS_TO_TICKS(60));
      continue;
    }
    flushed = false;
    if (seekReq) {
      float frac = constrain((float)seekFrac, 0.0f, 0.995f);
      f.seek(dataStart + (size_t)((totalBytes - dataStart) * frac));
      have = 0; eof = false; seekReq = false;
      M5.Speaker.stop(CH);
    }
    if (have < 2048 && !eof) {
      size_t n = f.read(in + have, INBUF - have);
      if (n == 0) eof = true; else have += n;
    }
    posBytes = f.position() - have;
    if (have == 0) break;
    int sync = MP3FindSyncWord(in, (int)have);
    if (sync < 0) { have = 0; if (eof) break; continue; }
    if (sync > 0) { memmove(in, in + sync, have - sync); have -= sync; }
    unsigned char* ptr = in;
    int bytesLeft = (int)have;
    int r = MP3Decode(dec, &ptr, &bytesLeft, pcm, 0);
    size_t used = have - bytesLeft;
    if (used == 0) used = 1;
    memmove(in, in + used, have - used);
    have -= used;
    if (r != ERR_MP3_NONE) { ++decodeErrors; if (eof && have < 4) break; continue; }
    MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
    bitrate = info.bitrate;
    int ch = max(1, info.nChans);
    size_t n = min<size_t>(OUTSAMPLES, info.outputSamps / ch);
    int16_t* o = out + slot * OUTSAMPLES;
    for (size_t i = 0; i < n; ++i)
      o[i] = ch > 1 ? (int16_t)(((int32_t)pcm[i * ch] + pcm[i * ch + 1]) / 2) : pcm[i];
    while (!stopReq && !paused && !seekReq && M5.Speaker.isPlaying(CH) >= 2) vTaskDelay(pdMS_TO_TICKS(3));
    if (stopReq || paused || seekReq) continue;
    M5.Speaker.playRaw(o, n, info.samprate > 0 ? info.samprate : 44100, false, 1, CH, false);
    slot = (slot + 1) & 3;
  }
  bool natural = ok && !stopReq;
  if (natural) {  // let the last frames play out
    for (int i = 0; i < 100 && !stopReq && M5.Speaker.isPlaying(CH); ++i) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (in) free(in);
  if (pcm) free(pcm);
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
  for (int i = 0; i < 100 && playing; ++i) delay(10);
  M5.Speaker.stop(CH);
  playing = false; paused = false; finished = false;
}

bool start(int index) {
  if (index < 0 || index >= (int)tracks.size()) return false;
  stop();
  current = index;
  stopReq = false; paused = false; finished = false; seekReq = false;
  posBytes = 0; totalBytes = tracks[index].size; decodeErrors = 0;
  if (!M5.Speaker.isRunning()) M5.Speaker.begin();
  M5.Speaker.setVolume((uint8_t)(volume * 255 / 100));
  playing = true;
  if (xTaskCreatePinnedToCore(decodeTask, "listen", 10240, (void*)(intptr_t)index, 2, nullptr, 1) != pdPASS) {
    playing = false;
    return false;
  }
  return true;
}

void setVolume(int percent) {
  volume = (uint8_t)constrain(percent, 0, 100);
  M5.Speaker.setVolume((uint8_t)(volume * 255 / 100));
}

}  // namespace listen
