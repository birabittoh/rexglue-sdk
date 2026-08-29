/**
 * @file        rex/thread/jthread_polyfill.h
 * @brief       Minimal std::jthread / std::stop_token polyfill for platforms
 *              where the standard library does not yet provide them (e.g.
 *              Android NDK r27 libc++).
 *
 * When the standard library has jthread, this header simply imports the
 * std:: names into rex::thread::compat. When it doesn't, a lightweight
 * replacement is provided that covers the subset used by timer_queue.cpp:
 *   - stop_source / stop_token / stop_requested()
 *   - jthread constructor taking (callable, stop_token)
 *   - request_stop(), get_id(), auto-join on destruction
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <version>
#include <thread>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
// Standard library provides jthread — alias into our compat namespace.
#define REX_HAS_STD_JTHREAD 1
#include <stop_token>
namespace rex::thread::compat {
using std::jthread;
using std::stop_source;
using std::stop_token;
}  // namespace rex::thread::compat

#else
// Polyfill.
#define REX_HAS_STD_JTHREAD 0
#include <atomic>
#include <functional>
#include <utility>

namespace rex::thread::compat {

/// Minimal stop_source / stop_token pair.
class stop_source {
 public:
  stop_source() : state_(std::make_shared<std::atomic<bool>>(false)) {}
  bool stop_requested() const noexcept { return state_->load(std::memory_order_acquire); }
  bool request_stop() noexcept {
    bool expected = false;
    return state_->compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }
  class token {
   public:
    token() = default;
    explicit token(std::shared_ptr<std::atomic<bool>> s) : state_(std::move(s)) {}
    bool stop_requested() const noexcept {
      return state_ && state_->load(std::memory_order_acquire);
    }

   private:
    std::shared_ptr<std::atomic<bool>> state_;
  };
  token get_token() const noexcept { return token{state_}; }

 private:
  std::shared_ptr<std::atomic<bool>> state_;
};

using stop_token = stop_source::token;

/// Minimal jthread: auto-joins on destruction, provides stop_token to the
/// callable, and supports request_stop().
class jthread {
 public:
  using id = std::thread::id;

  jthread() noexcept = default;

  template <typename F>
  explicit jthread(F&& f) : source_() {
    thread_ = std::thread(std::forward<F>(f), source_.get_token());
  }

  ~jthread() {
    if (thread_.joinable()) {
      source_.request_stop();
      thread_.join();
    }
  }

  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;

  jthread(jthread&& other) noexcept
      : thread_(std::move(other.thread_)), source_(std::move(other.source_)) {}

  jthread& operator=(jthread&& other) noexcept {
    if (thread_.joinable()) {
      source_.request_stop();
      thread_.join();
    }
    thread_ = std::move(other.thread_);
    source_ = std::move(other.source_);
    return *this;
  }

  bool request_stop() noexcept { return source_.request_stop(); }
  id get_id() const noexcept { return thread_.get_id(); }
  bool joinable() const noexcept { return thread_.joinable(); }

 private:
  std::thread thread_;
  stop_source source_;
};

}  // namespace rex::thread::compat

#endif  // __cpp_lib_jthread
