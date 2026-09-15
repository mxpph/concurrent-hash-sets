#ifndef HASH_SET_REFINABLE_H
#define HASH_SET_REFINABLE_H

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "src/HashSetBase.h"
#include "src/PaddedMutex.h"

template <typename T> class HashSetRefinable : public HashSetBase<T> {
public:
  explicit HashSetRefinable(std::size_t initial_capacity)
      : table_(std::max<std::size_t>(1, initial_capacity)), capacity_{ table_.size() } {
    EnlargeMutexArena(table_.size());
    mutex_ptrs_.store(BuildMutexPointers(table_.size()), std::memory_order_relaxed);
  }

  ~HashSetRefinable() override {
    // Free all mutex pointers once we're done
    for (auto* ptr : leaked_ptrs_) {
      delete ptr;
    }
    delete mutex_ptrs_.load(std::memory_order_relaxed);
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

  using MutexPtrs = std::vector<detail::PaddedMutex*>;

  // A mutex arena is formed by a collection of contiguous blocks of mutexes to reduce allocation
  // overhead. When new mutexes are needed, a new block is allocated and appended to the arena.
  struct MutexBlock {
    std::unique_ptr<detail::PaddedMutex[]> mutexes;
    std::size_t count{ 0 };
  };

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
        // Atomically load the mutex ptrs, because another thread could resize the hash set while we
        // are in this section of the code, which would cause a data race otherwise.
        MutexPtrs* mutex_ptrs{ parent.mutex_ptrs_.load(std::memory_order_relaxed) };
        const std::size_t old_capacity{ parent.capacity_.load(std::memory_order_relaxed) };
        hash = std::hash<T>()(elem) % old_capacity;
        elem_lock_ = &(*mutex_ptrs)[hash]->mutex;
        elem_lock_->lock();
        // If another thread didn't start resizing, or complete resizing and reallocate the
        // mutex_ptrs during this section, we can return.
        if (!parent.resizing_.load(std::memory_order_acquire) &&
            parent.mutex_ptrs_.load(std::memory_order_relaxed) == mutex_ptrs) {
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

  std::vector<MutexBlock> arena_;
  std::vector<MutexPtrs*> leaked_ptrs_;

  // Aligned to cacheline size to prevent false sharing
  alignas(detail::kAlignment) std::atomic<MutexPtrs*> mutex_ptrs_;
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
      auto* old_mutex_ptrs{ mutex_ptrs_.load(std::memory_order_relaxed) };
      Quiesce(old_mutex_ptrs);
      // Relaxed memory order is okay here because synchronisation is already guaranteed by the
      // resizing_ atomic bool being true and Quiesce finished
      const std::size_t old_capacity{ capacity_.load(std::memory_order_relaxed) };
      const std::size_t new_capacity{ 2 * old_capacity };
      EnlargeMutexArena(new_capacity - old_capacity);
      std::vector<std::vector<T>> new_table(new_capacity);
      for (auto& bucket : table_) {
        for (auto& elem : bucket) {
          const std::size_t hash{ std::hash<T>()(elem) % new_capacity };
          new_table[hash].push_back(std::move(elem));
        }
      }

      auto* new_mutex_ptrs{ BuildMutexPointers(new_capacity) };
      // We cannot immediately free the old mutex pointers. This is essential to the soundness of
      // the algorithm, because we cannot be sure how many threads have still loaded the old pointer
      // and are attempting to lock a bucket.
      leaked_ptrs_.push_back(old_mutex_ptrs);
      capacity_.store(new_capacity, std::memory_order_relaxed);
      mutex_ptrs_.store(new_mutex_ptrs, std::memory_order_relaxed);
      table_ = std::move(new_table);
      // This has to be release to ensure that the write to table_ is visible in acquire once
      // resizing_ is changed to false causing the spin to end.
      resizing_.store(false, std::memory_order_release);
    }
  }

  // Ensure that no other thread is in the middle of an Add(), Remove(), or Contains() call by
  // acquiring and immediately releasing all the mutexes.
  void Quiesce(MutexPtrs* mutex_ptrs) {
    for (auto* stripe : *mutex_ptrs) {
      const std::scoped_lock<std::mutex> lock{ stripe->mutex };
    }
  }

  void EnlargeMutexArena(std::size_t count) {
    arena_.push_back(MutexBlock{ std::make_unique<detail::PaddedMutex[]>(count), count });
  }

  // Build a new table of pointers to the lock. This is done instead of resizing the existing one,
  // which could potentially cause a move of all the elements in memory if contiguous allocation was
  // not possible, causing a race condition in the acquire function in between load()ing the pointer
  // and accessing the underlying mutex, which would segfault in such a case.
  [[nodiscard]] MutexPtrs* BuildMutexPointers(std::size_t capacity) const noexcept {
    auto* mutex_ptrs{ new MutexPtrs };
    mutex_ptrs->reserve(capacity);
    for (const auto& block : arena_) {
      for (std::size_t i{ 0 }; i < block.count; ++i) {
        mutex_ptrs->push_back(&block.mutexes[i]);
      }
    }
    assert(mutex_ptrs->size() == capacity && "blocks must contain the whole capacity");
    return mutex_ptrs;
  }

  // Called only while holding lock
  std::vector<T>& GetBucket(std::size_t hash) noexcept { return table_[hash]; }
};

#endif // HASH_SET_REFINABLE_H
