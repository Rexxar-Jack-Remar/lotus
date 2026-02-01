/*
 * IDE Solver
 *
 * This header provides the IDE (Interprocedural Distributive Environment)
 * solver for the IFDS framework with:
 * - Summary edge reuse for efficient interprocedural analysis
 * - Edge function composition memoization
 */

#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "Dataflow/IFDS/IFDSFramework.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifds {

// ============================================================================
// IDE Solver
// ============================================================================

template<typename Problem>
class IDESolver {
public:
    using Fact = typename Problem::FactType;
    using FactSet = typename Problem::FactSet;
    using Value = typename Problem::ValueType;
    using EdgeFunction = typename Problem::EdgeFunction;
    using EdgeFunctionPtr = std::shared_ptr<EdgeFunction>;
    using PathEdgeType = PathEdge<Fact>;
    using PathEdgeHashType = PathEdgeHash<Fact>;

    IDESolver(Problem& problem);

    void solve(const llvm::Module& module);

    // Bounded solver: optional step limit (0 = unbounded). When the bound is reached,
    // the solver stops and returns a partial result.
    void set_max_steps(size_t max_steps) { m_max_steps = max_steps; }
    size_t get_max_steps() const { return m_max_steps; }
    size_t get_steps_performed() const { return m_steps_performed; }
    bool bound_reached() const { return m_bound_reached; }

    // Query interface
    Value get_value_at(const llvm::Instruction* inst, const Fact& fact) const;
    const std::unordered_map<const llvm::Instruction*,
                            std::unordered_map<Fact, Value>>& get_all_values() const;

private:
    struct StartKey {
        const llvm::Instruction* start_node;
        Fact start_fact;

        bool operator==(const StartKey& other) const {
            return start_node == other.start_node && start_fact == other.start_fact;
        }
    };

    struct StartKeyHash {
        size_t operator()(const StartKey& key) const {
            size_t h1 = std::hash<const llvm::Instruction*>{}(key.start_node);
            size_t h2 = std::hash<Fact>{}(key.start_fact);
            return h1 ^ (h2 << 1);
        }
    };

    struct IncomingEdge {
        const llvm::CallInst* call;
        Fact call_fact;
        const llvm::Instruction* start_node;
        Fact start_fact;
        EdgeFunctionPtr caller_phi;

        bool operator==(const IncomingEdge& other) const {
            return call == other.call &&
                   call_fact == other.call_fact &&
                   start_node == other.start_node &&
                   start_fact == other.start_fact &&
                   caller_phi == other.caller_phi;
        }
    };

    // Composition cache key
    struct ComposePair {
        EdgeFunctionPtr f1;
        EdgeFunctionPtr f2;

        bool operator==(const ComposePair& other) const {
            return f1 == other.f1 && f2 == other.f2;
        }
    };

    struct ComposePairHash {
        size_t operator()(const ComposePair& cp) const {
            return std::hash<EdgeFunctionPtr>{}(cp.f1) ^
                   (std::hash<EdgeFunctionPtr>{}(cp.f2) << 1);
        }
    };

    // Helper: memoized composition
    EdgeFunctionPtr compose_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2);

    // Helper: create shared pointer to edge function
    EdgeFunctionPtr make_edge_function(const EdgeFunction& ef);

    Problem& m_problem;

    // Bounded solver state (0 = unbounded)
    size_t m_max_steps = 0;
    size_t m_steps_performed = 0;
    bool m_bound_reached = false;

    // Results: instruction -> fact -> value
    std::unordered_map<const llvm::Instruction*, std::unordered_map<Fact, Value>> m_values;

    // Jump functions: path edge -> edge functions
    std::unordered_map<PathEdgeType, std::vector<EdgeFunctionPtr>, PathEdgeHashType> m_jump_functions;

    // Incoming call edges for each callee start fact
    std::unordered_map<StartKey, std::vector<IncomingEdge>, StartKeyHash> m_incoming;

    // End summaries per callee start fact: exit_fact -> edge functions
    std::unordered_map<StartKey,
                       std::unordered_map<Fact, std::vector<EdgeFunctionPtr>>,
                       StartKeyHash>
        m_end_summaries;

    // Composition memoization table
    std::unordered_map<ComposePair, EdgeFunctionPtr, ComposePairHash> m_compose_cache;

    // Worklist of path edges with edge functions
    std::vector<std::pair<PathEdgeType, EdgeFunctionPtr>> m_worklist;
};

} // namespace ifds

#include "Dataflow/IFDS/Solvers/IDESolver.tpp"
