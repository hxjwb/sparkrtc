

#ifndef RTC_BASE_MESSAGE_BUS_H_
#define RTC_BASE_MESSAGE_BUS_H_

#include <map>
#include <mutex>
#include <string>

#include "base/no_destructor.h"
#include "absl/types/optional.h"

namespace webrtc {

class MessageBus {
 public:
  static MessageBus& GetInstance() {
    static base::NoDestructor<MessageBus> instance;
    return *instance;
  }

  void PostMessage(const std::string& key, int value) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_[key] = value;
  }

  absl::optional<int> GetMessage(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = messages_.find(key);
    if (it != messages_.end()) {
      return it->second;
    }
    return absl::nullopt;
  }

 private:
  friend class base::NoDestructor<MessageBus>;

  MessageBus() = default;
  ~MessageBus() = default;
  MessageBus(const MessageBus&) = delete;
  MessageBus& operator=(const MessageBus&) = delete;

  std::mutex mutex_;
  std::map<std::string, int> messages_;
};

}  // namespace webrtc

#endif  // RTC_BASE_MESSAGE_BUS_H_

