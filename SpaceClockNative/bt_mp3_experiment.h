// Experiment: play MP3 files from the SD card to Bluetooth headphones (A2DP).
// Only compiled with -DBT_MP3_EXPERIMENT. Controlled over HTTP (see handleHttp):
//   /bt?cmd=sd            list MP3 files on the SD card
//   /bt?cmd=scan          discover nearby Bluetooth devices (log only)
//   /bt?cmd=connect&name= connect to the first device whose name contains `name`
//   /bt?cmd=play&file=    decode and stream an MP3 file
//   /bt?cmd=stop          stop playing      /bt?cmd=off  switch Bluetooth off
//   /bt?cmd=vol&v=0..127  volume            /bt?cmd=status  state + log
#pragma once
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <freertos/stream_buffer.h>
#include <mp3dec.h>
#include "BluetoothA2DPSource.h"

namespace btmp3 {

BluetoothA2DPSource a2dp;
String logText;
String wantName;
bool btStarted = false;
bool sdMounted = false;
volatile bool stopPlayback = false;
volatile bool playing = false;
volatile uint32_t underruns = 0, callbacks = 0, decodedFrames = 0, outFramesTotal = 0;
volatile int curRate = 0, curChans = 0, curBitrate = 0;
uint32_t minInternalHeap = 0xFFFFFFFF;
StreamBufferHandle_t sb = nullptr;
StaticStreamBuffer_t sbStruct;
uint8_t* sbStorage = nullptr;
const size_t SB_SIZE = 64 * 1024;
String playingFile;

void lg(const char* fmt, ...) {
  char line[160];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
  logText += String(millis() / 1000.0f, 1) + "s " + line + "\n";
  if (logText.length() > 3500) logText.remove(0, logText.length() - 3500);
  Serial.printf("[bt] %s\n", line);
}

bool mountSd() {
  if (sdMounted) return true;
  SPI.begin(18, 38, 23, -1);
  sdMounted = SD.begin(4, SPI, 25000000);
  lg("SD mount %s", sdMounted ? "ok" : "FAILED");
  return sdMounted;
}

static bool containsNoCase(const char* hay, const String& needle) {
  String h = hay; h.toLowerCase();
  String n = needle; n.toLowerCase();
  return n.length() && h.indexOf(n) >= 0;
}

bool onDiscovered(const char* name, esp_bd_addr_t, int rssi) {
  lg("found \"%s\" rssi %d", name, rssi);
  if (wantName.length() && containsNoCase(name, wantName)) { lg("connecting to \"%s\"", name); return true; }
  return false;
}

int32_t dataCb(Frame* frames, int32_t count) {
  ++callbacks;
  size_t got = sb ? xStreamBufferReceive(sb, frames, (size_t)count * 4, 0) : 0;
  size_t gotFrames = got / 4;
  if ((int32_t)gotFrames < count) {
    memset(frames + gotFrames, 0, (count - gotFrames) * 4);
    if (playing) ++underruns;
  }
  return count;
}

void startBluetooth(const String& name) {
  if (btStarted) { a2dp.end(true); btStarted = false; delay(300); }
  wantName = name;
  a2dp.set_ssp_enabled(true);          // AirPods require Secure Simple Pairing
  a2dp.set_auto_reconnect(false);
  a2dp.set_ssid_callback(onDiscovered);
  a2dp.set_data_callback_in_frames(dataCb);
  a2dp.set_local_name("SpaceClock");
  if (!sb) {
    sbStorage = (uint8_t*)heap_caps_malloc(SB_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (sbStorage) sb = xStreamBufferCreateStatic(SB_SIZE, 1, sbStorage, &sbStruct);
  }
  a2dp.start();
  btStarted = true;
  lg("bluetooth started, name filter \"%s\", free internal %u", name.c_str(),
     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// Skip an ID3v2 tag at the start of the file. Returns the number of bytes to skip.
static size_t id3Size(File& f) {
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) { f.seek(0); return 0; }
  size_t size = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
  return size + 10;
}

static bool pushFrames(const int16_t* stereo, size_t frames) {
  const uint8_t* p = (const uint8_t*)stereo;
  size_t left = frames * 4;
  while (left && !stopPlayback) {
    size_t sent = xStreamBufferSend(sb, p, left, pdMS_TO_TICKS(200));
    p += sent; left -= sent;
  }
  return !stopPlayback;
}

void decodeTask(void*) {
  String path = playingFile;
  File f = SD.open(path);
  if (!f) { lg("cannot open %s", path.c_str()); playing = false; vTaskDelete(nullptr); return; }
  HMP3Decoder dec = MP3InitDecoder();
  if (!dec) { lg("decoder init failed"); f.close(); playing = false; vTaskDelete(nullptr); return; }
  const size_t INBUF = 6 * 1024;
  uint8_t* in = (uint8_t*)heap_caps_malloc(INBUF, MALLOC_CAP_8BIT);
  int16_t* pcm = (int16_t*)heap_caps_malloc(1152 * 2 * 2, MALLOC_CAP_8BIT);
  int16_t* out = (int16_t*)heap_caps_malloc(1400 * 4 * 2, MALLOC_CAP_8BIT);
  if (!in || !pcm || !out) { lg("decode buffers failed"); goto done; }
  {
    size_t skip = id3Size(f);
    f.seek(skip);
    size_t have = 0;
    bool eof = false;
    double pos = -1.0; int16_t prevL = 0, prevR = 0;
    lg("playing %s (id3 %u bytes)", path.c_str(), (unsigned)skip);
    while (!stopPlayback) {
      if (have < 2048 && !eof) {
        size_t n = f.read(in + have, INBUF - have);
        if (n == 0) eof = true; else have += n;
      }
      if (have == 0) break;
      int sync = MP3FindSyncWord(in, have);
      if (sync < 0) { have = 0; if (eof) break; continue; }
      if (sync > 0) { memmove(in, in + sync, have - sync); have -= sync; }
      unsigned char* ptr = in;
      int bytesLeft = (int)have;
      int r = MP3Decode(dec, &ptr, &bytesLeft, pcm, 0);
      size_t used = have - bytesLeft;
      if (used == 0) used = 1;
      memmove(in, in + used, have - used);
      have -= used;
      if (r != ERR_MP3_NONE) { if (eof && have < 4) break; continue; }
      MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
      curRate = info.samprate; curChans = info.nChans; curBitrate = info.bitrate;
      int ch = max(1, info.nChans);
      size_t n = info.outputSamps / ch;
      ++decodedFrames;
      // To stereo 16-bit.
      static int16_t stereo[1152 * 2];
      for (size_t i = 0; i < n; ++i) {
        int16_t l = pcm[i * ch], rr = ch > 1 ? pcm[i * ch + 1] : l;
        stereo[i * 2] = l; stereo[i * 2 + 1] = rr;
      }
      if (info.samprate == 44100 || info.samprate <= 0) {
        if (!pushFrames(stereo, n)) break;
        outFramesTotal += n;
      } else {
        // Linear resampling to 44.1 kHz.
        double step = (double)info.samprate / 44100.0;
        size_t outN = 0;
        for (;;) {
          int idx = (int)floor(pos);
          if (idx + 1 >= (int)n) break;
          double frac = pos - idx;
          int16_t l0 = idx < 0 ? prevL : stereo[idx * 2], r0 = idx < 0 ? prevR : stereo[idx * 2 + 1];
          int16_t l1 = stereo[(idx + 1) * 2], r1 = stereo[(idx + 1) * 2 + 1];
          out[outN * 2] = (int16_t)(l0 + (l1 - l0) * frac);
          out[outN * 2 + 1] = (int16_t)(r0 + (r1 - r0) * frac);
          ++outN; pos += step;
          if (outN >= 1400) break;
        }
        pos -= n; prevL = stereo[(n - 1) * 2]; prevR = stereo[(n - 1) * 2 + 1];
        if (!pushFrames(out, outN)) break;
        outFramesTotal += outN;
      }
      uint32_t heapNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      if (heapNow < minInternalHeap) minInternalHeap = heapNow;
    }
  }
done:
  if (in) free(in);
  if (pcm) free(pcm);
  if (out) free(out);
  MP3FreeDecoder(dec);
  f.close();
  lg("playback ended (%s), frames %u, underruns %u", stopPlayback ? "stopped" : "finished",
     (unsigned)decodedFrames, (unsigned)underruns);
  playing = false;
  vTaskDelete(nullptr);
}

void play(const String& path) {
  stopPlayback = true;
  for (int i = 0; i < 50 && playing; ++i) delay(20);
  if (!mountSd()) return;
  if (!sb) { lg("start Bluetooth first"); return; }
  xStreamBufferReset(sb);
  stopPlayback = false; underruns = 0; callbacks = 0; decodedFrames = 0; outFramesTotal = 0;
  minInternalHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  playingFile = path; playing = true;
  xTaskCreatePinnedToCore(decodeTask, "mp3dec", 12288, nullptr, 2, nullptr, 1);
}

static void listDir(String& out, const String& dir, int depth) {
  File d = SD.open(dir);
  if (!d) return;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    String name = e.name();
    String full = dir == "/" ? "/" + name : dir + "/" + name;
    if (e.isDirectory()) { if (depth > 0) listDir(out, full, depth - 1); }
    else {
      String l = name; l.toLowerCase();
      if (l.endsWith(".mp3")) out += full + "  " + String((unsigned)e.size()) + "\n";
    }
  }
}

static void startTask(void* arg) {
  String* name = (String*)arg;
  lg("start task: free internal %u largest %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  startBluetooth(*name);
  lg("start task done: free internal %u largest %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  delete name;
  vTaskDelete(nullptr);
}

// Bluetooth start can block for a long time: run it away from the web/loop task.
void startBluetoothAsync(const String& name) {
  xTaskCreatePinnedToCore(startTask, "btstart", 8192, new String(name), 3, nullptr, 0);
}

String runCommand(const String& cmd, const String& arg) {
  String body;
  if (cmd == "sd") {
    if (mountSd()) {
      body = "SD total " + String((unsigned)(SD.totalBytes() / 1048576)) + " MB, used " + String((unsigned)(SD.usedBytes() / 1048576)) + " MB\n";
      listDir(body, "/", 2);
    } else body = "SD mount failed\n";
  } else if (cmd == "wifioff") {
    wifiPaused = true; WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); delay(300);
    body = "wifi off\n";
  } else if (cmd == "wifion") {
    WiFi.mode(WIFI_STA); WiFi.begin(); wifiPaused = false;
    body = "wifi on\n";
  } else if (cmd == "scan") {
    startBluetoothAsync("");
    body = "scanning; read status for found devices\n";
  } else if (cmd == "connect") {
    startBluetoothAsync(arg);
    body = "connecting to \"" + arg + "\"\n";
  } else if (cmd == "play") {
    play(arg);
    body = "play requested\n";
  } else if (cmd == "stop") {
    stopPlayback = true; body = "stopped\n";
  } else if (cmd == "vol") {
    a2dp.set_volume(constrain(arg.toInt(), 0, 127)); body = "volume set\n";
  } else if (cmd == "off") {
    stopPlayback = true; delay(200);
    if (btStarted) { a2dp.end(true); btStarted = false; }
    body = "bluetooth off\n";
  }
  body += "--- status ---\n";
  body += String("bt=") + (btStarted ? 1 : 0) + " connected=" + (btStarted && a2dp.is_connected() ? 1 : 0) +
          " playing=" + (playing ? 1 : 0) + " file=" + playingFile + "\n";
  body += "rate=" + String(curRate) + " ch=" + String(curChans) + " kbps=" + String(curBitrate / 1000) +
          " decoded=" + String((unsigned)decodedFrames) + " cb=" + String((unsigned)callbacks) +
          " underruns=" + String((unsigned)underruns) + "\n";
  body += "heap internal free=" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
          " largest=" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) +
          " minDuringPlay=" + String((unsigned)(minInternalHeap == 0xFFFFFFFF ? 0 : minInternalHeap)) +
          " psram=" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + "\n";
  body += "--- log ---\n" + logText;
  return body;
}

void handleHttp(WebServer& s) {
  String arg = s.hasArg("name") ? s.arg("name") : (s.hasArg("file") ? s.arg("file") : s.arg("v"));
  s.send(200, "text/plain; charset=utf-8", runCommand(s.arg("cmd"), arg));
}

}  // namespace btmp3
