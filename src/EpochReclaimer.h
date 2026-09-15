#ifndef EPOCH_RECLAIMER_H
#define EPOCH_RECLAIMER_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

#if defined(__linux__) && __has_include(<linux/membarrier.h>)
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#define EPOCH_RECLAIMER_HAS_MEMBARRIER 1
#endif

namespace detail {

// See membarrier(2)
[[nodiscard]] inline bool RegisterMembarrier() noexcept {
#if defined(EPOCH_RECLAIMER_HAS_MEMBARRIER)
  return ::syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
#else
  return false;
#endif
}

// Runs a full barrier on every thread of this process. See membarrier(2) for details,
// tl;dr it's like mfence but optimised for a hot path and a cold path (this call).
[[nodiscard]] inline bool Membarrier() noexcept {
#if defined(EPOCH_RECLAIMER_HAS_MEMBARRIER)
  return ::syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0;
#else
  return false;
#endif
}

} // namespace detail

// Epoch-based reclamation implementation for nodes that have been unlinked from a lock-free
// structure but may still be read by a thread that loaded a pointer to one before it was unlinked.
template <typename T> class EpochReclaimer {
private:
  // Represents a thread participating in the reclamation for a particular set.
  struct Participant;

public:
  // RAII class for a reclaimer to announce a thread by writing the current global epoch
  // into the thread's Participant slot.
  class Guard {
  public:
    explicit Guard(EpochReclaimer& reclaimer)
        : reclaimer_{ reclaimer }, participant_{ reclaimer.Self() } {
      participant_.Enter(reclaimer.epoch_.load(std::memory_order_acquire));
      EpochReclaimer::AnnouncementFence();
    }

    ~Guard() { participant_.Leave(); }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&&) = delete;
    Guard& operator=(Guard&&) = delete;

    // Takes ownership of `garbage`, deleting it once no thread can still be reading it. Must be
    // called from inside a Guard, by the thread that unlinked it.
    void Retire(T* garbage) { reclaimer_.Retire(participant_, garbage); }

  private:
    EpochReclaimer& reclaimer_;
    Participant& participant_;
  };

  EpochReclaimer() {
    if (!detail::RegisterMembarrier()) {
      throw std::runtime_error("membarrier is required for epoch-based reclamation");
    }
  }

  EpochReclaimer(const EpochReclaimer&) = delete;
  EpochReclaimer& operator=(const EpochReclaimer&) = delete;
  EpochReclaimer(EpochReclaimer&&) = delete;
  EpochReclaimer& operator=(EpochReclaimer&&) = delete;

  // Frees every participant along with everything it was still holding, which is only safe once
  // every thread has finished with the structure.
  ~EpochReclaimer() {
    Participant* curr{ participants_.load(std::memory_order_acquire) };
    while (curr != nullptr) {
      Participant* const next{ curr->next };
      delete curr;
      curr = next;
    }
  }

private:
  void Retire(Participant& self, T* garbage) {
    std::unique_ptr<T> owned{ garbage };
    assert(self.epoch.load(std::memory_order_relaxed) != kInactive &&
           "nodes may only be retired from inside a Guard");
    const std::size_t bag_epoch{ self.announced + 1 };
    const std::size_t index{ bag_epoch % kBags };
    // A node retired at epoch e is safe to free once the global epoch reaches e + 2. Since
    // kBags == 3, when we get back to a particular index, we can unconditionally free whatever's
    // in that bag.
    if (self.bag_epochs[index] != bag_epoch) {
      FreeBag(self.bags[index]);
      self.bag_epochs[index] = bag_epoch;
    }
    self.bags[index].push_back(std::move(owned));
    if (++self.retires == kRetiresPerAdvance) {
      self.retires = 0;
      TryAdvance();
    }
  }

  static constexpr std::size_t kBags{ 3 };
  static constexpr std::size_t kInactive{ std::numeric_limits<std::size_t>::max() };
  static constexpr std::size_t kRetiresPerAdvance{ 1024 };
  static constexpr std::size_t kAlignment{ std::hardware_destructive_interference_size };

  // Represents the state for a single thread participating in the reclamation for a particular set.
  // One per thread per reclaimer/set. Only `epoch` is read by other threads, and only while they
  // are trying to advance the epoch. They form a lock-free linked list via the next pointer, which
  // is necessary for the reclaimer to traverse all participants in order to advance the global
  // epoch.
  //
  // Participants are reused via a cache mechanism rather than being thread_local because otherwise
  // the garbage that a participant stores could be destroyed when a thread exits, before freeing it
  // properly. This decouples the lifecycle of the slot from that of the thread, and allows
  // the EpochReclaimer destructor to safely reclaim all remaining participants and their associated
  // garbage.
  struct alignas(kAlignment) Participant {
    static consteval std::array<std::size_t, kBags> InitBagEpochs() {
      std::array<std::size_t, kBags> bag_epochs{};
      std::ranges::fill(bag_epochs, kInactive);
      return bag_epochs;
    }

    Participant() = default;

    Participant(const Participant&) = delete;
    Participant& operator=(const Participant&) = delete;
    Participant(Participant&&) = delete;
    Participant& operator=(Participant&&) = delete;

    // Frees any bag the epoch has already moved two past, so that a thread which stops retiring
    // lets go of its garbage instead of holding it until the reclaimer is destroyed.
    void CollectGarbage(std::size_t epoch_now) noexcept {
      for (std::size_t index{ 0 }; index < kBags; ++index) {
        const std::size_t bag_epoch{ bag_epochs[index] };
        if (bag_epoch != kInactive && bag_epoch + 2 <= epoch_now) {
          FreeBag(bags[index]);
          bag_epochs[index] = kInactive;
        }
      }
    }

    void Enter(std::size_t epoch_now) noexcept {
      assert(epoch.load(std::memory_order_relaxed) == kInactive && "guards must not be nested");
      if (epoch_now != announced) [[unlikely]] {
        CollectGarbage(epoch_now);
      }
      announced = epoch_now;
      epoch.store(epoch_now, std::memory_order_relaxed);
      // The caller follows this with the barrier that makes the announcement visible before
      // anything is read out of the structure.
    }

    // Release so that the reads made inside the guard cannot be moved past the announcement being
    // dropped.
    void Leave() noexcept { epoch.store(kInactive, std::memory_order_release); }

    std::atomic<std::size_t> epoch{ kInactive };

    // Written once, before the participant is published, and read-only afterwards.
    Participant* next{ nullptr };
    const void* owner{ nullptr };

    // Touched only by the owning thread.
    std::size_t announced{ 0 };
    std::size_t retires{ 0 };
    std::array<std::size_t, kBags> bag_epochs{ InitBagEpochs() };
    std::array<std::vector<std::unique_ptr<T>>, kBags> bags{};
  };

  // We use a cache to speed up the lookup of the Participant associated with a reclaimer/set,
  // rather than traversing the whole participants list every time.
  struct CacheSlot {
    std::size_t reclaimer_id{ 0 };
    Participant* participant{ nullptr };
  };

  // Remembering only the last reclaimer used would be enough for a thread that only ever touches
  // one structure, but a thread alternating between two would then miss on every operation, and
  // it's an expensive operation (traversal of the participants list). So each thread keeps a small
  // array instead, indexed by the reclaimer's id, which makes the lookup a compare against one
  // entry, regardless of many reclaimers the thread is using.
  //
  // Ids are handed out in sequence, so reclaimers (sets) created together land in different entries
  // and never displace each other. Limitation: two reclaimers whose ids happen to be congruent
  // modulo the table size and which the same thread alternates between will keep evicting one
  // another and fall back to the walk.
  static constexpr std::size_t kReclaimerCacheSize{ 16 };

  inline static thread_local std::array<CacheSlot, kReclaimerCacheSize> cache_{};
  inline static std::atomic<std::size_t> next_reclaimer_id_{ 1 };

  // Both are read at every operation, so they share a line.
  alignas(kAlignment) std::atomic<std::size_t> epoch_{ 0 };
  const std::size_t reclaimer_id_{ next_reclaimer_id_.fetch_add(1, std::memory_order_relaxed) };

  // Only touched while advancing the epoch.
  alignas(kAlignment) std::atomic<Participant*> participants_{ nullptr };
  std::atomic<bool> advancing_{};

  static void FreeBag(std::vector<std::unique_ptr<T>>& bag) noexcept { bag.clear(); }

  Participant& Self() {
    const CacheSlot& slot{ cache_[reclaimer_id_ % kReclaimerCacheSize] };
    if (slot.reclaimer_id == reclaimer_id_) [[likely]] {
      return *(slot.participant);
    }
    return Adopt();
  }

  // The thread has not been seen by this reclaimer since it last used another one, so find the
  // slot it left behind, or make it one. Slow path so it's noinline to not waste I-cache space.
  [[gnu::noinline]] Participant& Adopt() {
    const void* const owner{ &cache_ };
    Participant* participant{ participants_.load(std::memory_order_acquire) };
    while (participant != nullptr && participant->owner != owner) {
      participant = participant->next;
    }
    if (participant == nullptr) {
      participant = new Participant{};
      participant->owner = owner;
      participant->next = participants_.load(std::memory_order_relaxed);
      while (!participants_.compare_exchange_weak(
          participant->next, participant, std::memory_order_release, std::memory_order_relaxed)) {
        // Spin (there is low contention here)
      }
    }
    CacheSlot& entry{ cache_[reclaimer_id_ % kReclaimerCacheSize] };
    entry.reclaimer_id = reclaimer_id_;
    entry.participant = participant;
    return *participant;
  }

  // Orders an announcement before the reads that follow it. Hot path (compiler re-ordering barrier)
  static void AnnouncementFence() noexcept { std::atomic_signal_fence(std::memory_order_seq_cst); }

  // Scan: check if all announced threads are at the given epoch.
  [[nodiscard]] bool AllAtEpoch(std::size_t epoch_now) const noexcept {
    for (const Participant* participant{ participants_.load(std::memory_order_acquire) };
         participant != nullptr; participant = participant->next) {
      const std::size_t announced{ participant->epoch.load(std::memory_order_acquire) };
      if (announced != kInactive && announced != epoch_now) {
        return false;
      }
    }
    return true;
  }

  // Orders everything the announcing threads did before the scan that follows. Cold path (syscall)
  [[nodiscard]] static bool FenceBeforeScan() noexcept { return detail::Membarrier(); }

  // Advance the global epoch, if every announced thread is already at it. Nothing is freed here:
  // each thread frees its own bags as it reaches them.
  void TryAdvance() {
    const std::size_t epoch_now{ epoch_.load(std::memory_order_acquire) };
    // Naively check if all threads are already at the current epoch to avoid an unnecessary barrier
    if (!AllAtEpoch(epoch_now)) {
      return;
    }
    if (advancing_.exchange(true, std::memory_order_acquire)) {
      return;
    }
    const bool fenced{ FenceBeforeScan() };
    if (fenced && AllAtEpoch(epoch_now)) {
      std::size_t expected{ epoch_now };
      epoch_.compare_exchange_strong(expected, epoch_now + 1, std::memory_order_acq_rel,
                                     std::memory_order_relaxed);
    }
    advancing_.store(false, std::memory_order_release);
    // Without the barrier the announcements mean nothing, so reclamation would silently stop.
    if (!fenced) {
      throw std::runtime_error("membarrier failed, retired nodes cannot be reclaimed");
    }
  }
};

#endif // EPOCH_RECLAIMER_H
