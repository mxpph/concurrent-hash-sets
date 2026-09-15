#ifndef BUCKETLIST_H
#define BUCKETLIST_H

#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

#include "src/MarkableReference.h"

static_assert(std::atomic<uintptr_t>::is_always_lock_free,
              "lock-free pointer-sized atomic operations are required");

namespace detail {
[[nodiscard]] constexpr std::size_t ReverseBits(std::size_t n) noexcept {
  constexpr std::size_t m1{ static_cast<std::size_t>(0x5555555555555555ULL) };
  constexpr std::size_t m2{ static_cast<std::size_t>(0x3333333333333333ULL) };
  constexpr std::size_t m4{ static_cast<std::size_t>(0x0F0F0F0F0F0F0F0FULL) };
  n = ((n >> 1) & m1) | ((n & m1) << 1);
  n = ((n >> 2) & m2) | ((n & m2) << 2);
  n = ((n >> 4) & m4) | ((n & m4) << 4);
  return std::byteswap(n);
}
} // namespace detail

// A lock-free set backed by a singly linked list, sorted by the split-order hash of each element.
// Sentinel head and tail nodes are used so that every node has both a predecessor and a successor,
// which handles the special cases at either end of the list.
//
// To remove a node, it is first *logically* deleted by setting a mark bit on its own `next`
// pointer, which also prevents any node from being linked in after it. It is *physically* unlinked
// afterwards, by whichever thread next traverses over it. Marking the node rather than the
// predecessor's pointer is what makes this safe: a concurrent Insert that tries to splice a new
// node in after a marked node will see the mark and fail its CAS.
//
// Nodes are never reclaimed while the list is in use. Once a node has been unlinked another thread
// may still be traversing it, and without a reclamation scheme there is no way to know when the
// last such reference is gone. Removed nodes are therefore deliberately leaked. Destroy() frees the
// nodes that are still linked in, which is only safe once every thread has finished.
//
// TODO: Introduce RCU / EBR / hazard pointers.
template <typename T> class BucketList {
private:
  struct Node;
  static constexpr int kKeyBits{ std::numeric_limits<std::size_t>::digits };
  static constexpr std::size_t kKeyMask{ (std::size_t{ 1 } << (kKeyBits - 2)) - 1 };

public:
  using SentinelPtr = Node*;

  static constexpr std::size_t kMaxBuckets = kKeyMask + 1;

  static BucketList<T> MakeRoot() {
    BucketList<T> list{ new Node{
        .next{ MarkableReference<Node*>{ new Node{ .key{ kTailKey } }, false } } } };
    return list;
  }

  static BucketList<T> FromSentinel(SentinelPtr sentinel) { return BucketList<T>{ sentinel }; }

  // Nothing here synchronises with anything: tearing down a list that another thread may still be
  // traversing is undefined.
  static void Destroy(SentinelPtr sentinel) noexcept {
    Node* node{ sentinel };
    while (node != nullptr) {
      Node* next{ node->next.Load(std::memory_order_relaxed) };
      delete node;
      node = next;
    }
  }

  static std::size_t Hash(const T& elem) noexcept(kNothrowOnHash) { return std::hash<T>()(elem); }

  [[nodiscard]] SentinelPtr GetHead() const noexcept { return head_; }

  bool Add(const T& elem) {
    const std::size_t key{ MakeOrdinaryKey(elem) };
    Node* node{ nullptr };
    while (true) {
      const auto [pred, curr]{ Find(key, &elem) };
      if (Matches(curr, key, &elem)) {
        delete node;
        return false;
      }
      if (node == nullptr) {
        node = new Node{ elem, key, MarkableReference<Node*>{ curr, false } };
      } else {
        node->next.Store(curr, false, std::memory_order_relaxed);
      }
      if (pred->next.CompareAndSet(curr, false, node, false, std::memory_order_release)) {
        return true;
      }
    }
  }

  bool Remove(const T& elem) {
    const std::size_t key{ MakeOrdinaryKey(elem) };
    while (true) {
      const auto [pred, curr]{ Find(key, &elem) };
      if (!Matches(curr, key, &elem)) {
        return false;
      }
      Node* succ{ curr->next.Load(std::memory_order_acquire) };
      if (curr->next.AttemptMark(succ, true, std::memory_order_acq_rel)) {
        // Only attempt physical removal once, because another thread will remove the node
        // otherwise.
        pred->next.CompareAndSet(curr, false, succ, false, std::memory_order_acq_rel);
        return true;
      }
    }
  }

  // This function is wait-free because it doesn't physically remove any nodes. Traversing a marked
  // node is safe because nodes are never reclaimed and every `next` pointer leads forwards, so
  // traversal still ends at the tail sentinel.
  [[nodiscard]] bool Contains(const T& elem) const noexcept(kNothrowOnEquals && kNothrowOnHash) {
    assert(head_ != nullptr);
    const std::size_t key{ MakeOrdinaryKey(elem) };
    const Node* curr{ head_ };
    while (!AtOrPast(curr, key, &elem)) {
      curr = curr->next.Load(std::memory_order_acquire);
    }
    return Matches(curr, key, &elem) && !curr->next.IsMarked(std::memory_order_acquire);
  }

  SentinelPtr SentinelFor(std::size_t index) {
    const std::size_t key{ MakeSentinelKey(index) };
    assert(key >= head_->key && "sentinel would be spliced in out of order");
    if (head_->key == key) {
      return head_;
    }
    Node* node{ nullptr };
    while (true) {
      const auto [pred, curr]{ Find(key, nullptr) };
      if (Matches(curr, key, nullptr)) {
        delete node;
        return curr;
      }
      if (node == nullptr) {
        node = new Node{ .key{ key }, .next{ MarkableReference<Node*>{ curr, false } } };
      } else {
        node->next.Store(curr, false, std::memory_order_relaxed);
      }
      if (pred->next.CompareAndSet(curr, false, node, false, std::memory_order_release)) {
        return node;
      }
    }
  }

private:
  struct Node {
    T elem{};
    std::size_t key{ 0 };
    MarkableReference<Node*> next{ nullptr, false };
  };

  static_assert(alignof(Node) > 1, "Node must not be byte aligned");

  struct Window {
    Node* pred{ nullptr };
    Node* curr{ nullptr };
  };

  static constexpr std::size_t kTailKey{ std::numeric_limits<std::size_t>::max() };
  static constexpr std::size_t kOrdinaryKeyBit{ std::size_t{ 1 } << (kKeyBits - 1) };

  static_assert((kKeyMask & kOrdinaryKeyBit) == 0, "kKeyMask and kOrdinaryKeyBit must not overlap");
  static_assert(detail::ReverseBits(kKeyMask | kOrdinaryKeyBit) < kTailKey &&
                    detail::ReverseBits(kKeyMask) < kTailKey,
                "The key space must leave room for the tail sentinel");

  static constexpr bool kNothrowOnHash = std::is_nothrow_invocable_v<std::hash<T>, const T&>;
  static constexpr bool kNothrowOnEquals =
      std::is_nothrow_invocable_v<std::equal_to<>, const T&, const T&>;

  // Split-ordered hashing guarantees that new buckets follow the buckets that previously would
  // have contained their hash in the list, e.g. bucket 3 (mod 4), after resizing to 8, bucket 7
  // = 3 (mod 4) would immediately follow existing bucket 3. This ensures any thread doing a linear
  // scan from a hash using the previous capacity value will still find the element correctly.
  static std::size_t MakeOrdinaryKey(const T& elem) noexcept(kNothrowOnHash) {
    const std::size_t code{ Hash(elem) & kKeyMask };
    return detail::ReverseBits(code | kOrdinaryKeyBit);
  }

  static constexpr std::size_t MakeSentinelKey(std::size_t code) noexcept {
    return detail::ReverseBits(code & kKeyMask);
  }

  static bool Matches(const Node* node, std::size_t key, const T* elem) noexcept(kNothrowOnEquals) {
    return node->key == key && (elem == nullptr || node->elem == *elem);
  }

  static bool AtOrPast(const Node* node, std::size_t key,
                       const T* elem) noexcept(kNothrowOnEquals) {
    return node->key > key || Matches(node, key, elem);
  }

  Node* head_{ nullptr };

  explicit BucketList(Node* head) : head_{ head } {}

  Window Find(std::size_t key, const T* elem) noexcept(kNothrowOnEquals) {
    assert(head_ != nullptr);
  find_retry:
    Node* pred{ head_ };
    Node* curr{ head_->next.Load(std::memory_order_acquire) };
    while (true) {
      bool marked{ false };
      Node* succ{ curr->next.Load(marked, std::memory_order_acquire) };
      while (marked) {
        if (!pred->next.CompareAndSet(curr, false, succ, false, std::memory_order_acq_rel)) {
          goto find_retry;
        }
        curr = succ;
        succ = curr->next.Load(marked, std::memory_order_acquire);
      }
      if (AtOrPast(curr, key, elem)) {
        return Window{ pred, curr };
      }
      pred = curr;
      curr = succ;
    }
  }
};

#endif // BUCKETLIST_H
