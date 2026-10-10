/**
 * @file        ui/ui_text.cpp
 * @brief       UI text translation hook. See rex/ui/ui_text.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/ui_text.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace rex::ui {

namespace {
std::atomic<UiTextProvider> g_provider{nullptr};
}

void SetUiTextProvider(UiTextProvider provider) {
  g_provider.store(provider);
}

const char* UiText(const char* key, const char* english) {
  if (UiTextProvider provider = g_provider.load()) {
    if (const char* text = provider(key)) {
      return text;
    }
  }
  return english;
}

std::string UiLabel(const char* key, const char* english, const char* id) {
  return std::string(UiText(key, english)) + "###" + id;
}

std::string UiTextFormat(const char* key, const char* english, ...) {
  const char* format = UiText(key, english);
  va_list args;
  va_start(args, english);
  va_list copy;
  va_copy(copy, args);
  const int length = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  std::string out;
  if (length > 0) {
    std::vector<char> buffer(static_cast<size_t>(length) + 1);
    vsnprintf(buffer.data(), buffer.size(), format, args);
    out.assign(buffer.data(), static_cast<size_t>(length));
  }
  va_end(args);
  return out;
}

}  // namespace rex::ui
