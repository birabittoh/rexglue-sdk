/**
 * @file        rex/ui/ui_text.h
 * @brief       Lets a host application translate the SDK's overlay text.
 *
 * Overlay code asks for each string by key with its English text; with no
 * provider, or one that returns null, the English text is used.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>

namespace rex::ui {

// Returns the text for `key` in the current language, or null to use English.
// The pointer must stay valid until the next call.
using UiTextProvider = const char* (*)(const char* key);

void SetUiTextProvider(UiTextProvider provider);

// `english` when no provider is set or it has no text for `key`.
const char* UiText(const char* key, const char* english);

// UiText(key, english) + "###" + id: the id keeps an ImGui window or tab
// identifying the same across languages, so lookups by window name still work.
std::string UiLabel(const char* key, const char* english, const char* id);

// printf-style formatting of UiText(key, english).
std::string UiTextFormat(const char* key, const char* english, ...);

}  // namespace rex::ui
