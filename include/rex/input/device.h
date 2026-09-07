#pragma once
/**
 * @file        rex/input/device.h
 * @brief       Input device identity shared by drivers and device assignment.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <string>

namespace rex::input {

constexpr uint32_t kMaxGuestUsers = 4;
constexpr uint32_t kGuestUserUnassigned = UINT32_MAX;

/// Driver-scoped device handle. Never reused within a process run, so a handle
/// that outlives its device resolves to nothing rather than aliasing whichever
/// device took the freed slot.
enum class DeviceId : uint64_t { kInvalid = 0 };

enum class DeviceKind : uint8_t {
  kController,
  kKeyboardMouse,
  kTouch,
  kPlaceholder,
};

struct DeviceInfo {
  DeviceId id = DeviceId::kInvalid;
  uint32_t ordinal = 0;  // connection order, assigned by InputSystem
  std::string name;
  std::string guid;
  bool synthetic = false;  // keyboard/mouse emulation or the NOP stand-in
  DeviceKind kind = DeviceKind::kController;
};

}  // namespace rex::input
