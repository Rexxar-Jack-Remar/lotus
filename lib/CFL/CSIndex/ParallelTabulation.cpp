/**
 * @file ParallelTabulation.cpp
 * @brief Parallel implementation of tabulation-based CFL reachability.
 *
 * This module provides a multi-threaded version of the tabulation algorithm
 * for computing CFL reachability. It parallelizes the computation across
 * multiple vertices, with each thread maintaining its own visited sets
 * to avoid synchronization overhead.
 *
 * Key features:
 * - Thread-safe visited sets: Each thread has its own visited tracking
 * - Work distribution: Vertices are divided among threads
 * - Two parallelization strategies:
 *   1. Thread-based: Divide vertices into chunks for each thread
 *   2. Async-based: Launch async tasks for each vertex (better load balancing)
 *
 * The parallel version maintains the same correctness guarantees as the
 * sequential Tabulation class while providing significant speedup on
 * multi-core systems.
 *
 * Thread safety: Uses thread-local visited sets to avoid contention.
 */

#include "CFL/CSIndex/ParallelTabulation.h"

#include "CFL/CSIndex/CSProgressBar.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <future>
#include <vector>

#include <unistd.h>

static std::atomic<bool> timeout{false};

static void alarm_handler(int) {
  timeout.store(true, std::memory_order_relaxed);
}

ParallelTabulation::ParallelTabulation(Graph &g)
    : vfg(g), num_threads(std::thread::hardware_concurrency()) {
  if (num_threads == 0) {
    num_threads = 4; // Default fallback
  }
}

ParallelTabulation::ParallelTabulation(Graph &g, size_t threads)
    : vfg(g), num_threads(threads ? threads : 1) {}

bool ParallelTabulation::reach(int s, int t) {
  std::set<int> visited;
  std::set<int> func_visited;

  if (visited.count(s) != 0) {
    return false;
  }

  if (s == t) {
    return true;
  }

  visited.insert(s);
  auto &edges = vfg.out_edges(s);

  for (auto successor : edges) {
    if (is_call(s, successor)) {
      // Visit the func body
      if (reach_func(successor, t, func_visited)) {
        return true;
      }
    } else {
      if (reach(successor, t)) {
        return true;
      }
    }
  }

  return false;
}

bool ParallelTabulation::reach_func(int s, int t, std::set<int> &visited) {
  if (visited.count(s) != 0) {
    return false;
  }

  if (s == t) {
    return true;
  }

  visited.insert(s);
  auto &edges = vfg.out_edges(s);

  for (auto successor : edges) {
    if (is_return(s, successor)) {
      continue;
    } else {
      if (reach_func(successor, t, visited)) {
        return true;
      }
    }
  }

  return false;
}

bool ParallelTabulation::is_call(int s, int t) { return vfg.label(s, t) > 0; }

bool ParallelTabulation::is_return(int s, int t) { return vfg.label(s, t) < 0; }

/**
 * @brief Worker function for parallel processing of a vertex range.
 *
 * Processes vertices in the range [start, end) in parallel. Each thread
 * maintains its own visited sets to avoid synchronization overhead.
 * Results are written directly into disjoint slots of the shared results vector.
 *
 * @param start Starting vertex index (inclusive)
 * @param end Ending vertex index (exclusive)
 * @param results Shared results vector for storing reachable sets
 */
void ParallelTabulation::process_vertex_range(int start, int end,
                                              std::vector<std::set<int>> &results) {
  for (int i = start; i < end; ++i) {
    if (timeout.load(std::memory_order_relaxed)) {
      break;
    }

    // Compute reachable set for vertex i
    std::set<int> local_tc;
    std::set<int> visited;
    std::set<int> func_visited;
    traverse_parallel(i, local_tc, visited, func_visited);
    results[i] = std::move(local_tc);
  }
}

void ParallelTabulation::traverse_parallel(int s, std::set<int> &tc,
                                           std::set<int> &visited,
                                           std::set<int> &func_visited) {
  if (visited.count(s) != 0) {
    return;
  }

  if (timeout.load(std::memory_order_relaxed)) {
    return;
  }

  visited.insert(s);
  tc.insert(s);

  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_call(s, successor)) {
      // Visit the func body
      traverse_func_parallel(successor, tc, func_visited);
    } else {
      traverse_parallel(successor, tc, visited, func_visited);
    }
  }
}

void ParallelTabulation::traverse_func_parallel(int s, std::set<int> &tc,
                                                std::set<int> &visited) {
  if (visited.count(s) != 0) {
    return;
  }

  if (timeout.load(std::memory_order_relaxed)) {
    return;
  }

  visited.insert(s);
  tc.insert(s);

  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_return(s, successor)) {
      continue;
    } else {
      traverse_func_parallel(successor, tc, visited);
    }
  }
}

double ParallelTabulation::tc() {
  signal(SIGALRM, alarm_handler);
  timeout.store(false, std::memory_order_relaxed);
  alarm(3600 * 6);

  CSProgressBar bar(vfg.num_vertices());

  double total_memory = 0;
  std::vector<std::set<int>> results(vfg.num_vertices());

  // Use parallel processing for the main computation
  if (num_threads > 1) {
    // Divide work among threads
    int vertices_per_thread = vfg.num_vertices() / num_threads;
    int remainder = vfg.num_vertices() % num_threads;

    std::vector<std::future<void>> tasks;
    int current_start = 0;

    for (size_t i = 0; i < num_threads; ++i) {
      int chunk_size = vertices_per_thread + (i < remainder ? 1 : 0);
      int start = current_start;
      int end = start + chunk_size;

      if (start >= vfg.num_vertices())
        break;

      tasks.emplace_back(ThreadPool::get()->enqueue(
          [this, start, end, &results]() {
            process_vertex_range(start, end, results);
          }));

      current_start = end;
    }

    for (auto &task : tasks)
      task.get();
  } else {
    // Single-threaded fallback
    process_vertex_range(0, vfg.num_vertices(), results);
  }

  // Calculate memory usage
  for (const auto &tc_set : results) {
    total_memory += tc_set.size() * sizeof(int);
  }

  bar.update();

  return total_memory / 1024.0 / 1024.0;
}

// Alternative implementation using async/future for better load balancing
double ParallelTabulation::tc_async() {
  signal(SIGALRM, alarm_handler);
  timeout.store(false, std::memory_order_relaxed);
  alarm(3600 * 6);

  CSProgressBar bar(vfg.num_vertices());

  double total_memory = 0;
  std::vector<std::future<std::set<int>>> futures;

  // Launch asynchronous tasks for each vertex
  for (int i = 0; i < vfg.num_vertices(); ++i) {
    if (timeout.load(std::memory_order_relaxed))
      break;

    futures.emplace_back(ThreadPool::get()->enqueue([this, i]() -> std::set<int> {
      std::set<int> local_tc;
      std::set<int> visited;
      std::set<int> func_visited;
      traverse_parallel(i, local_tc, visited, func_visited);
      return local_tc;
    }));
  }

  // Collect results
  std::vector<std::set<int>> results(vfg.num_vertices());
  for (size_t i = 0; i < futures.size(); ++i) {
    if (timeout.load(std::memory_order_relaxed))
      break;

    results[i] = futures[i].get();

    // Update progress and memory calculation
    total_memory += results[i].size() * sizeof(int);
    bar.update();
  }

  return total_memory / 1024.0 / 1024.0;
}

const char *ParallelTabulation::method() const { return "ParallelTabulate"; }

void ParallelTabulation::reset() {}
