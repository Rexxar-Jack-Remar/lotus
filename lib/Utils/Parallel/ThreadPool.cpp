//===- ThreadPool.cpp - Multi-threaded task scheduling
//---------------------===//
//
// This file is distributed under the MIT License. See LICENSE for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// \brief Thread pool implementation for parallel task execution
///
/// This file implements the ThreadPool class which manages worker threads
/// for parallel task execution. Tasks are enqueued and processed by available
/// workers.
///
/// Features:
/// - Dynamic task enqueueing
/// - Thread-local storage support
/// - Wait synchronization for task completion
///===----------------------------------------------------------------------===//

#include "Utils/Parallel/ThreadPool.h"

#include <chrono>
#include <mutex>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

// Command line option for specifying the number of worker threads.tr
static cl::opt<unsigned>
    NumWorkers("nworkers",
               cl::desc("Specify the number of workers to perform analysis. "
                        "Default is 0, which runs tasks on the main thread "
                        "without launching worker threads."),
               cl::value_desc("num of workers"), cl::init(0));

// Global thread pool instance and thread-safe initialization.
static std::once_flag ThreadPoolOnce;
static ThreadPool *Threads = nullptr;

/// Hook functions to run at the beginning and end of a thread
/// @{
void (*before_thread_start_hook)() = nullptr;
void (*after_thread_complete_hook)() = nullptr;
/// @}

// Returns the global thread pool instance, creating it if necessary.
ThreadPool *ThreadPool::get() {
  std::call_once(ThreadPoolOnce, []() { Threads = new ThreadPool; });
  return Threads;
}

// Constructs the thread pool and launches worker threads.
ThreadPool::ThreadPool() : IsStop(false) {
  unsigned NCores = std::thread::hardware_concurrency();
  if (NumWorkers == 0) {
    // We do not fork any threads, just use the main thread
  } else if (NumWorkers > NCores) {
    errs() << "Warning: requested " << NumWorkers
           << " workers but only detected " << NCores
           << " hardware threads; oversubscription may degrade performance.\n";
  }

  NumRunningTask = 0;

  for (unsigned I = 0; I < NumWorkers.getValue(); ++I) {
    Workers.emplace_back([this] {
      if (before_thread_start_hook)
        before_thread_start_hook();

      for (;;) {
        std::function<void()> Task;

        {
          std::unique_lock<std::mutex> Lock(this->QueueMutex);
          this->Condition.wait(Lock, [this] {
            return this->IsStop || !this->TaskQueue.empty();
          });
          // If ThreadPool already stopped, return without checking
          // tasks.
          if (this->IsStop) { // && this->tasks.empty())
            if (after_thread_complete_hook)
              after_thread_complete_hook();
            return;
          }
          if (!this->TaskQueue.empty()) {
            Task = std::move(this->TaskQueue.front());
            this->TaskQueue.pop();
          }

          NumRunningTask++;
        }

        Task();

        {
          std::unique_lock<std::mutex> Lock(this->QueueMutex);
          NumRunningTask--;
          if (TaskQueue.empty() && NumRunningTask == 0)
            Condition.notify_all();
        }
      }
    });

    ThreadLocals[Workers.back().get_id()] = nullptr;
    WorkerIds.insert(Workers.back().get_id());
  }
}

bool ThreadPool::runOnePendingTaskOrWait() {
  std::function<void()> Task;
  {
    std::unique_lock<std::mutex> Lock(QueueMutex);
    if (TaskQueue.empty()) {
      if (NumRunningTask == 0 || IsStop)
        return false;
      Condition.wait_for(Lock, std::chrono::milliseconds(1), [this] {
        return IsStop || !TaskQueue.empty() ||
               (TaskQueue.empty() && NumRunningTask == 0);
      });
      if (TaskQueue.empty() || IsStop)
        return false;
    }

    Task = std::move(TaskQueue.front());
    TaskQueue.pop();
    ++NumRunningTask;
  }

  Task();

  {
    std::unique_lock<std::mutex> Lock(QueueMutex);
    --NumRunningTask;
    if (TaskQueue.empty() && NumRunningTask == 0)
      Condition.notify_all();
    else
      Condition.notify_one();
  }

  return true;
}

// Waits for all tasks to complete.
void ThreadPool::wait() {
  if (!hasWorkers())
    return;

  if (!isWorkerThread()) {
    std::unique_lock<std::mutex> Lock(this->QueueMutex);
    Condition.wait(Lock,
                   [this] { return TaskQueue.empty() && NumRunningTask == 0; });
    return;
  }

  while (true) {
    {
      std::unique_lock<std::mutex> Lock(this->QueueMutex);
      if (TaskQueue.empty() && NumRunningTask == 0)
        return;
    }

    if (!runOnePendingTaskOrWait()) {
      std::unique_lock<std::mutex> Lock(this->QueueMutex);
      if (TaskQueue.empty() && NumRunningTask == 0)
        return;
      Condition.wait_for(Lock, std::chrono::milliseconds(1));
    }
  }
}

// Destructor joins all worker threads.
ThreadPool::~ThreadPool() {
  wait();
  {
    std::unique_lock<std::mutex> Lock(QueueMutex);
    IsStop = true;
  }
  Condition.notify_all();
  for (std::thread &Worker : Workers) {
    Worker.join();
  }
}
