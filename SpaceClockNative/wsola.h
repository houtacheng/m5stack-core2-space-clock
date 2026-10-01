// Pitch-preserving speed change (WSOLA) for mono 16-bit audio.
//
// Frames of 2*Hs samples are taken from the input every Hs*speed samples
// (nominal), each shifted by up to +-delta samples to the position that best
// continues the previous frame (minimum average magnitude difference on a
// decimated copy), and overlap-added with a sin^2/cos^2 window at a hop of Hs.
// Output length = input length / speed. Speed 1.0 should be passed through by
// the caller without using this class.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(ARDUINO)
#include <esp_heap_caps.h>
static inline void* wsolaAlloc(size_t n) { void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM); return p ? p : malloc(n); }
// The input ring is read heavily by the matcher: keep it in fast internal RAM when possible.
static inline void* wsolaAllocFast(size_t n) {
  // Only use scarce internal RAM when plenty is left (Bluetooth needs ~80 KB).
  void* p = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 90000 ? heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
  return p ? p : wsolaAlloc(n);
}
#else
static inline void* wsolaAlloc(size_t n) { return malloc(n); }
static inline void* wsolaAllocFast(size_t n) { return malloc(n); }
#endif

class Wsola {
 public:
  ~Wsola() { release(); }

  // (Re)configure for a sample rate and speed. Drops any buffered audio.
  bool begin(int sampleRate, float speed) {
    release();
    rate_ = sampleRate;
    hs_ = ((sampleRate * 16 / 1000) & ~3);          // 16 ms hop
    if (hs_ < 64) hs_ = 64;
    n_ = 2 * hs_;
    delta_ = (sampleRate * 8 / 1000) & ~1;          // +-8 ms search
    ring_ = 8192;
    while (ring_ < n_ * 4 + 2 * delta_ + 1024) ring_ <<= 1;
    ringMask_ = ring_ - 1;
    buf_ = (int16_t*)wsolaAllocFast(ring_ * sizeof(int16_t));
    rise_ = (int16_t*)wsolaAlloc(hs_ * sizeof(int16_t));
    tail_ = (int16_t*)wsolaAlloc(hs_ * sizeof(int16_t));
    if (!buf_ || !rise_ || !tail_) { release(); return false; }
    for (int i = 0; i < hs_; ++i) {
      float s = sinf((float)M_PI / 2.0f * (i + 0.5f) / hs_);
      rise_[i] = (int16_t)lrintf(s * s * 32767.0f);
    }
    setSpeed(speed);
    reset();
    return true;
  }

  void setSpeed(float speed) { speed_ = speed < 0.25f ? 0.25f : (speed > 4.0f ? 4.0f : speed); }

  void reset() {
    inEnd_ = 0; nominal_ = 0; prevPos_ = -1; started_ = false;
    if (buf_) memset(buf_, 0, ring_ * sizeof(int16_t));
  }

  int hop() const { return hs_; }
  int rate() const { return rate_; }

  // Append decoded input.
  void push(const int16_t* in, size_t n) {
    for (size_t i = 0; i < n; ++i) buf_[(inEnd_ + i) & ringMask_] = in[i];
    inEnd_ += n;
  }

  // Signal end of input: flush the last frames with silence.
  void finish() {
    static const int16_t zeros[256] = {0};
    int need = n_ + delta_ + hs_;
    while (need > 0) { int c = need > 256 ? 256 : need; push(zeros, c); need -= c; }
  }

  // Produce one hop (hop() samples) if enough input has been pushed.
  bool pull(int16_t* out) {
    int64_t center = (int64_t)llround(nominal_);
    if (!started_) {
      if (inEnd_ < (int64_t)n_) return false;
      for (int i = 0; i < hs_; ++i) out[i] = at(i);
      storeTail(0);
      prevPos_ = 0; started_ = true; nominal_ = (double)hs_ * speed_;
      return true;
    }
    if (inEnd_ < center + delta_ + n_) return false;
    int64_t lo = center - delta_, hi = center + delta_;
    int64_t oldest = inEnd_ - ring_ + 16;
    if (lo < oldest) lo = oldest;
    if (lo < 0) lo = 0;
    if (hi + n_ > inEnd_) hi = inEnd_ - n_;
    if (hi < lo) hi = lo;
    int64_t best = pickBest(lo, hi, center);
    for (int i = 0; i < hs_; ++i) {
      int32_t v = tail_[i] + (((int32_t)at(best + i) * rise_[i]) >> 15);
      out[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
    }
    storeTail(best);
    prevPos_ = best;
    nominal_ += (double)hs_ * speed_;
    return true;
  }

  // Input samples before this absolute position are no longer needed.
  int64_t consumedUpTo() const { return nominal_ > delta_ ? (int64_t)nominal_ - delta_ - hs_ : 0; }
  int64_t inputTotal() const { return inEnd_; }
  // Rough fill level the caller should keep: samples buffered ahead of what is needed.
  int64_t ahead() const { return inEnd_ - ((int64_t)llround(nominal_) + delta_ + n_); }

 private:
  void release() {
    if (buf_) free(buf_); if (rise_) free(rise_); if (tail_) free(tail_);
    buf_ = rise_ = tail_ = nullptr;
  }
  inline int16_t at(int64_t pos) const { return buf_[pos & ringMask_]; }

  // Falling half of the window applied to the second half of the frame at pos.
  void storeTail(int64_t pos) {
    for (int i = 0; i < hs_; ++i) {
      int32_t fall = 32767 - rise_[i];
      tail_[i] = (int16_t)(((int32_t)at(pos + hs_ + i) * fall) >> 15);
    }
  }

  // Candidate whose first half best matches the natural continuation of the
  // previous frame (its second half in the input). Coarse search on a sparse
  // grid with sparse sampling, then a fine pass around the winner.
  int64_t pickBest(int64_t lo, int64_t hi, int64_t center) {
    int64_t refPos = prevPos_ + hs_;
    int64_t bestPos = center < lo ? lo : (center > hi ? hi : center);
    uint32_t bestCost = 0xFFFFFFFFu;
    auto cost = [&](int64_t p, int dec) {
      uint32_t c = 0;
      for (int i = 0; i < hs_; i += dec) {
        int32_t d = (int32_t)at(p + i) - (int32_t)at(refPos + i);
        c += d < 0 ? -d : d;
      }
      // Prefer positions near the nominal one (keeps timing and breaks ties).
      int64_t off = p - center; if (off < 0) off = -off;
      return c * (8 / dec) + (uint32_t)(off * 2);
    };
    for (int64_t p = lo; p <= hi; p += 4) {
      uint32_t c = cost(p, 8);
      if (c < bestCost) { bestCost = c; bestPos = p; }
    }
    int64_t coarse = bestPos;
    bestCost = 0xFFFFFFFFu;
    for (int64_t p = coarse - 3; p <= coarse + 3; ++p) {
      if (p < lo || p > hi) continue;
      uint32_t c = cost(p, 2);
      if (c < bestCost) { bestCost = c; bestPos = p; }
    }
    return bestPos;
  }

  int rate_ = 0, hs_ = 0, n_ = 0, delta_ = 0;
  size_t ring_ = 0, ringMask_ = 0;
  int16_t *buf_ = nullptr, *rise_ = nullptr, *tail_ = nullptr;
  int64_t inEnd_ = 0, prevPos_ = -1;
  double nominal_ = 0;
  float speed_ = 1.0f;
  bool started_ = false;
};
