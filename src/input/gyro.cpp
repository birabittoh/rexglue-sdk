/**
 * @file        input/gyro.cpp
 * @brief       Gyro aiming shared by every driver that has a gyroscope.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/gyro.h>

#include <algorithm>
#include <cmath>

REXCVAR_DEFINE_BOOL(gyro_aim, false, "Input", "Add the gyroscope to a thumbstick's input");
REXCVAR_DEFINE_BOOL(gyro_left_stick, false, "Input",
                    "Drive the left stick with the gyro aiming instead of the right");
REXCVAR_DEFINE_DOUBLE(gyro_sensitivity, 1.0, "Input", "Gyro aiming sensitivity").range(0.01, 10.0);
REXCVAR_DEFINE_BOOL(gyro_invert_x, false, "Input", "Invert the gyro's horizontal axis");
REXCVAR_DEFINE_BOOL(gyro_invert_y, false, "Input", "Invert the gyro's vertical axis");
REXCVAR_DEFINE_DOUBLE(gyro_deadzone, 0.0, "Input",
                      "Gyro rates below this many rad/s are ignored, for pads that drift")
    .range(0.0, 0.5);

namespace rex::input {

namespace {

// The gyro drives stick deflection from angular velocity.
constexpr float kGyroFullScaleRadPerSec = 4.0f;

int16_t GyroAxisToStick(float rad_per_sec, double sensitivity, double deadzone, bool invert) {
  if (std::abs(rad_per_sec) < deadzone) {
    return 0;
  }
  if (invert) {
    rad_per_sec = -rad_per_sec;
  }
  const double scaled = double(rad_per_sec) * sensitivity * 32767.0 / kGyroFullScaleRadPerSec;
  return static_cast<int16_t>(std::clamp(scaled, -32767.0, 32767.0));
}

// Saturating, so a gyro flick on top of a pushed stick cannot wrap the axis.
int16_t AddStick(int16_t base, int16_t delta) {
  return static_cast<int16_t>(
      std::clamp(int32_t(base) + int32_t(delta), int32_t(-32767), int32_t(32767)));
}

}  // namespace

bool ApplyGyroToGamepad(float yaw_rad_per_sec, float pitch_rad_per_sec, X_INPUT_GAMEPAD* gamepad) {
  if (!REXCVAR_GET(gyro_aim)) {
    return false;
  }
  const double sensitivity = REXCVAR_GET(gyro_sensitivity);
  const double deadzone = REXCVAR_GET(gyro_deadzone);
  const int16_t gyro_x =
      GyroAxisToStick(-yaw_rad_per_sec, sensitivity, deadzone, REXCVAR_GET(gyro_invert_x));
  const int16_t gyro_y =
      GyroAxisToStick(pitch_rad_per_sec, sensitivity, deadzone, REXCVAR_GET(gyro_invert_y));
  if (!gyro_x && !gyro_y) {
    return false;
  }
  const bool left = REXCVAR_GET(gyro_left_stick);
  auto& out_x = left ? gamepad->thumb_lx : gamepad->thumb_rx;
  auto& out_y = left ? gamepad->thumb_ly : gamepad->thumb_ry;
  out_x = AddStick(out_x, gyro_x);
  out_y = AddStick(out_y, gyro_y);
  return true;
}

}  // namespace rex::input
