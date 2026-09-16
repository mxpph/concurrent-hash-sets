#ifndef HASH_SET_TBB_H
#define HASH_SET_TBB_H

#include <cstddef>
#include <stdexcept>

#include <oneapi/tbb/concurrent_hash_map.h>

#include "src/HashSetBase.h"

// Intel oneTBB's concurrent_hash_map, used as a set by mapping every element to a dummy value.
//
// It is the nearest thing oneTBB has to a concurrent set that supports concurrent removal.
// concurrent_unordered_set is the closer match on paper - it implements the same split-ordered
// list as HashSetLockFree - but its only erase is unsafe_erase, which may not run concurrently
// with any other operation, so it cannot run the scenarios that remove.
//
// The map guards each bucket with a spin reader-writer lock, so it stands in for a fine-grained
// locking design rather than a lock-free one, and is closest in kind to HashSetStriped and
// HashSetRefinable. oneTBB's default tbb_hash_compare<T> hashes with std::hash<T> and compares
// with std::equal_to<T>, which is the hash BucketList uses, so neither side gets a better one.
template <typename T> class HashSetTbb : public HashSetBase<T> {
private:
  // The mapped value is never read; only the key makes up the set.
  using Map = oneapi::tbb::concurrent_hash_map<T, char>;

public:
  explicit HashSetTbb(std::size_t initial_capacity) : map_(ValidatedCapacity(initial_capacity)) {}

  HashSetTbb(const HashSetTbb&) = delete;
  HashSetTbb& operator=(const HashSetTbb&) = delete;
  HashSetTbb(HashSetTbb&&) = delete;
  HashSetTbb& operator=(HashSetTbb&&) = delete;

  ~HashSetTbb() override = default;

  // These are the overloads that take no accessor, so they hold each bucket's lock only for the
  // call rather than handing it back to the caller. That is what the three operations below need,
  // and it keeps them level with a HashSetBase, which answers with a bool and lends out nothing.
  bool Add(T elem) final { return map_.insert(typename Map::value_type{ elem, char{} }); }

  bool Remove(T elem) final { return map_.erase(elem); }

  [[nodiscard]] bool Contains(T elem) final { return map_.count(elem) != 0; }

  [[nodiscard]] size_t Size() const final { return map_.size(); }

private:
  Map map_;

  // Runs before map_ is constructed, so an invalid capacity never reaches oneTBB.
  static std::size_t ValidatedCapacity(std::size_t initial_capacity) {
    if (initial_capacity < 2) {
      throw std::invalid_argument("initial capacity must be at least two");
    }
    return initial_capacity;
  }
};

#endif // HASH_SET_TBB_H
