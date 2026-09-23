/**
 * @file        rex/input/gyro.h
 * @brief       Gyro aiming shared by every driver that has a gyroscope.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/input.h>

REXCVAR_DECLARE(bool, gyro_aim);
REXCVAR_DECLARE(bool, gyro_left_stick);
REXCVAR_DECLARE(double, gyro_sensitivity);
REXCVAR_DECLARE(bool, gyro_invert_x);
REXCVAR_DECLARE(bool, gyro_invert_y);
REXCVAR_DECLARE(double, gyro_deadzone);

namespace rex::input {

/// Adds gyro aiming to the stick the gyro_* cvars select. Rates are rad/s,
/// yaw positive turning left and pitch positive tilting up, as SDL reports a
/// pad held in front of you. Returns true if the stick was moved.
bool ApplyGyroToGamepad(float yaw_rad_per_sec, float pitch_rad_per_sec, X_INPUT_GAMEPAD* gamepad);

}  // namespace rex::input
