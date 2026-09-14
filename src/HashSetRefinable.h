#ifndef HASH_SET_REFINABLE_H
#define HASH_SET_REFINABLE_H

#include <emmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "src/HashSetBase.h"
#include "src/PaddedMutex.h"

template <typename T> class HashSetRefinable : public HashSetBase<T> {
public:
  explicit HashSetRefinable(std::size_t initial_capacity)
      : initial_shift_{ ValidatedShift(initial_capacity) }, table_(initial_capacity),
        capacity_{ initial_capacity } {
    EnlargeMutexArena(0);
  }

  bool Add(T elem) final {
    {
      std::size_t hash{};
      const RefinableScopedLock refinable_scoped_lock{ *this, elem, hash };
      auto& bucket{ GetBucket(hash) };
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
    std::size_t hash{};
    const RefinableScopedLock refinable_scoped_lock{ *this, elem, hash };
    auto& bucket{ GetBucket(hash) };
    auto it{ std::ranges::find(bucket, elem) };
    if (it != bucket.end()) {
      bucket.erase(it);
      set_size_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool Contains(T elem) final {
    std::size_t hash{};
    const RefinableScopedLock refinable_scoped_lock{ *this, elem, hash };
    const auto& bucket{ GetBucket(hash) };
    return std::ranges::find(bucket, elem) != bucket.end();
  }

  [[nodiscard]] size_t Size() const final { return set_size_.load(std::memory_order_relaxed); }

private:
  static constexpr std::size_t kLoadFactorNumerator{ 3 };
  static constexpr std::size_t kLoadFactorDenominator{ 4 };
  static constexpr std::size_t kMaxBlocks{
    static_cast<std::size_t>(std::numeric_limits<std::size_t>::digits) + 1
  };

  // A mutex arena is formed by a collection of contiguous blocks of mutexes to reduce allocation
  // overhead. Each resize appends one block, holding the mutexes for the buckets that the resize
  // adds. Blocks are never moved or freed while the set is alive, so a thread may hold a mutex
  // across a resize, and the mutex for a bucket is fixed for the lifetime of the set.
  using MutexBlock = std::unique_ptr<detail::PaddedMutex[]>;

  // RAII class for handling the refinable locking logic
  class RefinableScopedLock {
  public:
    explicit RefinableScopedLock(const HashSetRefinable& parent, const T& elem, std::size_t& hash) {
      while (true) {
        // Acquire synchronises with the release order when resizing ends.
        while (parent.resizing_.load(std::memory_order_acquire)) {
          // Use x86 pause intrinsic to indicate that this thread is spinning
          _mm_pause();
          _mm_pause();
          _mm_pause();
          _mm_pause();
        }
        // Acquire synchronises with the release order when the new capacity is published
        const std::size_t old_capacity{ parent.capacity_.load(std::memory_order_acquire) };
        hash = std::hash<T>()(elem) % old_capacity;
        elem_lock_ = &parent.GetMutex(hash);
        elem_lock_->lock();
        // The mutex for a bucket never changes, so Resize() only invalidates `hash`.
        // If no thread started resizing, or completed a resize during this section of the code, our
        // hash is still the one this capacity gives, and thus we hold the right mutex.
        if (!parent.resizing_.load(std::memory_order_acquire) &&
            parent.capacity_.load(std::memory_order_relaxed) == old_capacity) {
          return;
        }
        // Otherwise, we must retry.
        elem_lock_->unlock();
      }
    }
    ~RefinableScopedLock() { elem_lock_->unlock(); }

    RefinableScopedLock(const RefinableScopedLock&) = delete;
    RefinableScopedLock& operator=(const RefinableScopedLock&) = delete;
    RefinableScopedLock(RefinableScopedLock&&) = delete;
    RefinableScopedLock& operator=(RefinableScopedLock&&) = delete;

  private:
    std::mutex* elem_lock_{ nullptr };
  };

  [[nodiscard]] static std::size_t ValidatedShift(std::size_t initial_capacity) {
    if (initial_capacity < 2) {
      throw std::invalid_argument("initial capacity must be at least two");
    }
    if ((initial_capacity - 1) & initial_capacity) {
      throw std::invalid_argument("initial capacity must be a power of two");
    }
    return static_cast<std::size_t>(std::countr_zero(initial_capacity));
  }

  std::array<MutexBlock, kMaxBlocks> arena_{};
  const std::size_t initial_shift_;

  // Aligned to cacheline size to prevent false sharing
  alignas(detail::kAlignment) std::vector<std::vector<T>> table_;
  alignas(detail::kAlignment) std::atomic<std::size_t> capacity_;
  alignas(detail::kAlignment) std::atomic<bool> resizing_{ false };
  alignas(detail::kAlignment) std::atomic<std::size_t> set_size_{ 0 };

  // Capacity and set_size_ are atomic due to potential data races otherwise when resizing the set
  // or adding/removing respectively. The memory order for both is relaxed in this policy because
  // it's okay if we see an old value since the Resize function is synchronised, and we will check
  // the Policy() again from within the synchronised section.
  [[nodiscard]] bool Policy() const {
    const std::size_t capacity{ capacity_.load(std::memory_order_relaxed) };
    return set_size_.load(std::memory_order_relaxed) >
           capacity * kLoadFactorNumerator / kLoadFactorDenominator;
  }

  void Resize() {
    // Allow this thread to resize if resizing_ is false, atomically setting resizing_ to true to
    // prevent other threads from resizing simultaneously
    if (!resizing_.exchange(true, std::memory_order_acq_rel)) {
      if (!Policy()) {
        // If the hash set was resized between the time we called Resize() and acquired all the
        // locks, then Policy() will be false, and we don't need to resize again.
        resizing_.store(false, std::memory_order_relaxed);
        return;
      }
      // Relaxed memory order is okay here because synchronisation is already guaranteed by the
      // resizing_ atomic bool being true
      const std::size_t old_capacity{ capacity_.load(std::memory_order_relaxed) };
      Quiesce(old_capacity);
      const std::size_t new_capacity{ 2 * old_capacity };
      EnlargeMutexArena(BlockIndex(old_capacity));
      std::vector<std::vector<T>> new_table(new_capacity);
      for (auto& bucket : table_) {
        for (auto& elem : bucket) {
          const std::size_t hash{ std::hash<T>()(elem) % new_capacity };
          new_table[hash].push_back(std::move(elem));
        }
      }

      // This has to be release to ensure that the write to the new block is visible if a thread
      // reads the new capacity then hashes and accesses it before resizing_ is changed.
      capacity_.store(new_capacity, std::memory_order_release);
      table_ = std::move(new_table);
      // This has to be release to ensure that the write to table_ is visible in acquire once
      // resizing_ is changed to false causing the spin to end.
      resizing_.store(false, std::memory_order_release);
    }
  }

  // Ensure that no other thread is in the middle of an Add(), Remove(), or Contains() call by
  // acquiring and immediately releasing all the mutexes.
  void Quiesce(std::size_t capacity) {
    for (std::size_t index{ 0 }; index < capacity; ++index) {
      const std::scoped_lock<std::mutex> lock{ GetMutex(index) };
    }
  }

  void EnlargeMutexArena(std::size_t block_index) {
    assert(arena_[block_index] == nullptr && "a block is only allocated once");
    arena_[block_index] = std::make_unique<detail::PaddedMutex[]>(BlockSize(block_index));
  }

  [[nodiscard]] std::size_t BlockIndex(std::size_t index) const noexcept {
    return static_cast<std::size_t>(std::bit_width(index >> initial_shift_));
  }

  [[nodiscard]] std::size_t BlockOffset(std::size_t index) const noexcept {
    return index - (std::bit_floor(index >> initial_shift_) << initial_shift_);
  }

  // Blocks grow in powers of two
  [[nodiscard]] std::size_t BlockSize(std::size_t block_index) const noexcept {
    return std::size_t{ 1 } << (block_index == 0 ? initial_shift_
                                                 : initial_shift_ + block_index - 1);
  }

  [[nodiscard]] std::mutex& GetMutex(std::size_t index) const noexcept {
    const std::size_t block_index{ BlockIndex(index) };
    const std::size_t offset{ BlockOffset(index) };
    assert(arena_[block_index] != nullptr && "the block must be allocated before it is locked");
    assert(offset < BlockSize(block_index) && "the offset must fall inside the block");
    return arena_[block_index][offset].mutex;
  }

  // Called only while holding lock
  std::vector<T>& GetBucket(std::size_t hash) noexcept { return table_[hash]; }
};

#endif // HASH_SET_REFINABLE_H
