#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/ParallelSolver.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/SemiSparsePropagator.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/TransferFunction.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tpa {
namespace {

// A persistent team avoids creating operating-system threads for every small
// transfer. Publication happens only after the entire evaluation batch drains.
class TransferPool {
  std::mutex mutex;
  std::condition_variable work, done;
  std::vector<std::thread> workers;
  std::function<void(std::size_t)> function;
  std::atomic<std::size_t> next{0};
  std::atomic<unsigned> active{0}, peak{0};
  std::size_t count = 0, generation = 0, pending = 0;
  bool stop = false;
  std::exception_ptr failure;

  void evaluate() {
    while (true) {
      const auto index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= count)
        return;
      const auto running = active.fetch_add(1) + 1;
      auto maximum = peak.load();
      while (maximum < running &&
             !peak.compare_exchange_weak(maximum, running)) {
      }
      try {
        function(index);
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure)
          failure = std::current_exception();
      }
      active.fetch_sub(1);
    }
  }

public:
  explicit TransferPool(unsigned threads) {
    try {
      for (unsigned i = 1; i < threads; ++i)
        workers.emplace_back([&] {
          std::size_t seen = 0;
          while (true) {
            {
              std::unique_lock<std::mutex> lock(mutex);
              work.wait(lock, [&] { return stop || generation != seen; });
              if (stop)
                return;
              seen = generation;
            }
            evaluate();
            {
              std::lock_guard<std::mutex> lock(mutex);
              --pending;
            }
            done.notify_one();
          }
        });
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~TransferPool() { shutdown(); }
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    work.notify_all();
    for (auto &thread : workers)
      if (thread.joinable())
        thread.join();
  }
  void run(std::size_t jobs, std::function<void(std::size_t)> callback) {
    // Small dependency chains frequently expose only one new transfer. Avoid
    // waking the whole team for that case; cached predictions remain reusable.
    if (jobs == 1) {
      callback(0);
      if (!peak.load())
        peak = 1;
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      function = std::move(callback);
      count = jobs;
      next = 0;
      failure = {};
      pending = workers.size();
      ++generation;
    }
    work.notify_all();
    evaluate();
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [&] { return pending == 0; });
    if (failure)
      std::rethrow_exception(failure);
  }
  unsigned peakWorkers() const { return peak.load(); }
};

struct Candidate {
  ProgramPoint point;
  PointerManager pointers;
  Env environment;
  GlobalState state;
  std::shared_ptr<const Store> input;
  std::unique_ptr<EvalResult> result;
  std::size_t graphRevision;
  bool usesGraph;

  Candidate(GlobalState &base, Memo &memo, const ProgramPoint &pp)
      : point(pp), pointers(base.getPointerManager()),
        environment(base.getEnv().makeOverlay()),
        state(pointers, base.getMemoryManager(), base.getSemiSparseProgram(),
              base.getExternalPointerTable(), environment),
        graphRevision(base.getCallGraph().getRevision()),
        usesGraph(pp.getCFGNode()->isCallNode() ||
                  pp.getCFGNode()->isReturnNode()) {
    if (usesGraph)
      state.getCallGraph() = base.getCallGraph().makeOverlay();
    input = memo.snapshot(pp);
  }
  void evaluate() {
    result = std::make_unique<EvalResult>(
        TransferFunction(state, input.get()).eval(point));
  }
  bool valid(GlobalState &base, Memo &memo) const {
    if (!environment.validateOverlay(base.getEnv()) || !pointers.validateView())
      return false;
    const Store *current = memo.lookup(point);
    if (current != input.get())
      return false;
    return !usesGraph || graphRevision == base.getCallGraph().getRevision();
  }
  void commit(GlobalState &base) {
    pointers.publishView();
    environment.commitOverlayTo(base.getEnv());
    if (usesGraph)
      state.getCallGraph().commitOverlayTo(base.getCallGraph());
  }
};
} // namespace

void solveParallel(GlobalState &state, Memo &memo, ForwardWorkList worklist,
                   SemiSparsePointerAnalysis::Config config,
                   SemiSparsePointerAnalysis::Statistics &stats) {
  if (!config.lookahead)
    throw std::invalid_argument("TPA parallel lookahead must be positive");
  const unsigned threads =
      config.threads ? config.threads
                     : std::max(1U, std::thread::hardware_concurrency());
  stats.threads = threads;
  TransferPool pool(threads);
  const std::size_t window =
      static_cast<std::size_t>(threads) * config.lookahead;
  std::unordered_map<ProgramPoint, std::unique_ptr<Candidate>> cache;
  auto isTopLevel = [](const ProgramPoint &point) {
    const auto *node = point.getCFGNode();
    return node->isAllocNode() || node->isCopyNode() || node->isOffsetNode();
  };
  auto retireTopLevel = [&] {
    const auto point = worklist.dequeue();
    auto result = TransferFunction(state, memo.lookup(point)).eval(point);
    SemiSparsePropagator(memo, worklist).propagate(result);
    ++stats.transfers;
    ++stats.evaluations;
  };
  while (!worklist.empty()) {
    // These small Env operations feed later memory transfers. Evaluating them
    // directly avoids snapshots and wakeups and makes their inputs available
    // before predicting work. No worker publishes or evaluates during this
    // step.
    if (isTopLevel(worklist.front())) {
      retireTopLevel();
      continue;
    }
    // Preserve both the function/context FIFO and each local priority queue.
    // Prediction may be wrong when retirement enqueues a higher-priority node.
    const auto points = worklist.peek(window);
    std::unordered_set<ProgramPoint> predicted;
    for (auto point : points)
      if (!isTopLevel(point))
        predicted.insert(point);
    // A newly enqueued successor may preempt earlier predictions without
    // changing their inputs. Keep those evaluations until they reach the head.
    // Evict outside the current window to bound retained Store snapshots.
    for (auto it = cache.begin(); it != cache.end();) {
      if (!predicted.count(it->first)) {
        it = cache.erase(it);
        ++stats.discarded;
      } else {
        ++it;
      }
    }
    std::vector<std::unique_ptr<Candidate>> candidates;
    for (auto point : points)
      if (!isTopLevel(point) && !cache.count(point))
        candidates.push_back(std::make_unique<Candidate>(state, memo, point));
    ++stats.batches;
    pool.run(candidates.size(),
             [&](std::size_t index) { candidates[index]->evaluate(); });
    stats.evaluations += candidates.size();
    for (auto &candidate : candidates) {
      auto point = candidate->point;
      cache.emplace(point, std::move(candidate));
    }
    while (!worklist.empty()) {
      const auto point = worklist.front();
      if (isTopLevel(point)) {
        retireTopLevel();
        continue;
      }
      const auto found = cache.find(point);
      if (found == cache.end())
        break;
      auto candidate = std::move(found->second);
      cache.erase(found);
      if (!candidate->valid(state, memo)) {
        ++stats.retries;
        ++stats.evaluations;
        candidate = std::make_unique<Candidate>(state, memo, point);
        candidate->evaluate();
      }
      // No worker or later transfer publishes while a stale head is replayed.
      worklist.dequeue();
      candidate->commit(state);
      SemiSparsePropagator(memo, worklist).propagate(*candidate->result);
      ++stats.transfers;
    }
  }
  stats.discarded += cache.size();
  stats.peakWorkers = pool.peakWorkers();
}
} // namespace tpa
