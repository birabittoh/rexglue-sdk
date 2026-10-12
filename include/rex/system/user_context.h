/**
 * @file        rex/system/user_context.h
 * @brief       Per-user XGIUserSetContext store and rich presence resolution.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/util/xlast.h>
#include <rex/system/xcontent.h>

namespace rex::system {

class KernelState;

struct UserContextEvent {
  uint32_t user_index = 0;
  uint32_t context_id = 0;
  uint32_t value = 0;
  // Localized string id the title defines for (context_id, value), 0 if none.
  uint32_t string_id = 0;
  // Resolved rich presence string after applying this update, empty if none.
  std::string presence_string;
};

using UserContextListenerHandle = uint64_t;
using UserContextCallback = std::function<void(const UserContextEvent&)>;

// Stores the values titles publish with XGIUserSetContext and turns them into
// localized rich presence strings using the title's XLast metadata.
class UserContextManager {
 public:
  explicit UserContextManager(KernelState* kernel_state);

  void SetContext(uint32_t user_index, uint32_t context_id, uint32_t value);
  std::optional<uint32_t> GetContext(uint32_t user_index, uint32_t context_id) const;
  std::map<uint32_t, uint32_t> GetContexts(uint32_t user_index) const;

  // Resolved rich presence string for a user, empty when unavailable.
  std::string GetPresenceString(uint32_t user_index,
                                XLanguage language = XLanguage::kEnglish) const;

  UserContextListenerHandle RegisterContextCallback(UserContextCallback callback);
  void UnregisterCallback(UserContextListenerHandle handle);

  // Supplies the title's XLast metadata directly rather than reading the XDBF
  // resource out of the guest image. `xml_bytes` is the raw XLast document
  // (UTF-16 or UTF-8). Safe to call before the title's first
  // XGIUserSetContext; passing an empty vector clears any injected data.
  void SetXLastBytes(std::vector<uint8_t> xml_bytes);

 private:
  const util::XLast* xlast() const;

  KernelState* kernel_state_ = nullptr;
  mutable std::mutex mutex_;
  std::map<uint32_t, std::map<uint32_t, uint32_t>> contexts_;
  std::vector<uint8_t> injected_xlast_bytes_;
  mutable std::unique_ptr<util::XLast> xlast_;
  mutable bool xlast_loaded_ = false;
  std::unordered_map<UserContextListenerHandle, UserContextCallback> callbacks_;
  UserContextListenerHandle next_listener_handle_ = 1;
};

}  // namespace rex::system
