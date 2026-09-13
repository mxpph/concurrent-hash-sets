#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "src/HashSetCoarseGrained.h"
#include "src/HashSetLockFree.h"
#include "src/HashSetRefinable.h"
#include "src/HashSetStriped.h"

namespace {

enum class Operation { kContains, kAdd, kRemove };

// Where an operation takes its element from.
enum class KeyChoice {
  kShared,     // Anywhere in the key range
  kDisjoint,   // Anywhere in this thread's window of the key range
  kHotSpot,    // Mostly from the small hot part of the key range
  kSequential, // This thread's window, in order, each element once
};

// Shares of a hundred operations.
struct Mix {
  unsigned contains;
  unsigned add;
  unsigned remove;
};

struct Scenario {
  const char* name;
  Mix mix;
  KeyChoice key_choice;
  std::size_t keys;
  std::size_t initial_capacity;
  std::size_t max_capacity; // only the lock-free set uses this
  std::size_t iterations;   // per thread
  bool prefill;
};

struct Step {
  Operation operation;
  std::size_t elem;
};

constexpr std::size_t kSmallKeys{ 1U << 12 };
constexpr std::size_t kSmallCapacity{ 1U << 13 };
constexpr std::size_t kLargeKeys{ 1U << 20 };
constexpr std::size_t kLargeCapacity{ 1U << 21 };
constexpr std::size_t kIterations{ 200000 };
constexpr std::size_t kOutgrownCapacity{ 1U << 2 };
constexpr std::size_t kGrowthKeys{ 1U << 19 };
constexpr std::size_t kGrowthCapacity{ 1U << 10 };
constexpr std::size_t kGrowthMaxCapacity{ 1U << 20 };
constexpr std::size_t kGrowthIterations{ 1U << 15 };

constexpr std::size_t kStepsPerThread{ 1U << 14 };
constexpr unsigned kHotKeysPercent{ 90 };
constexpr std::size_t kSeed{ 0x5DEECE66D };

constexpr std::array<int, 5> kThreadCounts{ { 1, 2, 4, 8, 16 } };

constexpr std::array<Scenario, 10> kScenarios{ {
    { .name = "contains_only",
      .mix{ .contains = 100, .add = 0, .remove = 0 },
      .key_choice = KeyChoice::kShared,
      .keys = kSmallKeys,
      .initial_capacity = kSmallCapacity,
      .max_capacity = kSmallCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "read_heavy_small",
      .mix{ .contains = 90, .add = 5, .remove = 5 },
      .key_choice = KeyChoice::kShared,
      .keys = kSmallKeys,
      .initial_capacity = kSmallCapacity,
      .max_capacity = kSmallCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "read_heavy_large",
      .mix{ .contains = 90, .add = 5, .remove = 5 },
      .key_choice = KeyChoice::kShared,
      .keys = kLargeKeys,
      .initial_capacity = kLargeCapacity,
      .max_capacity = kLargeCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "balanced_small",
      .mix{ .contains = 50, .add = 25, .remove = 25 },
      .key_choice = KeyChoice::kShared,
      .keys = kSmallKeys,
      .initial_capacity = kSmallCapacity,
      .max_capacity = kSmallCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "balanced_large",
      .mix{ .contains = 50, .add = 25, .remove = 25 },
      .key_choice = KeyChoice::kShared,
      .keys = kLargeKeys,
      .initial_capacity = kLargeCapacity,
      .max_capacity = kLargeCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "write_heavy",
      .mix{ .contains = 20, .add = 40, .remove = 40 },
      .key_choice = KeyChoice::kShared,
      .keys = kSmallKeys,
      .initial_capacity = kSmallCapacity,
      .max_capacity = kSmallCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "disjoint_keys",
      .mix{ .contains = 50, .add = 25, .remove = 25 },
      .key_choice = KeyChoice::kDisjoint,
      .keys = kLargeKeys,
      .initial_capacity = kLargeCapacity,
      .max_capacity = kLargeCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "skewed",
      .mix{ .contains = 50, .add = 25, .remove = 25 },
      .key_choice = KeyChoice::kHotSpot,
      .keys = kLargeKeys,
      .initial_capacity = kLargeCapacity,
      .max_capacity = kLargeCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "outgrown_capacity",
      .mix{ .contains = 90, .add = 5, .remove = 5 },
      .key_choice = KeyChoice::kShared,
      .keys = kLargeKeys,
      .initial_capacity = kOutgrownCapacity,
      .max_capacity = kLargeCapacity,
      .iterations = kIterations,
      .prefill = true },
    { .name = "growth",
      .mix{ .contains = 0, .add = 100, .remove = 0 },
      .key_choice = KeyChoice::kSequential,
      .keys = kGrowthKeys,
      .initial_capacity = kGrowthCapacity,
      .max_capacity = kGrowthMaxCapacity,
      .iterations = kGrowthIterations,
      .prefill = false },
} };

template <typename HashSetType> HashSetType* hash_set_under_test_{ nullptr };
std::size_t initial_size_{ 0 };
std::atomic<std::size_t> added_{ 0 };
std::atomic<std::size_t> removed_{ 0 };
bool check_failed_{ false };

// Splitmix64
[[nodiscard]] bool StartsPresent(std::size_t elem) noexcept {
  std::size_t mixed{ elem + 0x9E3779B97F4A7C15UL };
  mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9UL;
  mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBUL;
  return ((mixed ^ (mixed >> 31)) & 1U) == 0;
}

[[nodiscard]] Operation PickOperation(const Mix& mix, std::mt19937_64& generator) {
  const unsigned choice{ static_cast<unsigned>(generator() % 100) };
  if (choice < mix.contains) {
    return Operation::kContains;
  }
  if (choice < mix.contains + mix.add) {
    return Operation::kAdd;
  }
  return Operation::kRemove;
}

[[nodiscard]] std::size_t PickElem(const Scenario& scenario, std::mt19937_64& generator,
                                   std::size_t window_start, std::size_t window, std::size_t step) {
  if (scenario.key_choice == KeyChoice::kDisjoint) {
    return window_start + generator() % window;
  }
  if (scenario.key_choice == KeyChoice::kSequential) {
    return window_start + step;
  }
  if (scenario.key_choice == KeyChoice::kHotSpot) {
    const std::size_t hot_keys{ scenario.keys / 100 };
    if (generator() % 100 < kHotKeysPercent) {
      return generator() % hot_keys;
    }
    return hot_keys + generator() % (scenario.keys - hot_keys);
  }
  return generator() % scenario.keys;
}

[[nodiscard]] std::vector<Step> BuildSteps(const Scenario& scenario, std::size_t thread_index,
                                           std::size_t threads) {
  const std::size_t count{ scenario.key_choice == KeyChoice::kSequential ? scenario.iterations
                                                                         : kStepsPerThread };
  const std::size_t window{ scenario.keys / threads };
  const std::size_t window_start{ thread_index * window };

  std::mt19937_64 generator{ kSeed + thread_index };
  std::vector<Step> steps;
  steps.reserve(count);
  for (std::size_t i{ 0 }; i < count; ++i) {
    steps.push_back(Step{ .operation = PickOperation(scenario.mix, generator),
                          .elem = PickElem(scenario, generator, window_start, window, i) });
  }
  return steps;
}

template <typename HashSetType> [[nodiscard]] HashSetType* MakeHashSet(const Scenario& scenario) {
  if constexpr (std::is_same_v<HashSetType, HashSetLockFree<std::size_t>>) {
    return new HashSetType{ scenario.initial_capacity, scenario.max_capacity };
  } else {
    return new HashSetType{ scenario.initial_capacity };
  }
}

template <typename HashSetType> void SetUpRun(const Scenario& scenario) {
  hash_set_under_test_<HashSetType> = MakeHashSet<HashSetType>(scenario);
  std::size_t present{ 0 };
  if (scenario.prefill) {
    for (std::size_t elem{ 0 }; elem < scenario.keys; ++elem) {
      if (StartsPresent(elem)) {
        hash_set_under_test_<HashSetType>->Add(elem);
        ++present;
      }
    }
  }
  initial_size_ = present;
  added_.store(0, std::memory_order_relaxed);
  removed_.store(0, std::memory_order_relaxed);
}

// Runs once every thread has finished, so the counts are complete.
template <typename HashSetType> void CheckRun(const benchmark::State& state) {
  const std::size_t expected_size{ initial_size_ + added_.load(std::memory_order_relaxed) -
                                   removed_.load(std::memory_order_relaxed) };
  const std::size_t size{ hash_set_under_test_<HashSetType>->Size() };
  if (size != expected_size) {
    std::cerr << state.name() << " failed: size " << size << " does not match expected size "
              << expected_size << std::endl;
    check_failed_ = true;
  }
  delete hash_set_under_test_<HashSetType>;
  hash_set_under_test_<HashSetType> = nullptr;
}

template <typename HashSetType>
void RunScenario(benchmark::State& state, const Scenario& scenario) {
  const std::vector<Step> steps{ BuildSteps(scenario,
                                            static_cast<std::size_t>(state.thread_index()),
                                            static_cast<std::size_t>(state.threads())) };
  const std::size_t step_mask{ steps.size() - 1 };
  HashSetType& hash_set{ *hash_set_under_test_<HashSetType> };

  std::size_t index{ 0 };
  std::size_t thread_added{ 0 };
  std::size_t thread_removed{ 0 };
  for (auto _ : state) {
    const Step& step{ steps[index & step_mask] };
    ++index;
    if (step.operation == Operation::kContains) {
      benchmark::DoNotOptimize(hash_set.Contains(step.elem));
    } else if (step.operation == Operation::kAdd) {
      thread_added += hash_set.Add(step.elem) ? 1 : 0;
    } else {
      thread_removed += hash_set.Remove(step.elem) ? 1 : 0;
    }
  }

  added_.fetch_add(thread_added, std::memory_order_relaxed);
  removed_.fetch_add(thread_removed, std::memory_order_relaxed);
  state.SetItemsProcessed(state.iterations());
}

template <typename HashSetType>
void Register(const Scenario& scenario, const char* set_name, int threads) {
  const std::string name{ std::string{ scenario.name } + "/" + set_name };
  benchmark::RegisterBenchmark(name, RunScenario<HashSetType>, scenario)
      ->Threads(threads)
      ->Iterations(static_cast<benchmark::IterationCount>(scenario.iterations))
      ->UseRealTime()
      ->Setup([&scenario](const benchmark::State&) { SetUpRun<HashSetType>(scenario); })
      ->Teardown(CheckRun<HashSetType>);
}

void RegisterAll() {
  for (const Scenario& scenario : kScenarios) {
    for (const int threads : kThreadCounts) {
      Register<HashSetCoarseGrained<std::size_t>>(scenario, "coarse_grained", threads);
      Register<HashSetStriped<std::size_t>>(scenario, "striped", threads);
      Register<HashSetRefinable<std::size_t>>(scenario, "refinable", threads);
      Register<HashSetLockFree<std::size_t>>(scenario, "lock_free", threads);
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  RegisterAll();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return check_failed_ ? 1 : 0;
}
