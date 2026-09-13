#ifndef PADDED_MUTEX_H
#define PADDED_MUTEX_H

#include <cstddef>
#include <mutex>
#include <new>

namespace detail {
constexpr std::size_t kAlignment{ std::hardware_destructive_interference_size };

// One stripe per cache line to avoid false sharing.
struct alignas(kAlignment) PaddedMutex {
  std::mutex mutex;
};
} // namespace detail

#endif // PADDED_MUTEX_H
