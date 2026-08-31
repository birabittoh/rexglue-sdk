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

#include <algorithm>
#include <atomic>
#include <limits>
#include <ratio>

#include <rex/chrono/chrono.h>

// This is in a separate header because casting to and from steady time points
// usually doesn't make sense and is imprecise. However, NT uses the FileTime
// epoch as a steady clock in waits. In such cases, include this header and use
// rex::chrono::clock_cast<>().

namespace rex::chrono::detail {

// WinSystemClock counts 100ns ticks while steady_clock counts nanoseconds, so
// converting a delta between the two multiplies by 100. A FILETIME instant far
// from now (a guest passing 0, the 1601 epoch, is the common case) produces a
// delta whose nanosecond form does not fit in the int64 that
// steady_clock::duration is: the multiply overflows, and a due time in the
// distant past silently becomes one centuries in the future. Saturate instead,
// so an out of range instant stays clamped at the correct end of the range.
template <typename SteadyDuration, typename WinDuration>
constexpr SteadyDuration SaturatingToSteady(WinDuration delta) noexcept {
  using Rep = typename SteadyDuration::rep;
  // How many steady units one Win tick becomes: 100, for 100ns ticks into
  // nanoseconds. That is the factor the count gets multiplied by, and so the
  // factor by which the representable range shrinks.
  using Factor = std::ratio_divide<typename WinDuration::period, typename SteadyDuration::period>;
  constexpr Rep kMaxTicks = std::numeric_limits<Rep>::max() / static_cast<Rep>(Factor::num) *
                            static_cast<Rep>(Factor::den);
  const auto ticks = delta.count();
  if (ticks > kMaxTicks) {
    return SteadyDuration::max();
  }
  if (ticks < -kMaxTicks) {
    return SteadyDuration::min();
  }
  return std::chrono::duration_cast<SteadyDuration>(delta);
}

}  // namespace rex::chrono::detail

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

    return steady_now +
           ::rex::chrono::detail::SaturatingToSteady<steady_clock_::duration>(t - nt_now);
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

    return steady_now + detail::SaturatingToSteady<steady_clock_::duration>(t - nt_now);
  }
};

}  // namespace rex::chrono

#endif  // REX_HAS_STD_CLOCK_CAST
