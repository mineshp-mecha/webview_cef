#pragma once

// FPSDBG: temporary frame-rate counters for device measurements.
// Remove before committing.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace fpsdbg {

// Counts events and prints their rate to stderr about once a second while
// they keep coming. Quiet when idle.
class Counter {
 public:
  explicit Counter(const char* name) : name_(name) {}

  void Tick() {
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(mutex_);
    if (start_ms_ == 0 || now - start_ms_ > 2000) {
      // First event, or the first after an idle gap: start a new window.
      start_ms_ = now;
      count_ = 0;
    }
    ++count_;
    const int64_t elapsed = now - start_ms_;
    if (elapsed >= 1000) {
      std::fprintf(stderr, "[FPSDBG] %-18s %5.1f/s\n", name_,
                   count_ * 1000.0 / elapsed);
      start_ms_ = now;
      count_ = 0;
    }
  }

 private:
  static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  const char* name_;
  std::mutex mutex_;
  int64_t start_ms_ = 0;
  int count_ = 0;
};

}  // namespace fpsdbg
