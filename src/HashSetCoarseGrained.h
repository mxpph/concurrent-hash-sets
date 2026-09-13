#ifndef HASH_SET_COARSE_GRAINED_H
#define HASH_SET_COARSE_GRAINED_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <new>
#include <vector>

#include "src/HashSetBase.h"

template <typename T> class HashSetCoarseGrained : public HashSetBase<T> {
public:
  explicit HashSetCoarseGrained(std::size_t initial_capacity)
      : table_(std::max<std::size_t>(1, initial_capacity)), capacity_{ table_.size() } {}

  bool Add(T elem) final {
    {
      // Using scoped_lock to make sure lock is released on early return
      const std::scoped_lock<std::mutex> lock{ mutex_ };
      auto& bucket{ GetBucket(elem) };
      if (std::ranges::find(bucket, elem) != bucket.end()) {
        return false;
      }
      bucket.push_back(std::move(elem));
      // Using relaxed memory order because we only require atomicity
      set_size_.fetch_add(1, std::memory_order_relaxed);
    }
    if (Policy()) {
      Resize();
    }
    return true;
  }

  bool Remove(T elem) final {
    const std::scoped_lock<std::mutex> lock{ mutex_ };
    auto& bucket{ GetBucket(elem) };
    auto it{ std::ranges::find(bucket, elem) };
    if (it != bucket.end()) {
      bucket.erase(it);
      set_size_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool Contains(T elem) final {
    const std::scoped_lock<std::mutex> lock{ mutex_ };
    const auto& bucket{ GetBucket(elem) };
    return std::ranges::find(bucket, elem) != bucket.end();
  }

  [[nodiscard]] size_t Size() const final { return set_size_.load(std::memory_order_relaxed); }

private:
  static constexpr std::size_t kLoadFactorNumerator{ 3 };
  static constexpr std::size_t kLoadFactorDenominator{ 4 };
  static constexpr std::size_t kAlignment{ std::hardware_destructive_interference_size };

  // Aligned to cacheline size to prevent false sharing
  alignas(kAlignment) std::mutex mutex_;
  alignas(kAlignment) std::vector<std::vector<T>> table_;
  alignas(kAlignment) std::atomic<std::size_t> set_size_{ 0 };
  alignas(kAlignment) std::atomic<std::size_t> capacity_;

  // Capacity and set_size_ are atomic due to potential data races otherwise when resizing the set
  // or adding/removing respectively. The memory order for both is relaxed in this policy because
  // it's okay if we see an old value, because we will check the Policy() again from within the
  // synchronised section of Resize().
  [[nodiscard]] bool Policy() const {
    const std::size_t capacity{ capacity_.load(std::memory_order_relaxed) };
    return set_size_.load(std::memory_order_relaxed) >
           capacity * kLoadFactorNumerator / kLoadFactorDenominator;
  }

  void Resize() {
    // Acquire the lock to ensure no thread is currently modifying the hash set.
    const std::scoped_lock<std::mutex> lock{ mutex_ };
    if (!Policy()) {
      // If the hash set was resized between the time we called Resize() and acquired all the locks,
      // then Policy() will be false, and we don't need to resize again.
      return;
    }
    // Relaxed memory order is okay here because synchronisation is already guaranteed by holding
    // the lock.
    const std::size_t new_capacity{ 2 * capacity_.load(std::memory_order_relaxed) };
    std::vector<std::vector<T>> new_table(new_capacity);
    for (auto& bucket : table_) {
      for (auto& elem : bucket) {
        const std::size_t hash{ std::hash<T>()(elem) % new_capacity };
        new_table[hash].push_back(std::move(elem));
      }
    }
    table_ = std::move(new_table);
    capacity_.store(new_capacity, std::memory_order_relaxed);
  }

  // Called only while holding lock. Relaxed memory order is okay here because synchronisation is
  // already guaranteed by holding the lock.
  std::vector<T>& GetBucket(const T& elem) {
    const std::size_t hash{ std::hash<T>()(elem) % capacity_.load(std::memory_order_relaxed) };
    return table_[hash];
  }
};

#endif // HASH_SET_COARSE_GRAINED_H
