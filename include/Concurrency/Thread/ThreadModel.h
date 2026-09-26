#pragma once
#include "Concurrency/Utils/ThreadFlowGraph.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Value.h>

namespace lotus::concurrency {
    class ThreadModel {
    public:
        ThreadModel() : m_tfg(std::make_unique<mhp::ThreadFlowGraph>()) {}
        mhp::ThreadFlowGraph& flowGraph() { return *m_tfg; }
        const mhp::ThreadFlowGraph& flowGraph() const { return *m_tfg; }

        // ====================================================================
        // Thread bookkeeping
        //
        // These maps mirror the fields currently living in MHPAnalysis. They
        // are kept as a parallel structure for now; the TFG-construction
        // extraction (processFunction, handleThreadFork, ...) will migrate the
        // implementation onto this class incrementally.
        // ====================================================================

        // Thread -> fork instruction
        std::unordered_map<mhp::ThreadID, const llvm::Instruction*>&
        threadForkSites() {
            return m_thread_fork_sites;
        }
        const std::unordered_map<mhp::ThreadID, const llvm::Instruction*>&
        threadForkSites() const {
            return m_thread_fork_sites;
        }

        // Child -> Parent
        std::unordered_map<mhp::ThreadID, mhp::ThreadID>& threadParents() {
            return m_thread_parents;
        }
        const std::unordered_map<mhp::ThreadID, mhp::ThreadID>&
        threadParents() const {
            return m_thread_parents;
        }

        // Parent -> Children
        std::unordered_map<mhp::ThreadID, std::vector<mhp::ThreadID>>&
        threadChildren() {
            return m_thread_children;
        }
        const std::unordered_map<mhp::ThreadID, std::vector<mhp::ThreadID>>&
        threadChildren() const {
            return m_thread_children;
        }

        // Fork inst -> context-specific created threads
        std::unordered_map<const llvm::Instruction*,
                           std::unordered_set<mhp::ThreadID>>&
        forkToThread() {
            return m_fork_to_thread;
        }
        const std::unordered_map<const llvm::Instruction*,
                                 std::unordered_set<mhp::ThreadID>>&
        forkToThread() const {
            return m_fork_to_thread;
        }

        // Fork node -> thread
        std::unordered_map<const mhp::SyncNode*, mhp::ThreadID>&
        forkNodeToThread() {
            return m_fork_node_to_thread;
        }
        const std::unordered_map<const mhp::SyncNode*, mhp::ThreadID>&
        forkNodeToThread() const {
            return m_fork_node_to_thread;
        }

        // Join inst -> joined thread
        std::unordered_map<const llvm::Instruction*, mhp::ThreadID>&
        joinToThread() {
            return m_join_to_thread;
        }
        const std::unordered_map<const llvm::Instruction*, mhp::ThreadID>&
        joinToThread() const {
            return m_join_to_thread;
        }

        std::unordered_set<mhp::ThreadID>& detachedThreads() {
            return m_detached_threads;
        }
        const std::unordered_set<mhp::ThreadID>& detachedThreads() const {
            return m_detached_threads;
        }

        // pthread_t value -> possible thread IDs
        std::unordered_map<const llvm::Value*,
                           std::unordered_set<mhp::ThreadID>>&
        pthreadValueToThreads() {
            return m_pthread_value_to_threads;
        }
        const std::unordered_map<const llvm::Value*,
                                 std::unordered_set<mhp::ThreadID>>&
        pthreadValueToThreads() const {
            return m_pthread_value_to_threads;
        }

        // thread ID -> pthread_t value
        std::unordered_map<mhp::ThreadID, const llvm::Value*>&
        threadToPthreadValue() {
            return m_thread_to_pthread_value;
        }
        const std::unordered_map<mhp::ThreadID, const llvm::Value*>&
        threadToPthreadValue() const {
            return m_thread_to_pthread_value;
        }

        // Multi-instance thread tracking
        std::unordered_set<mhp::ThreadID>& multiInstanceThreads() {
            return m_multi_instance_threads;
        }
        const std::unordered_set<mhp::ThreadID>& multiInstanceThreads() const {
            return m_multi_instance_threads;
        }

        // Thread ID allocation (0 is reserved for the main thread)
        mhp::ThreadID nextThreadID() const { return m_next_thread_id; }
        void setNextThreadID(mhp::ThreadID tid) { m_next_thread_id = tid; }
        mhp::ThreadID allocateThreadID() { return m_next_thread_id++; }

    private:
        std::unique_ptr<mhp::ThreadFlowGraph> m_tfg;

        // Thread bookkeeping maps (parallel to MHPAnalysis's fields).
        std::unordered_map<mhp::ThreadID, const llvm::Instruction*>
            m_thread_fork_sites; // Thread -> fork instruction
        std::unordered_map<mhp::ThreadID, mhp::ThreadID>
            m_thread_parents; // Child -> Parent
        std::unordered_map<mhp::ThreadID, std::vector<mhp::ThreadID>>
            m_thread_children; // Parent -> Children
        std::unordered_map<const llvm::Instruction*,
                           std::unordered_set<mhp::ThreadID>>
            m_fork_to_thread; // Fork inst -> context-specific created threads
        std::unordered_map<const mhp::SyncNode*, mhp::ThreadID>
            m_fork_node_to_thread;
        std::unordered_map<const llvm::Instruction*, mhp::ThreadID>
            m_join_to_thread; // Join inst -> joined thread
        std::unordered_set<mhp::ThreadID> m_detached_threads;

        // Value tracking for pthread_t variables.
        std::unordered_map<const llvm::Value*,
                           std::unordered_set<mhp::ThreadID>>
            m_pthread_value_to_threads; // pthread_t value -> possible thread IDs
        std::unordered_map<mhp::ThreadID, const llvm::Value*>
            m_thread_to_pthread_value; // thread ID -> pthread_t value

        std::unordered_set<mhp::ThreadID> m_multi_instance_threads;
        mhp::ThreadID m_next_thread_id = 1; // 0 is reserved for main thread
    };
} // namespace lotus::concurrency