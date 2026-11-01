#pragma once
// Serial that also keeps the last ~24 KB of output in a ring buffer, so the log can be read over the network
// (see the /adb routes and tools/clockctl.py). Everything written to Serial goes to the UART as before.
#include <Arduino.h>

class AdbSerialClass : public Stream {
 public:
  static const size_t CAP = 24576;
  void begin(unsigned long baud) {
    ::Serial.begin(baud);
    if (!buf) buf = (char*)ps_malloc(CAP);
  }
  int available() override { return ::Serial.available(); }
  int read() override { return ::Serial.read(); }
  int peek() override { return ::Serial.peek(); }
  void flush() override { ::Serial.flush(); }
  size_t write(uint8_t c) override { ::Serial.write(c); tee(&c, 1); return 1; }
  size_t write(const uint8_t* data, size_t n) override { ::Serial.write(data, n); tee(data, n); return n; }

  // Total number of bytes ever written (a position in the log).
  uint32_t position() const { return total; }

  // Copy log bytes from position `from` (clamped to what is still in the buffer) into out; returns the length
  // and sets `next` to the position to ask for next time.
  size_t copyFrom(uint32_t from, char* out, size_t maxLen, uint32_t& next) {
    portENTER_CRITICAL(&mux);
    uint32_t end = total;
    uint32_t start = end > CAP ? end - CAP : 0;
    if (from < start) from = start;
    if (from > end) from = end;
    size_t n = end - from;
    if (n > maxLen) n = maxLen;
    for (size_t i = 0; i < n; ++i) out[i] = buf ? buf[(from + i) % CAP] : '?';
    next = from + n;
    portEXIT_CRITICAL(&mux);
    return n;
  }

 private:
  void tee(const uint8_t* data, size_t n) {
    if (!buf) return;
    portENTER_CRITICAL(&mux);
    for (size_t i = 0; i < n; ++i) buf[(total + i) % CAP] = (char)data[i];
    total += n;
    portEXIT_CRITICAL(&mux);
  }
  char* buf = nullptr;
  volatile uint32_t total = 0;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};

extern AdbSerialClass AdbSerial;
#define Serial AdbSerial
