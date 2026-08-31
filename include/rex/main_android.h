/**
 * @file        rex/main_android.h
 * @brief       Android-specific platform utilities.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <android/api-level.h>

namespace rex {

/// Returns the device's runtime API level (e.g. 28 for Android 9).
inline int GetAndroidApiLevel() {
  return android_get_device_api_level();
}

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
