/**
 * @file        rex/system/user_context.cpp
 * @brief       See rex/system/user_context.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/user_context.h>

#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/presence_string_builder.h>
#include <rex/system/util/xdbf_utils.h>

namespace rex::system {

UserContextManager::UserContextManager(KernelState* kernel_state) : kernel_state_(kernel_state) {}

const util::XLast* UserContextManager::xlast() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (xlast_loaded_) {
    return xlast_.get();
  }
  auto loaded = std::make_unique<util::XLast>();
  std::vector<uint8_t> xlast_bytes;
  if (!injected_xlast_bytes_.empty()) {
    xlast_bytes = injected_xlast_bytes_;
  } else if (kernel_state_) {
    util::XdbfGameData db = kernel_state_->title_xdbf();
    xlast_bytes = db.is_valid() ? db.xlast() : std::vector<uint8_t>{};
  }
  if (!xlast_bytes.empty()) {
    *loaded = util::XLast(std::move(xlast_bytes));
  }
  xlast_ = std::move(loaded);
  xlast_loaded_ = true;
  REXKRNL_INFO("XLast load: has_xlast={}", xlast_->HasXLast());
  return xlast_.get();
}

void UserContextManager::SetXLastBytes(std::vector<uint8_t> xml_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  injected_xlast_bytes_ = std::move(xml_bytes);
  xlast_.reset();
  xlast_loaded_ = false;
}

void UserContextManager::SetContext(uint32_t user_index, uint32_t context_id, uint32_t value) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    contexts_[user_index][context_id] = value;
  }

  UserContextEvent event;
  event.user_index = user_index;
  event.context_id = context_id;
  event.value = value;
  if (const util::XLast* xl = xlast(); xl && xl->HasXLast()) {
    if (auto string_id = xl->GetContextStringId(context_id, value)) {
      event.string_id = *string_id;
    }
  }
  event.presence_string = GetPresenceString(user_index);

  REXKRNL_INFO("SetContext user={} ctx={:08X} val={} -> presence='{}'", user_index, context_id,
               value, event.presence_string);

  std::vector<UserContextCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks.reserve(callbacks_.size());
    for (auto& entry : callbacks_) {
      callbacks.push_back(entry.second);
    }
  }
  for (auto& callback : callbacks) {
    callback(event);
  }
}

std::optional<uint32_t> UserContextManager::GetContext(uint32_t user_index,
                                                       uint32_t context_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto user_it = contexts_.find(user_index);
  if (user_it == contexts_.end()) {
    return std::nullopt;
  }
  auto context_it = user_it->second.find(context_id);
  if (context_it == user_it->second.end()) {
    return std::nullopt;
  }
  return context_it->second;
}

std::map<uint32_t, uint32_t> UserContextManager::GetContexts(uint32_t user_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = contexts_.find(user_index);
  if (it == contexts_.end()) {
    return {};
  }
  return it->second;
}

std::string UserContextManager::GetPresenceString(uint32_t user_index, XLanguage language) const {
  std::map<uint32_t, uint32_t> contexts;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(user_index);
    if (it == contexts_.end()) {
      return "";
    }
    contexts = it->second;
  }

  const util::XLast* xl = xlast();
  if (!xl || !xl->HasXLast()) {
    return "";
  }
  // Titles ship a single presence mode (contextValue 0) by default; that is the
  // template XAM shows when no other mode is selected.
  const std::string raw = xl->GetPresenceRawString(0, language);
  if (raw.empty()) {
    REXKRNL_INFO("GetPresenceString user={}: no raw presence template", user_index);
    return "";
  }
  std::string result =
      util::PresenceStringBuilder(raw, xl, std::move(contexts), language).presence_string();
  REXKRNL_INFO("GetPresenceString user={}: raw='{}' -> '{}'", user_index, raw, result);
  return result;
}

UserContextListenerHandle UserContextManager::RegisterContextCallback(
    UserContextCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  const UserContextListenerHandle handle = next_listener_handle_++;
  callbacks_[handle] = std::move(callback);
  return handle;
}

void UserContextManager::UnregisterCallback(UserContextListenerHandle handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_.erase(handle);
}

}  // namespace rex::system
