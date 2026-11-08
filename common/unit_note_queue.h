// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/**
 * Note events from the callback thread to the render thread.
 *
 *   rfx::UnitNoteQueue<64> queue_;
 *
 *   // in the note callbacks
 *   queue_.push(rfx::kUnitNoteOn, note, velocity);
 *
 *   // at the top of Render()
 *   rfx::UnitNoteMessage msg;
 *   while (queue_.pop(msg)) { ... }
 *
 * Single producer, single consumer. Nothing allocates, locks or throws.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rfx {

enum UnitNoteEventType : uint8_t {
  kUnitNoteOn = 0,
  kUnitNoteOff,
  kUnitGateOn,
  kUnitGateOff,
  kUnitAllNoteOff
};

struct UnitNoteMessage {
  UnitNoteEventType type;
  uint8_t           note;
  uint8_t           velocity;
};

/*
 * One slot is kept free, so full and empty differ. When full, the new event is
 * dropped; the ones waiting are older.
 */
template <unsigned Size>
class UnitNoteQueue {
  static_assert(Size >= 2 && Size <= 128,
                "one slot is kept free, and the index is a uint8_t");

 public:
  UnitNoteQueue() : write_(0), read_(0) {}

  void push(UnitNoteEventType type, uint8_t note = 0, uint8_t velocity = 0) {
    const uint8_t w    = write_.load(std::memory_order_relaxed);
    const uint8_t next = (uint8_t)((w + 1) % Size);

    if (next == read_.load(std::memory_order_acquire))
      return;

    messages_[w].type     = type;
    messages_[w].note     = note;
    messages_[w].velocity = velocity;

    /* Written before the index that publishes it. */
    write_.store(next, std::memory_order_release);
  }

  /* The next event, oldest first; false when there is none. */
  bool pop(UnitNoteMessage & out) {
    const uint8_t r = read_.load(std::memory_order_relaxed);

    if (r == write_.load(std::memory_order_acquire))
      return false;

    out = messages_[r];

    read_.store((uint8_t)((r + 1) % Size), std::memory_order_release);
    return true;
  }

  /* The next event without taking it, so the rest stays in order for the next
   * block. */
  bool peek(UnitNoteMessage & out) const {
    const uint8_t r = read_.load(std::memory_order_relaxed);

    if (r == write_.load(std::memory_order_acquire))
      return false;

    out = messages_[r];
    return true;
  }

  /* Drop what is waiting. A reset uses this: those events belong to the
   * pattern that was just stopped. */
  void clear() {
    read_.store(write_.load(std::memory_order_acquire),
                std::memory_order_release);
  }

  bool empty() const {
    return read_.load(std::memory_order_relaxed) ==
           write_.load(std::memory_order_acquire);
  }

 private:
  UnitNoteMessage      messages_[Size];
  std::atomic<uint8_t> write_;
  std::atomic<uint8_t> read_;
};

}  // namespace rfx
