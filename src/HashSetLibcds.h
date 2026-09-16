#ifndef HASH_SET_LIBCDS_H
#define HASH_SET_LIBCDS_H

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>

#include <cds/algo/atomic.h>
#include <cds/container/michael_list_dhp.h>
#include <cds/container/split_list_set.h>
#include <cds/gc/dhp.h>
#include <cds/init.h>

#include "src/HashSetBase.h"

namespace detail {

// The process-wide libcds collector. Exactly one may exist, and it has to be constructed before
// any set and destroyed after every set and every LibcdsGuard. cds::Initialize() has to run before
// the collector is built, which is why the collector is held behind a pointer rather than as a
// member that would be constructed first.
class LibcdsReclaimer {
public:
  LibcdsReclaimer() {
    cds::Initialize();
    collector_ = std::make_unique<cds::gc::DHP>();
  }

  LibcdsReclaimer(const LibcdsReclaimer&) = delete;
  LibcdsReclaimer& operator=(const LibcdsReclaimer&) = delete;
  LibcdsReclaimer(LibcdsReclaimer&&) = delete;
  LibcdsReclaimer& operator=(LibcdsReclaimer&&) = delete;

  ~LibcdsReclaimer() {
    collector_.reset();
    cds::Terminate();
  }

private:
  std::unique_ptr<cds::gc::DHP> collector_;
};

// Attaches the calling thread to the collector for the guard's lifetime. Every thread that touches
// a HashSetLibcds must hold one for the whole of its work, including the thread that constructs the
// set, prefills it and destroys it - destroying the set retires nodes, which needs the attachment
// just as much as an Add does.
//
// libcds attaches idempotently but detaches unconditionally, so a guard taken inside another guard
// on the same thread would otherwise detach the outer one. The depth count makes the inner guard a
// no-op instead.
class LibcdsGuard {
public:
  LibcdsGuard() {
    if (depth_++ == 0) {
      cds::gc::dhp::smr::attach_thread();
    }
  }

  LibcdsGuard(const LibcdsGuard&) = delete;
  LibcdsGuard& operator=(const LibcdsGuard&) = delete;
  LibcdsGuard(LibcdsGuard&&) = delete;
  LibcdsGuard& operator=(LibcdsGuard&&) = delete;

  ~LibcdsGuard() {
    if (--depth_ == 0) {
      cds::gc::dhp::smr::detach_thread();
    }
  }

private:
  inline static thread_local std::size_t depth_{ 0 };
};

} // namespace detail

// libcds's SplitListSet, which is the reference implementation of the same split-ordered list that
// HashSetLockFree implements, taken from the same Shalev and Shavit paper. It lays a Michael
// lock-free ordered list under the same recursive split of the buckets, and reclaims nodes with
// dynamic hazard pointers where HashSetLockFree uses EpochReclaimer, so it is the one baseline that
// differs from HashSetLockFree in its reclamation scheme rather than in its algorithm.
//
// The set is hashed with std::hash<T>, the hash BucketList uses, and counts its elements with an
// atomic counter, which is what HashSetLockFree's set_size_ is - the libcds default counts nothing
// and would report a size of zero.
//
// It takes a max capacity rather than an initial one, because that is the only capacity libcds
// lets you set. Its constructor's item count fixes the bucket table's segment count once and for
// all, and the table can never grow past it, so the argument is HashSetLockFree's max_capacity
// rather than its initial_capacity. Passing an initial capacity here would cap the set there and
// leave it with buckets thousands of elements long. There is no way to pre-size the bucket count
// the set starts with: like HashSetLockFree at its default, it starts at two buckets and doubles,
// allocating each segment of the table on first use.
template <typename T> class HashSetLibcds : public HashSetBase<T> {
private:
  // libcds takes a whole number of elements per bucket, so one is as near as it can get to
  // HashSetLockFree's load factor of three quarters. With one, the bucket count the set grows into
  // is the max capacity it was given.
  static constexpr std::size_t kLoadFactor{ 1 };

  struct SetTraits : public cds::container::split_list::traits {
    using ordered_list = cds::container::michael_list_tag;
    using hash = std::hash<T>;
    using item_counter = cds::atomicity::item_counter;

    struct ordered_list_traits : public cds::container::michael_list::traits {
      using less = std::less<T>;
    };
  };

  using SplitListSet = cds::container::SplitListSet<cds::gc::DHP, T, SetTraits>;

public:
  explicit HashSetLibcds(std::size_t max_capacity)
      : set_(ValidatedCapacity(max_capacity), kLoadFactor) {}

  HashSetLibcds(const HashSetLibcds&) = delete;
  HashSetLibcds& operator=(const HashSetLibcds&) = delete;
  HashSetLibcds(HashSetLibcds&&) = delete;
  HashSetLibcds& operator=(HashSetLibcds&&) = delete;

  // Retires the nodes that are still linked in, so the calling thread must still be attached.
  ~HashSetLibcds() override = default;

  bool Add(T elem) final { return set_.insert(elem); }

  bool Remove(T elem) final { return set_.erase(elem); }

  [[nodiscard]] bool Contains(T elem) final { return set_.contains(elem); }

  [[nodiscard]] size_t Size() const final { return set_.size(); }

private:
  SplitListSet set_;

  // Runs before set_ is constructed, so an invalid capacity never reaches libcds.
  static std::size_t ValidatedCapacity(std::size_t max_capacity) {
    if (max_capacity < 2) {
      throw std::invalid_argument("max capacity must be at least two");
    }
    return max_capacity;
  }
};

#endif // HASH_SET_LIBCDS_H
