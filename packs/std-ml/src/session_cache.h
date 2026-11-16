#pragma once

#include <cstddef>
#include <filesystem>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace lyflow::ops::detail {

inline std::filesystem::path sessionModelPath(const std::filesystem::path& model) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(model, ec);
  if (ec)
    return model;
  const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
  return ec ? absolute : canonical;
}

struct SessionKey {
  std::string model;
  std::string stamp;
  int threads;

  bool operator==(const SessionKey& other) const {
    return model == other.model && stamp == other.stamp && threads == other.threads;
  }
};

template <typename T> class SessionCache {
public:
  explicit SessionCache(std::size_t capacity) : capacity_(capacity) {}

  template <typename Factory>
  std::shared_ptr<T> getOrCreate(const SessionKey& key, Factory&& create) {
    if (auto cached = find(key))
      return cached;
    std::unique_lock<std::mutex> creating(creationMutex_);
    if (auto cached = find(key))
      return cached;
    auto made = std::forward<Factory>(create)();
    if (!made)
      return made;

    std::list<Entry> retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->key.model == key.model && it->key.stamp != key.stamp) {
          auto stale = it++;
          retired.splice(retired.end(), entries_, stale);
        } else {
          ++it;
        }
      }
      entries_.push_front({key, made});
      while (entries_.size() > capacity_)
        retired.splice(retired.end(), entries_, std::prev(entries_.end()));
    }
    creating.unlock();
    return made;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

private:
  struct Entry {
    SessionKey key;
    std::shared_ptr<T> session;
  };

  std::shared_ptr<T> find(const SessionKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key == key) {
        auto session = it->session;
        entries_.splice(entries_.begin(), entries_, it);
        return session;
      }
    }
    return {};
  }

  const std::size_t capacity_;
  std::mutex creationMutex_;
  mutable std::mutex mutex_;
  std::list<Entry> entries_;
};

} // namespace lyflow::ops::detail
