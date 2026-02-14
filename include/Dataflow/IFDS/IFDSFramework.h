/*
 * IFDS/IDE Framework
 * 
 * This header provides a comprehensive IFDS/IDE framework
 */

#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Utils/ADT/ThreadSafe.h"

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declaration
namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace ifds {

// Import thread-safe data structures from Support/ADT
using lotus::SimpleOptional;
using lotus::ThreadSafeSet;
using lotus::ThreadSafeMap;
using lotus::ThreadSafeVector;
using lotus::ShardedMap;

// ============================================================================
// Forward Declarations
// ============================================================================

template<typename Fact> class IFDSProblem;
template<typename Fact, typename Value> class IDEProblem;
template<typename Fact> class ExplodedSupergraph;

// ============================================================================
// IFDS Core Data Structures
// ============================================================================

// Helper for fact comparison so clients can specialize (e.g. for types without
// visible operator< at template instantiation).
template <typename Fact>
bool fact_less(const Fact &a, const Fact &b) {
  return a < b;
}

template<typename Fact>
struct PathEdge {
    const llvm::Instruction* start_node;
    Fact start_fact;
    const llvm::Instruction* target_node;
    Fact target_fact;
    
    PathEdge(const llvm::Instruction* s_node, const Fact& s_fact,
             const llvm::Instruction* t_node, const Fact& t_fact)
        : start_node(s_node), start_fact(s_fact), target_node(t_node), target_fact(t_fact) {}
    
    bool operator==(const PathEdge& other) const {
        return start_node == other.start_node && target_node == other.target_node &&
               start_fact == other.start_fact && target_fact == other.target_fact;
    }
    bool operator<(const PathEdge& other) const {
        if (start_node != other.start_node) return start_node < other.start_node;
        if (target_node != other.target_node) return target_node < other.target_node;
        if (start_fact != other.start_fact) return fact_less(start_fact, other.start_fact);
        return fact_less(target_fact, other.target_fact);
    }
};

template<typename Fact>
struct PathEdgeHash {
    size_t operator()(const PathEdge<Fact>& edge) const {
        size_t h1 = std::hash<const llvm::Instruction*>{}(edge.start_node);
        size_t h2 = std::hash<const llvm::Instruction*>{}(edge.target_node);
        size_t h3 = std::hash<Fact>{}(edge.start_fact);
        size_t h4 = std::hash<Fact>{}(edge.target_fact);
        return ((h1 ^ (h2 << 1)) ^ (h3 << 2)) ^ (h4 << 3);
    }
};

template<typename Fact>
struct SummaryEdge {
    const llvm::CallBase* call_site;
    Fact call_fact;
    Fact return_fact;
    
    SummaryEdge(const llvm::CallBase* call, const Fact& c_fact, const Fact& r_fact)
        : call_site(call), call_fact(c_fact), return_fact(r_fact) {}
    
    bool operator==(const SummaryEdge& other) const {
        return call_site == other.call_site && call_fact == other.call_fact && return_fact == other.return_fact;
    }
    bool operator<(const SummaryEdge& other) const {
        if (call_site != other.call_site) return call_site < other.call_site;
        if (call_fact != other.call_fact) return fact_less(call_fact, other.call_fact);
        return fact_less(return_fact, other.return_fact);
    }
};

template<typename Fact>
struct SummaryEdgeHash {
    size_t operator()(const SummaryEdge<Fact>& edge) const {
        size_t h1 = std::hash<const llvm::CallBase*>{}(edge.call_site);
        size_t h2 = std::hash<Fact>{}(edge.call_fact);
        size_t h3 = std::hash<Fact>{}(edge.return_fact);
        return (h1 ^ (h2 << 1)) ^ (h3 << 2);
    }
};

// ============================================================================
// Initial Seeds Representation
// ============================================================================

template<typename Fact>
struct InitialSeeds {
    using FactSet = std::set<Fact>;
    using SeedMap = std::unordered_map<const llvm::Instruction*, FactSet>;

    void add_seed(const llvm::Instruction* inst, const Fact& fact) {
        seeds[inst].insert(fact);
    }

    void add_seed(const llvm::Instruction* inst, const FactSet& facts) {
        auto& set = seeds[inst];
        set.insert(facts.begin(), facts.end());
    }

    const SeedMap& get_seeds() const { return seeds; }
    bool empty() const { return seeds.empty(); }

    SeedMap seeds;
};

// ============================================================================
// IFDS Problem Interface
// ============================================================================

template<typename Fact>
class IFDSProblem {
public:
    using FactType = Fact;
    using FactSet = std::set<Fact>;
    using InitialSeeds = ifds::InitialSeeds<Fact>;
    
    virtual ~IFDSProblem() = default;
    
    // Zero fact (lambda in IFDS terminology)
    virtual Fact zero_fact() const = 0;
    
    // Flow functions for different statement types
    virtual FactSet normal_flow(const llvm::Instruction* stmt, const Fact& fact) = 0;
    virtual FactSet call_flow(const llvm::CallBase* call, const llvm::Function* callee, const Fact& fact) = 0;
    virtual FactSet return_flow(const llvm::CallBase* call, const llvm::Function* callee, 
                               const Fact& exit_fact, const Fact& call_fact) = 0;
    virtual FactSet call_to_return_flow(const llvm::CallBase* call, const Fact& fact) = 0;
    
    // Initial facts at program entry
    virtual FactSet initial_facts(const llvm::Function* main) = 0;

    // Optional initial seeds override (multiple entry points)
    virtual InitialSeeds initial_seeds(const llvm::Module& module);

    // Zero-fact handling (auto-add and identity preservation)
    virtual bool auto_add_zero() const { return true; }
    virtual bool is_zero_fact(const Fact& fact) const { return fact == zero_fact(); }
    
    // Alias analysis integration
    virtual void set_alias_analysis(lotus::AliasAnalysisWrapper* aa);
    bool has_alias_analysis_configured() const;
    
    // Helper methods for common operations
    virtual bool is_source(const llvm::Instruction* inst) const;
    virtual bool is_sink(const llvm::Instruction* inst) const;
    
protected:
    lotus::AliasAnalysisWrapper* m_alias_analysis = nullptr;
    
    // Alias analysis helper using AliasAnalysisWrapper
    bool may_alias(const llvm::Value* v1, const llvm::Value* v2) const;
};

// ============================================================================
// No-Alias IFDS/IDE Problem Bases (Phasar-style split)
// ============================================================================

template<typename Fact>
class DefaultNoAliasIFDSProblem : public IFDSProblem<Fact> {};

// ============================================================================
// Alias-Aware IFDS Problem Base
// ============================================================================

template<typename Fact>
class DefaultAliasAwareIFDSProblem : public IFDSProblem<Fact> {
public:
    using typename IFDSProblem<Fact>::FactSet;

protected:
    bool has_alias_analysis() const {
        return this->m_alias_analysis != nullptr &&
               this->m_alias_analysis->isInitialized();
    }

    bool may_alias_or_equal(const llvm::Value* v1, const llvm::Value* v2) const {
        if (v1 == v2) {
            return true;
        }
        return this->may_alias(v1, v2);
    }

    std::vector<const llvm::Value*>
    get_aliases_including_self(const llvm::Value* value) const {
        std::vector<const llvm::Value*> aliases;
        if (!value) {
            return aliases;
        }

        aliases.push_back(value);
        if (!has_alias_analysis()) {
            return aliases;
        }

        std::vector<const llvm::Value*> queried;
        if (!this->m_alias_analysis->getAliasSet(value, queried)) {
            return aliases;
        }

        std::unordered_set<const llvm::Value*> seen;
        seen.insert(value);
        for (const llvm::Value* alias : queried) {
            if (!alias || !seen.insert(alias).second) {
                continue;
            }
            aliases.push_back(alias);
        }
        return aliases;
    }

    std::vector<const llvm::Value*>
    get_aliases_including_self_in_context(const llvm::Value* value,
                                          const llvm::Instruction* context) const {
        auto aliases = get_aliases_including_self(value);
        if (!context) {
            return aliases;
        }

        std::vector<const llvm::Value*> filtered;
        filtered.reserve(aliases.size());
        for (const llvm::Value* alias : aliases) {
            if (const auto* inst = llvm::dyn_cast<llvm::Instruction>(alias)) {
                if (inst->getParent() == context->getParent() &&
                    context->comesBefore(inst)) {
                    // Keep Phasar-like precision guard: don't add future defs
                    // in the same block; they will be seen later anyway.
                    continue;
                }
            }
            filtered.push_back(alias);
        }
        return filtered;
    }

    template <typename ExtractValueFn, typename BuildAliasFactFn, typename ShouldExpandFn>
    void expand_facts_with_aliases_in_context(
        FactSet& facts, const llvm::Instruction* context,
        ExtractValueFn&& extractValue, BuildAliasFactFn&& buildAliasFact,
        ShouldExpandFn&& shouldExpand) const {
        FactSet snapshot = facts;
        for (const auto& fact : snapshot) {
            if (!shouldExpand(fact)) {
                continue;
            }
            const llvm::Value* value = extractValue(fact);
            if (!value) {
                continue;
            }
            for (const llvm::Value* alias :
                 get_aliases_including_self_in_context(value, context)) {
                facts.insert(buildAliasFact(alias, fact));
            }
        }
    }
};

// ============================================================================
// IDE Problem Interface
// ============================================================================

template<typename Fact, typename Value>
class IDEProblem : public IFDSProblem<Fact> {
public:
    using ValueType = Value;
    using EdgeFunction = std::function<Value(const Value&)>;
    using FactSet = typename IFDSProblem<Fact>::FactSet;
    
    // Edge functions for IDE
    virtual EdgeFunction normal_edge_function(const llvm::Instruction* stmt, 
                                            const Fact& src_fact, const Fact& tgt_fact) = 0;
    virtual EdgeFunction call_edge_function(const llvm::CallBase* call, 
                                           const Fact& src_fact, const Fact& tgt_fact) = 0;
    virtual EdgeFunction return_edge_function(const llvm::CallBase* call, 
                                             const Fact& exit_fact, const Fact& ret_fact) = 0;
    virtual EdgeFunction call_to_return_edge_function(const llvm::CallBase* call, 
                                                     const Fact& src_fact, const Fact& tgt_fact) = 0;
    // Optional summary flow/edge functions (for special-cased callees)
    virtual FactSet summary_flow(const llvm::CallBase* /*call*/,
                                 const llvm::Function* /*callee*/,
                                 const Fact& /*fact*/) {
        return {};
    }
    virtual EdgeFunction summary_edge_function(const llvm::CallBase* /*call*/,
                                               const Fact& /*src_fact*/,
                                               const Fact& /*tgt_fact*/) {
        return identity();
    }
    
    // Value domain operations
    virtual Value top_value() const = 0;
    virtual Value bottom_value() const = 0;
    virtual Value join(const Value& v1, const Value& v2) const = 0;
    
    // Edge function composition
    virtual EdgeFunction compose(const EdgeFunction& f1, const EdgeFunction& f2) const;
    // Edge function join (meet-over-all-paths merge on jump functions)
    virtual EdgeFunction join_edge_functions(const EdgeFunction& f1,
                                            const EdgeFunction& f2) const;
    // Heuristic equality check used to suppress redundant jump-function updates.
    virtual bool edge_function_equivalent(const EdgeFunction& f1,
                                          const EdgeFunction& f2) const;
    
    // Identity edge function
    EdgeFunction identity() const;
};

template<typename Fact, typename Value>
class DefaultNoAliasIDEProblem : public IDEProblem<Fact, Value> {};

template<typename Fact, typename Value>
class DefaultAliasAwareIDEProblem : public IDEProblem<Fact, Value> {
protected:
    bool has_alias_analysis() const {
        return this->m_alias_analysis != nullptr &&
               this->m_alias_analysis->isInitialized();
    }

    bool may_alias_or_equal(const llvm::Value* v1, const llvm::Value* v2) const {
        if (v1 == v2) {
            return true;
        }
        return this->may_alias(v1, v2);
    }

    std::vector<const llvm::Value*>
    get_aliases_including_self(const llvm::Value* value) const {
        std::vector<const llvm::Value*> aliases;
        if (!value) {
            return aliases;
        }

        aliases.push_back(value);
        if (!has_alias_analysis()) {
            return aliases;
        }

        std::vector<const llvm::Value*> queried;
        if (!this->m_alias_analysis->getAliasSet(value, queried)) {
            return aliases;
        }

        std::unordered_set<const llvm::Value*> seen;
        seen.insert(value);
        for (const llvm::Value* alias : queried) {
            if (!alias || !seen.insert(alias).second) {
                continue;
            }
            aliases.push_back(alias);
        }
        return aliases;
    }

    std::vector<const llvm::Value*>
    get_aliases_including_self_in_context(const llvm::Value* value,
                                          const llvm::Instruction* context) const {
        auto aliases = get_aliases_including_self(value);
        if (!context) {
            return aliases;
        }

        std::vector<const llvm::Value*> filtered;
        filtered.reserve(aliases.size());
        for (const llvm::Value* alias : aliases) {
            if (const auto* inst = llvm::dyn_cast<llvm::Instruction>(alias)) {
                if (inst->getParent() == context->getParent() &&
                    context->comesBefore(inst)) {
                    continue;
                }
            }
            filtered.push_back(alias);
        }
        return filtered;
    }
};

// ============================================================================
// Exploded Supergraph Representation
// ============================================================================

template<typename Fact>
class ExplodedSupergraph {
public:
    struct Node {
        const llvm::Instruction* instruction;
        Fact fact;
        
        Node() : instruction(nullptr), fact() {}
        Node(const llvm::Instruction* inst, const Fact& f) : instruction(inst), fact(f) {}
        
        bool operator==(const Node& other) const {
            return instruction == other.instruction && fact == other.fact;
        }
        bool operator<(const Node& other) const {
            if (instruction != other.instruction) return instruction < other.instruction;
            return fact < other.fact;
        }
    };
    
    struct NodeHash {
        size_t operator()(const Node& node) const {
            size_t h1 = std::hash<const llvm::Instruction*>{}(node.instruction);
            size_t h2 = std::hash<Fact>{}(node.fact);
            return h1 ^ (h2 << 1);
        }
    };
    
    struct Edge {
        Node source;
        Node target;
        enum Type { NORMAL, CALL, RETURN, CALL_TO_RETURN } type;
        
        Edge(const Node& src, const Node& tgt, Type t) : source(src), target(tgt), type(t) {}
    };
    
    using NodeId = Node;
    using EdgeId = Edge;
    using Graph = ExplodedSupergraph<Fact>;
    
    // GraphInterface implementation for fixpoint iterator  
    static NodeId entry(const Graph& graph);
    static NodeId source(const Graph& graph, const EdgeId& edge);
    static NodeId target(const Graph& graph, const EdgeId& edge);
    static std::vector<EdgeId> predecessors(const Graph& graph, const NodeId& node);
    static std::vector<EdgeId> successors(const Graph& graph, const NodeId& node);
    
    void add_edge(const Edge& edge);
    void set_entry(const NodeId& entry);
    const std::vector<Edge>& get_edges() const;
    
private:
    std::unique_ptr<NodeId> m_entry;
    std::vector<Edge> m_edges;
    std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_successors;
    std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_predecessors;
};






// ============================================================================
// IFDS/IDE Solvers
// ============================================================================
// Solver declarations: include/Dataflow/IFDS/Solvers/IFDSSolver.h and
// include/Dataflow/IFDS/Solvers/IDESolver.h

} // namespace ifds

// Provide std::hash specializations for IFDS types used in unordered containers
namespace std {
template<typename Fact>
struct hash<ifds::PathEdge<Fact>> {
    size_t operator()(const ifds::PathEdge<Fact>& edge) const noexcept {
        return ifds::PathEdgeHash<Fact>{}(edge);
    }
};

template<typename Fact>
struct hash<ifds::SummaryEdge<Fact>> {
    size_t operator()(const ifds::SummaryEdge<Fact>& edge) const noexcept {
        return ifds::SummaryEdgeHash<Fact>{}(edge);
    }
};
} // namespace std

// ============================================================================
// Template Implementation (moved to .cpp for explicit instantiation)
// ============================================================================

// Provide inline template implementations for commonly used helpers so that
// templated clients (e.g., taint analysis) can link without relying on a
// separate translation unit.

namespace ifds {

template<typename Fact>
inline void IFDSProblem<Fact>::set_alias_analysis(lotus::AliasAnalysisWrapper* aa) {
    m_alias_analysis = aa;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::has_alias_analysis_configured() const {
    return m_alias_analysis != nullptr;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::is_source(const llvm::Instruction*) const {
    return false;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::is_sink(const llvm::Instruction*) const {
    return false;
}

template<typename Fact>
inline bool IFDSProblem<Fact>::may_alias(const llvm::Value* v1, const llvm::Value* v2) const {
    if (!m_alias_analysis || !v1 || !v2) return false;
    return m_alias_analysis->mayAlias(v1, v2);
}

template<typename Fact>
inline typename IFDSProblem<Fact>::InitialSeeds
IFDSProblem<Fact>::initial_seeds(const llvm::Module& module) {
    InitialSeeds seeds;
    const llvm::Function* main_func = module.getFunction("main");
    if (!main_func || main_func->empty()) {
        return seeds;
    }

    const llvm::Instruction* entry = &main_func->getEntryBlock().front();
    seeds.add_seed(entry, initial_facts(main_func));
    return seeds;
}

// ============================================================================
// IDEProblem Inline Implementations
// ============================================================================

template<typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::EdgeFunction
IDEProblem<Fact, Value>::compose(const EdgeFunction& f1, const EdgeFunction& f2) const {
    return [f1, f2](const Value& v) { return f1(f2(v)); };
}

template<typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::EdgeFunction
IDEProblem<Fact, Value>::join_edge_functions(const EdgeFunction& f1,
                                             const EdgeFunction& f2) const {
    return [this, f1, f2](const Value& v) { return join(f1(v), f2(v)); };
}

template<typename Fact, typename Value>
inline bool IDEProblem<Fact, Value>::edge_function_equivalent(
    const EdgeFunction& f1, const EdgeFunction& f2) const {
    const Value top = top_value();
    const Value bottom = bottom_value();
    if (!(f1(top) == f2(top))) {
        return false;
    }
    if (!(f1(bottom) == f2(bottom))) {
        return false;
    }
    const Value joined_probe = join(top, bottom);
    return f1(joined_probe) == f2(joined_probe);
}

template<typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::EdgeFunction
IDEProblem<Fact, Value>::identity() const {
    return [](const Value& v) { return v; };
}

} // namespace ifds
