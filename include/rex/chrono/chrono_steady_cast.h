/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>

#include <rex/chrono/chrono.h>

// This is in a separate header because casting to and from steady time points
// usually doesn't make sense and is imprecise. However, NT uses the FileTime
// epoch as a steady clock in waits. In such cases, include this header and use
// rex::chrono::clock_cast<>().

#if REX_HAS_STD_CLOCK_CAST

namespace std::chrono {

// This conveniently works only for Host time domain because Guest needs
// additional scaling. Convert XSystemClock to WinSystemClock first if
// necessary.
template <>
struct clock_time_conversion<::rex::chrono::WinSystemClock, std::chrono::steady_clock> {
  using WinSystemClock_ = ::rex::chrono::WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  typename WinSystemClock_::time_point operator()(
      const std::chrono::time_point<steady_clock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = std::chrono::floor<WinSystemClock_::duration>(t - steady_now);
    return nt_now + delta;
  }
};

template <>
struct clock_time_conversion<std::chrono::steady_clock, ::rex::chrono::WinSystemClock> {
  using WinSystemClock_ = ::rex::chrono::WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  steady_clock_::time_point operator()(
      const std::chrono::time_point<WinSystemClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = t - nt_now;
    return steady_now + delta;
  }
};

}  // namespace std::chrono

#else  // !REX_HAS_STD_CLOCK_CAST — extend the rex::chrono polyfill

namespace rex::chrono {

template <>
struct clock_time_conversion<WinSystemClock, std::chrono::steady_clock> {
  using WinSystemClock_ = WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  typename WinSystemClock_::time_point operator()(
      const std::chrono::time_point<steady_clock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = std::chrono::floor<WinSystemClock_::duration>(t - steady_now);
    return nt_now + delta;
  }
};

template <>
struct clock_time_conversion<std::chrono::steady_clock, WinSystemClock> {
  using WinSystemClock_ = WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  steady_clock_::time_point operator()(
      const std::chrono::time_point<WinSystemClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = t - nt_now;
    return steady_now + delta;
  }
};

}  // namespace rex::chrono

#endif  // REX_HAS_STD_CLOCK_CAST
