/**
 * @file        rex/system/util/presence_string_builder.cpp
 * @brief       See rex/system/util/presence_string_builder.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/util/presence_string_builder.h>

#include <cstdlib>
#include <utility>

namespace rex::system::util {

PresenceStringBuilder::PresenceStringBuilder(std::string_view template_string, const XLast* xlast,
                                             std::map<uint32_t, uint32_t> contexts,
                                             XLanguage language)
    : xlast_(xlast), contexts_(std::move(contexts)), language_(language) {
  presence_string_.reserve(template_string.size());
  for (size_t i = 0; i < template_string.size();) {
    if (template_string[i] != '{') {
      presence_string_.push_back(template_string[i]);
      ++i;
      continue;
    }
    const size_t close = template_string.find('}', i);
    if (close == std::string_view::npos) {
      presence_string_.append(template_string.substr(i));
      break;
    }
    presence_string_ += ResolveSpecifier(template_string.substr(i, close - i + 1));
    i = close + 1;
  }
}

std::string PresenceStringBuilder::ResolveSpecifier(std::string_view specifier) const {
  // Specifiers are {cN} for contexts (or {pN} for properties), N decimal.
  if (specifier.size() < 4 || specifier.front() != '{' || specifier.back() != '}') {
    return std::string(specifier);
  }
  const char type = specifier[1];
  const std::string digits(specifier.substr(2, specifier.size() - 3));
  if (type != 'c' || digits.empty()) {
    return std::string(specifier);
  }
  const uint32_t context_id = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 10));
  return ResolveContext(context_id);
}

std::string PresenceStringBuilder::ResolveContext(uint32_t context_id) const {
  if (!xlast_) {
    return "";
  }
  auto it = contexts_.find(context_id);
  if (it == contexts_.end()) {
    return "";
  }
  const auto string_id = xlast_->GetContextStringId(context_id, it->second);
  if (!string_id) {
    return "";
  }
  return xlast_->GetLocalizedString(*string_id, language_);
}

}  // namespace rex::system::util
