/**
 * @file        rex/system/util/xlast.cpp
 * @brief       XLast parser implementation. See rex/system/util/xlast.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/util/xlast.h>

#include <cstdlib>
#include <map>
#include <string_view>

#include <rex/assert.h>

namespace rex::system::util {

namespace {

// Decodes the XLast XML bytes to UTF-8. The XLast compiler emits UTF-16 (with
// or without a BOM); plain UTF-8 is passed through.
std::string DecodeToUtf8(const std::vector<uint8_t>& bytes) {
  if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
    return std::string(reinterpret_cast<const char*>(bytes.data() + 3), bytes.size() - 3);
  }

  bool big_endian = false;
  size_t start = 0;
  bool utf16 = false;

  if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    utf16 = true;
    start = 2;
  } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
    utf16 = true;
    big_endian = true;
    start = 2;
  } else if (bytes.size() >= 2 && bytes[1] == 0x00 && bytes[0] != 0x00) {
    utf16 = true;
  } else if (bytes.size() >= 2 && bytes[0] == 0x00 && bytes[1] != 0x00) {
    utf16 = true;
    big_endian = true;
  }

  if (!utf16) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  auto read_unit = [&](size_t i) -> uint16_t {
    return big_endian ? static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1])
                      : static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
  };

  std::string out;
  out.reserve((bytes.size() - start) / 2);
  for (size_t i = start; i + 1 < bytes.size(); i += 2) {
    const uint32_t unit = read_unit(i);
    if (unit == 0) {
      break;
    }
    if (unit < 0x80) {
      out.push_back(static_cast<char>(unit));
    } else if (unit < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
      out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    } else if ((unit & 0xFC00) == 0xD800) {
      const uint32_t low = (i + 3 < bytes.size()) ? read_unit(i + 2) : 0;
      if ((low & 0xFC00) == 0xDC00) {
        const uint32_t cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        i += 2;
        continue;
      }
      out.push_back('?');
    } else {
      out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
      out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    }
  }
  return out;
}

constexpr const char* kLocaleEnglish = "en-US";

const char* LocaleForLanguage(XLanguage language) {
  switch (language) {
    case XLanguage::kEnglish:
      return "en-US";
    case XLanguage::kJapanese:
      return "ja-JP";
    case XLanguage::kGerman:
      return "de-DE";
    case XLanguage::kFrench:
      return "fr-FR";
    case XLanguage::kSpanish:
      return "es-ES";
    case XLanguage::kItalian:
      return "it-IT";
    case XLanguage::kKorean:
      return "ko-KR";
    case XLanguage::kTChinese:
      return "zh-CHT";
    case XLanguage::kPortuguese:
      return "pt-PT";
    case XLanguage::kPolish:
      return "pl-PL";
    case XLanguage::kRussian:
      return "ru-RU";
    default:
      return kLocaleEnglish;
  }
}

std::optional<uint32_t> ParseU32(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  const char* begin = text.c_str();
  char* end = nullptr;
  unsigned long value;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    value = std::strtoul(begin + 2, &end, 16);
  } else {
    value = std::strtoul(begin, &end, 10);
  }
  if (end == begin) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(value);
}

size_t FindTag(const std::string& s, size_t from, std::string_view name) {
  const std::string pattern = "<" + std::string(name);
  size_t p = s.find(pattern, from);
  while (p != std::string::npos) {
    const char next = (p + pattern.size() < s.size()) ? s[p + pattern.size()] : '\0';
    if (next == '>' || next == '/' || next == ' ' || next == '\t' || next == '\r' || next == '\n') {
      return p;
    }
    p = s.find(pattern, p + 1);
  }
  return std::string::npos;
}

size_t TagEnd(const std::string& s, size_t tag_start) {
  char quote = 0;
  for (size_t i = tag_start; i < s.size(); ++i) {
    const char c = s[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i;
    }
  }
  return std::string::npos;
}

bool IsSelfClosing(const std::string& s, size_t tag_start, size_t tag_end) {
  size_t i = tag_end;
  while (i > tag_start &&
         (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r' || s[i - 1] == '\n')) {
    --i;
  }
  return i > tag_start && s[i - 1] == '/';
}

std::map<std::string, std::string> ParseAttributes(const std::string& s, size_t tag_start,
                                                   size_t tag_end) {
  std::map<std::string, std::string> attrs;
  size_t i = tag_start + 1;
  while (i < tag_end && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n' &&
         s[i] != '/' && s[i] != '>') {
    ++i;
  }
  while (i < tag_end) {
    while (i < tag_end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
      ++i;
    }
    if (i >= tag_end || s[i] == '/') {
      break;
    }
    const size_t name_start = i;
    while (i < tag_end && s[i] != '=' && s[i] != ' ' && s[i] != '\t') {
      ++i;
    }
    const std::string name = s.substr(name_start, i - name_start);
    while (i < tag_end && (s[i] == ' ' || s[i] == '\t')) {
      ++i;
    }
    if (i < tag_end && s[i] == '=') {
      ++i;
      while (i < tag_end && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
      }
      if (i < tag_end && (s[i] == '"' || s[i] == '\'')) {
        const char q = s[i++];
        const size_t value_start = i;
        while (i < tag_end && s[i] != q) {
          ++i;
        }
        attrs[name] = s.substr(value_start, i - value_start);
      }
    }
  }
  return attrs;
}

struct Element {
  std::map<std::string, std::string> attrs;
  size_t content_start = 0;
  size_t content_end = 0;
};

enum class AttrMatch { kString, kU32 };

// Finds the first element `name` within [from, until) whose attribute `attr`
// matches `value`. The attribute check is skipped when `attr` is empty.
bool FindElement(const std::string& s, size_t from, size_t until, std::string_view name,
                 const std::string& attr, const std::string& value, AttrMatch match, Element& out) {
  size_t p = from;
  while (true) {
    p = FindTag(s, p, name);
    if (p == std::string::npos || p >= until) {
      return false;
    }
    const size_t tag_end = TagEnd(s, p);
    if (tag_end == std::string::npos) {
      return false;
    }
    auto attrs = ParseAttributes(s, p, tag_end);
    bool matches = attr.empty();
    if (!attr.empty()) {
      auto it = attrs.find(attr);
      if (it != attrs.end()) {
        if (match == AttrMatch::kU32) {
          const auto expected = ParseU32(value);
          const auto actual = ParseU32(it->second);
          matches = expected && actual && *expected == *actual;
        } else {
          matches = (it->second == value);
        }
      }
    }
    if (matches) {
      out.attrs = std::move(attrs);
      if (IsSelfClosing(s, p, tag_end)) {
        out.content_start = tag_end + 1;
        out.content_end = tag_end + 1;
      } else {
        const std::string close_tag = "</" + std::string(name) + ">";
        const size_t close = s.find(close_tag, tag_end + 1);
        out.content_start = tag_end + 1;
        out.content_end = (close == std::string::npos) ? s.size() : close;
      }
      return true;
    }
    p = tag_end + 1;
  }
}

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
  size_t p = 0;
  while ((p = s.find(from, p)) != std::string::npos) {
    s.replace(p, from.size(), to);
    p += to.size();
  }
}

std::string Unescape(std::string s) {
  ReplaceAll(s, "&lt;", "<");
  ReplaceAll(s, "&gt;", ">");
  ReplaceAll(s, "&quot;", "\"");
  ReplaceAll(s, "&apos;", "'");
  ReplaceAll(s, "&amp;", "&");
  return s;
}

std::string Trim(std::string s) {
  constexpr const char* kWhitespace = " \t\r\n";
  const size_t begin = s.find_first_not_of(kWhitespace);
  if (begin == std::string::npos) {
    return "";
  }
  const size_t end = s.find_last_not_of(kWhitespace);
  return s.substr(begin, end - begin + 1);
}

}  // namespace

XLast::XLast(std::vector<uint8_t> xml_bytes) {
  if (!xml_bytes.empty()) {
    xml_ = DecodeToUtf8(xml_bytes);
  }
}

std::string XLast::GetLocaleStringFromLanguage(XLanguage language) const {
  return LocaleForLanguage(language);
}

std::string XLast::GetLocalizedString(uint32_t string_id, XLanguage language) const {
  if (xml_.empty()) {
    return "";
  }
  Element element;
  if (!FindElement(xml_, 0, xml_.size(), "LocalizedString", "id", std::to_string(string_id),
                   AttrMatch::kU32, element)) {
    return "";
  }
  Element translation;
  if (!FindElement(xml_, element.content_start, element.content_end, "Translation", "locale",
                   GetLocaleStringFromLanguage(language), AttrMatch::kString, translation)) {
    return "";
  }
  return Unescape(Trim(
      xml_.substr(translation.content_start, translation.content_end - translation.content_start)));
}

std::optional<uint32_t> XLast::GetContextStringId(uint32_t context_id, uint32_t value) const {
  if (xml_.empty()) {
    return std::nullopt;
  }
  Element context;
  if (!FindElement(xml_, 0, xml_.size(), "Context", "id", std::to_string(context_id),
                   AttrMatch::kU32, context)) {
    return std::nullopt;
  }
  Element context_value;
  if (!FindElement(xml_, context.content_start, context.content_end, "ContextValue", "value",
                   std::to_string(value), AttrMatch::kU32, context_value)) {
    return std::nullopt;
  }
  auto it = context_value.attrs.find("stringId");
  if (it == context_value.attrs.end()) {
    return std::nullopt;
  }
  return ParseU32(it->second);
}

std::optional<uint32_t> XLast::GetPresenceStringId(uint32_t presence_mode_value) const {
  if (xml_.empty()) {
    return std::nullopt;
  }
  Element mode;
  if (!FindElement(xml_, 0, xml_.size(), "PresenceMode", "contextValue",
                   std::to_string(presence_mode_value), AttrMatch::kU32, mode)) {
    return std::nullopt;
  }
  auto it = mode.attrs.find("stringId");
  if (it == mode.attrs.end()) {
    return std::nullopt;
  }
  return ParseU32(it->second);
}

std::string XLast::GetPresenceRawString(uint32_t presence_mode_value, XLanguage language) const {
  const auto string_id = GetPresenceStringId(presence_mode_value);
  if (!string_id) {
    return "";
  }
  return GetLocalizedString(*string_id, language);
}

}  // namespace rex::system::util
