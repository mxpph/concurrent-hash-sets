#ifndef HASH_SET_LOCK_FREE_H
#define HASH_SET_LOCK_FREE_H

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>

#include "src/BucketList.h"
#include "src/HashSetBase.h"

// Implementation based on that in "The Art Of Multiprocessor Programming" (Herlihy and Shavit)
template <typename T> class HashSetLockFree : public HashSetBase<T> {
private:
  using SentinelPtr = typename BucketList<T>::SentinelPtr;

public:
  explicit HashSetLockFree(std::size_t initial_capacity = kDefaultInitialCapacity,
                           std::size_t max_capacity = kDefaultMaxCapacity)
      : max_capacity_{ max_capacity }, capacity_{ initial_capacity } {
    if (initial_capacity < 2) {
      throw std::invalid_argument("initial capacity must be at least two");
    }
    if ((initial_capacity - 1) & initial_capacity) {
      throw std::invalid_argument("initial capacity must be a power of two");
    }
    if ((max_capacity - 1) & max_capacity) {
      throw std::invalid_argument("max capacity must be a power of two");
    }
    if (initial_capacity > max_capacity) {
      throw std::invalid_argument("initial capacity must not exceed max capacity");
    }
    if (max_capacity > BucketList<T>::kMaxBuckets) {
      throw std::invalid_argument("bucket index must fit in the sentinel key space");
    }
    buckets_ = std::make_unique<std::atomic<SentinelPtr>[]>(max_capacity);
    buckets_[0].store(BucketList<T>::MakeRoot().GetHead(), std::memory_order_relaxed);
  }

  ~HashSetLockFree() override {
    BucketList<T>::Destroy(buckets_[0].load(std::memory_order_relaxed));
  }

  bool Add(T elem) final {
    std::size_t current_capacity{ capacity_.load(std::memory_order_relaxed) };
    std::size_t bucket_index{ BucketList<T>::Hash(elem) % current_capacity };
    BucketList<T> bucket{ GetBucketList(bucket_index) };
    if (!bucket.Add(elem)) {
      return false;
    }
    set_size_.fetch_add(1, std::memory_order_relaxed);
    ResizeIfNecessary();
    return true;
  }

  bool Remove(T elem) final {
    const std::size_t current_capacity{ capacity_.load(std::memory_order_relaxed) };
    const std::size_t bucket_index{ BucketList<T>::Hash(elem) % current_capacity };
    BucketList<T> bucket{ GetBucketList(bucket_index) };
    if (!bucket.Remove(elem)) {
      return false;
    }
    set_size_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }

  // NOT Wait-free as GetBucketList can allocate.
  [[nodiscard]] bool Contains(T elem) final {
    const std::size_t current_capacity{ capacity_.load(std::memory_order_relaxed) };
    const std::size_t bucket_index{ BucketList<T>::Hash(elem) % current_capacity };
    BucketList<T> bucket{ GetBucketList(bucket_index) };
    return bucket.Contains(elem);
  }

  [[nodiscard]] size_t Size() const final { return set_size_.load(std::memory_order_relaxed); }

private:
  static constexpr std::size_t kDefaultMaxCapacity{ 1U << 20 };
  static constexpr std::size_t kDefaultInitialCapacity{ 2 };
  static constexpr std::size_t kLoadFactorNumerator{ 3 };
  static constexpr std::size_t kLoadFactorDenominator{ 4 };
  static constexpr std::size_t kAlignment{ std::hardware_destructive_interference_size };

  alignas(kAlignment) std::unique_ptr<std::atomic<SentinelPtr>[]> buckets_;
  const std::size_t max_capacity_{ kDefaultMaxCapacity };

  // Aligned to cacheline size to prevent false sharing
  alignas(kAlignment) std::atomic<std::size_t> capacity_{ kDefaultInitialCapacity };
  alignas(kAlignment) std::atomic<std::size_t> set_size_{ 0 };

  static constexpr std::size_t GetParentIndex(std::size_t index) noexcept {
    return index - std::bit_floor(index);
  }

  void ResizeIfNecessary() {
    std::size_t capacity{ capacity_.load(std::memory_order_relaxed) };
    const std::size_t set_size{ set_size_.load(std::memory_order_relaxed) };
    if (capacity < max_capacity_ &&
        set_size > capacity * kLoadFactorNumerator / kLoadFactorDenominator) {
      capacity_.compare_exchange_strong(capacity, capacity * 2, std::memory_order_relaxed,
                                        std::memory_order_relaxed);
    }
  }

  BucketList<T> GetBucketList(std::size_t index) {
    assert(index < max_capacity_);
    SentinelPtr sentinel{ buckets_[index].load(std::memory_order_acquire) };
    if (sentinel == nullptr) {
      sentinel = InitializeBucket(index);
    }
    return BucketList<T>::FromSentinel(sentinel);
  }

  SentinelPtr InitializeBucket(std::size_t index) {
    assert(index != 0);
    BucketList<T> parent{ GetBucketList(GetParentIndex(index)) };
    const SentinelPtr sentinel{ parent.SentinelFor(index) };
    SentinelPtr expected{ nullptr };
    buckets_[index].compare_exchange_strong(expected, sentinel, std::memory_order_release,
                                            std::memory_order_relaxed);
    // `sentinel` is now the node in the list, whether we won or lost the race
    return sentinel;
  }
};

#endif // HASH_SET_LOCK_FREE_H
