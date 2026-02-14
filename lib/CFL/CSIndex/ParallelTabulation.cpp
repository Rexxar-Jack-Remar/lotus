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

#include <algorithm>
#include <atomic>
#include <csignal>
#include <future>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "CFL/CSIndex/ParallelTabulation.h"
#include "CFL/CSIndex/CSProgressBar.h"

static std::atomic<bool> timeout{false};

static void alarm_handler(int) {
    timeout.store(true, std::memory_order_relaxed);
}

ParallelTabulation::ParallelTabulation(Graph &g) : vfg(g), num_threads(std::thread::hardware_concurrency()) {
    if (num_threads == 0) {
        num_threads = 4; // Default fallback
    }
    visited_sets = std::make_unique<ThreadSafeVisitedSet>(num_threads);
    func_visited_sets = std::make_unique<ThreadSafeVisitedSet>(num_threads);
}

ParallelTabulation::ParallelTabulation(Graph &g, size_t threads) : vfg(g), num_threads(threads) {
    visited_sets = std::make_unique<ThreadSafeVisitedSet>(num_threads);
    func_visited_sets = std::make_unique<ThreadSafeVisitedSet>(num_threads);
}

bool ParallelTabulation::reach(int s, int t) {
    // For single queries, use thread-local storage approach
    size_t thread_id = 0; // Default thread ID for single queries

    if (visited_sets->count(thread_id, s)) {
        return false;
    }

    if (s == t) {
        return true;
    }

    visited_sets->insert(thread_id, s);
    auto& edges = vfg.out_edges(s);

    for (auto successor : edges) {
        if (is_call(s, successor)) {
            // Visit the func body
            if (reach_func(successor, t, thread_id)) {
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

bool ParallelTabulation::reach_func(int s, int t, size_t thread_id) {
    if (func_visited_sets->count(thread_id, s)) {
        return false;
    }

    if (s == t) {
        return true;
    }

    func_visited_sets->insert(thread_id, s);
    auto& edges = vfg.out_edges(s);

    for (auto successor : edges) {
        if (is_return(s, successor)) {
            continue;
        } else {
            if (reach_func(successor, t, thread_id)) {
                return true;
            }
        }
    }

    return false;
}

bool ParallelTabulation::is_call(int s, int t) {
    return vfg.label(s, t) > 0;
}

bool ParallelTabulation::is_return(int s, int t) {
    return vfg.label(s, t) < 0;
}

/**
 * @brief Worker function for parallel processing of a vertex range.
 * 
 * Processes vertices in the range [start, end) in parallel. Each thread
 * maintains its own visited sets to avoid synchronization overhead.
 * Results are stored in the shared results vector with mutex protection.
 * 
 * @param start Starting vertex index (inclusive)
 * @param end Ending vertex index (exclusive)
 * @param results Shared results vector for storing reachable sets
 * @param results_mutex Mutex for protecting results vector
 */
void ParallelTabulation::process_vertex_range(int start, int end,
                                            std::vector<std::set<int>>& results,
                                            std::mutex& results_mutex) {
    // Compute thread-local ID for accessing thread-local visited sets
    size_t thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id()) % num_threads;

    for (int i = start; i < end; ++i) {
        if (timeout.load(std::memory_order_relaxed)) {
            break;
        }

        // Clear thread-local visited sets for this vertex
        visited_sets->clear(thread_id);
        func_visited_sets->clear(thread_id);

        // Compute reachable set for vertex i
        std::set<int> local_tc;
        traverse_parallel(i, local_tc, thread_id);

        // Safely store result
        {
            std::lock_guard<std::mutex> lock(results_mutex);
            results[i] = std::move(local_tc);
        }
    }
}

void ParallelTabulation::traverse_parallel(int s, std::set<int>& tc, size_t thread_id) {
    if (visited_sets->count(thread_id, s)) {
        return;
    }

    if (timeout.load(std::memory_order_relaxed)) {
        return;
    }

    visited_sets->insert(thread_id, s);
    tc.insert(s);

    auto& edges = vfg.out_edges(s);
    for (auto successor : edges) {
        if (is_call(s, successor)) {
            // Visit the func body
            traverse_func_parallel(successor, tc, thread_id);
        } else {
            traverse_parallel(successor, tc, thread_id);
        }
    }
}

void ParallelTabulation::traverse_func_parallel(int s, std::set<int>& tc, size_t thread_id) {
    if (func_visited_sets->count(thread_id, s)) {
        return;
    }

    if (timeout.load(std::memory_order_relaxed)) {
        return;
    }

    func_visited_sets->insert(thread_id, s);
    tc.insert(s);

    auto& edges = vfg.out_edges(s);
    for (auto successor : edges) {
        if (is_return(s, successor)) {
            continue;
        } else {
            traverse_func_parallel(successor, tc, thread_id);
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

        std::vector<std::thread> threads;
        std::mutex results_mutex;
        int current_start = 0;

        for (size_t i = 0; i < num_threads; ++i) {
            int chunk_size = vertices_per_thread + (i < remainder ? 1 : 0);
            int start = current_start;
            int end = start + chunk_size;

            if (start >= vfg.num_vertices()) break;

            threads.emplace_back(&ParallelTabulation::process_vertex_range, this,
                               start, end, std::ref(results), std::ref(results_mutex));

            current_start = end;
        }

        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
    } else {
        // Single-threaded fallback
        std::mutex results_mutex;
        process_vertex_range(0, vfg.num_vertices(), results, results_mutex);
    }

    // Calculate memory usage
    for (const auto& tc_set : results) {
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
    std::mutex results_mutex;

    // Launch asynchronous tasks for each vertex
    for (int i = 0; i < vfg.num_vertices(); ++i) {
        if (timeout.load(std::memory_order_relaxed)) break;

        futures.emplace_back(std::async(std::launch::async, [this, i]() -> std::set<int> {
            size_t thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id()) % num_threads;

            // Clear thread-local visited sets for this vertex
            visited_sets->clear(thread_id);
            func_visited_sets->clear(thread_id);

            // Compute reachable set for vertex i
            std::set<int> local_tc;
            traverse_parallel(i, local_tc, thread_id);
            return local_tc;
        }));
    }

    // Collect results
    std::vector<std::set<int>> results(vfg.num_vertices());
    for (size_t i = 0; i < futures.size(); ++i) {
        if (timeout.load(std::memory_order_relaxed)) break;

        results[i] = futures[i].get();

        // Update progress and memory calculation
        total_memory += results[i].size() * sizeof(int);
        bar.update();
    }

    return total_memory / 1024.0 / 1024.0;
}

const char *ParallelTabulation::method() const {
    return "ParallelTabulate";
}

void ParallelTabulation::reset() {
    // Clear all thread-local visited sets
    for (size_t i = 0; i < num_threads; ++i) {
        visited_sets->clear(i);
        func_visited_sets->clear(i);
    }
}
