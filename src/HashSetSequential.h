#ifndef HASH_SET_SEQUENTIAL_H
#define HASH_SET_SEQUENTIAL_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

#include "src/HashSetBase.h"

template <typename T> class HashSetSequential : public HashSetBase<T> {
public:
  explicit HashSetSequential(std::size_t initial_capacity)
      : table_(std::max<std::size_t>(1, initial_capacity)) {}

  bool Add(T elem) final {
    auto& bucket{ GetBucket(elem) };
    if (std::ranges::find(bucket, elem) != bucket.end()) {
      return false;
    }
    bucket.push_back(std::move(elem));
    ++set_size_;
    if (Policy()) {
      Resize();
    }
    return true;
  }

  bool Remove(T elem) final {
    auto& bucket{ GetBucket(elem) };
    auto it{ std::ranges::find(bucket, elem) };
    if (it != bucket.end()) {
      bucket.erase(it);
      --set_size_;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool Contains(T elem) final {
    const auto& bucket{ GetBucket(elem) };
    return std::ranges::find(bucket, elem) != bucket.end();
  }

  [[nodiscard]] size_t Size() const final { return set_size_; }

private:
  static constexpr std::size_t kLoadFactorNumerator{ 3 };
  static constexpr std::size_t kLoadFactorDenominator{ 4 };

  std::vector<std::vector<T>> table_;
  std::size_t set_size_{ 0 };

  [[nodiscard]] bool Policy() const {
    return set_size_ > table_.size() * kLoadFactorNumerator / kLoadFactorDenominator;
  }

  void Resize() {
    const std::size_t new_capacity{ 2 * table_.size() };
    std::vector<std::vector<T>> new_table(new_capacity);
    for (auto& bucket : table_) {
      for (auto& elem : bucket) {
        const std::size_t hash{ std::hash<T>()(elem) % new_capacity };
        new_table[hash].push_back(std::move(elem));
      }
    }
    table_ = std::move(new_table);
  }

  std::vector<T>& GetBucket(const T& elem) {
    const std::size_t hash{ std::hash<T>()(elem) % table_.size() };
    return table_[hash];
  }
};

#endif // HASH_SET_SEQUENTIAL_H
