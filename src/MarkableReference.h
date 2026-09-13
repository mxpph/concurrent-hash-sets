#ifndef MARKABLE_REFERENCE_H
#define MARKABLE_REFERENCE_H
#include <atomic>
#include <type_traits>

template <typename T> class MarkableReference {
public:
  static_assert(std::is_pointer_v<T> && !std::is_void_v<std::remove_pointer_t<T>>,
                "T must be a non-void pointer type");

  explicit MarkableReference(T ref = nullptr, bool marked = false) noexcept
      : packed_{ Pack(ref, marked) } {}

  T Load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return Unpack(packed_.load(order));
  }

  T Load(bool& marked, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    const uintptr_t packed{ packed_.load(order) };
    marked = (packed & kMarkBit) != 0;
    return Unpack(packed);
  }

  bool IsMarked(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return (packed_.load(order) & kMarkBit) != 0;
  }

  void Store(T ref, bool marked, std::memory_order order = std::memory_order_seq_cst) noexcept {
    packed_.store(Pack(ref, marked), order);
  }

  bool CompareAndSet(T expected_ref, bool expected_mark, T new_ref, bool new_mark,
                     std::memory_order order = std::memory_order_seq_cst) noexcept {
    uintptr_t expected{ Pack(expected_ref, expected_mark) };
    return packed_.compare_exchange_strong(expected, Pack(new_ref, new_mark), order,
                                           std::memory_order_relaxed);
  }

  bool AttemptMark(T expected_ref, bool new_mark,
                   std::memory_order order = std::memory_order_seq_cst) noexcept {
    return CompareAndSet(expected_ref, !new_mark, expected_ref, new_mark, order);
  }

private:
  static constexpr uintptr_t kMarkBit{ 1 };

  static uintptr_t Pack(T ref, bool marked) noexcept {
    return reinterpret_cast<uintptr_t>(ref) | (marked ? kMarkBit : 0);
  }

  static T Unpack(uintptr_t packed) noexcept { return reinterpret_cast<T>(packed & ~kMarkBit); }

  std::atomic<uintptr_t> packed_;
};

#endif // MARKABLE_REFERENCE_H
