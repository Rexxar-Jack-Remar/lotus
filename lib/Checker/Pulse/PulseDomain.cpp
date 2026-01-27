#include "Checker/Pulse/PulseDomain.h"
#include "Checker/Pulse/PulseFormula.h"
#include "Checker/Pulse/PulseTransitiveInfo.h"
#include <cassert>

namespace pulse {

//===----------------------------------------------------------------------===//
// Stack Implementation
//===----------------------------------------------------------------------===//

void Stack::add(const llvm::Value* var, Address addr) {
    stack_[var] = addr;
}

Address* Stack::find(const llvm::Value* var) {
    auto it = stack_.find(var);
    return (it != stack_.end()) ? &it->second : nullptr;
}

const Address* Stack::find(const llvm::Value* var) const {
    auto it = stack_.find(var);
    return (it != stack_.end()) ? &it->second : nullptr;
}

void Stack::remove(const llvm::Value* var) {
    stack_.erase(var);
}

void Stack::clear() {
    stack_.clear();
}

//===----------------------------------------------------------------------===//
// Heap Implementation
//===----------------------------------------------------------------------===//

void Heap::addEdge(AbstractValue from, Access access, Address to) {
    edges_[from][access] = to;
}

Address* Heap::findEdge(AbstractValue from, Access access) {
    auto it = edges_.find(from);
    if (it == edges_.end()) return nullptr;
    auto acc_it = it->second.find(access);
    return (acc_it != it->second.end()) ? &acc_it->second : nullptr;
}

const Address* Heap::findEdge(AbstractValue from, Access access) const {
    auto it = edges_.find(from);
    if (it == edges_.end()) return nullptr;
    auto acc_it = it->second.find(access);
    return (acc_it != it->second.end()) ? &acc_it->second : nullptr;
}

void Heap::removeEdges(AbstractValue addr) {
    edges_.erase(addr);
}

//===----------------------------------------------------------------------===//
// AddressAttributes Implementation
//===----------------------------------------------------------------------===//

void AddressAttributes::add(AbstractValue addr, Attribute attr) {
    attrs_[addr].insert(attr);
}

void AddressAttributes::remove(AbstractValue addr, Attribute attr) {
    auto it = attrs_.find(addr);
    if (it != attrs_.end()) {
        it->second.erase(attr);
        if (it->second.empty()) {
            attrs_.erase(it);
        }
    }
}

bool AddressAttributes::has(AbstractValue addr, Attribute attr) const {
    auto it = attrs_.find(addr);
    return (it != attrs_.end()) && (it->second.count(attr) > 0);
}

AttributeSet AddressAttributes::get(AbstractValue addr) const {
    auto it = attrs_.find(addr);
    return (it != attrs_.end()) ? it->second : AttributeSet();
}

void AddressAttributes::clear(AbstractValue addr) {
    attrs_.erase(addr);
}

//===----------------------------------------------------------------------===//
// AbductiveDomain Implementation
//===----------------------------------------------------------------------===//

AbductiveDomain::~AbductiveDomain() = default;

PulseFormula& AbductiveDomain::getPathFormula() {
    if (!path_formula_) {
        path_formula_ = std::make_unique<PulseFormula>();
    }
    return *path_formula_;
}

const PulseFormula& AbductiveDomain::getPathFormula() const {
    if (!path_formula_) {
        // Create a mutable copy for const access
        const_cast<AbductiveDomain*>(this)->path_formula_ = std::make_unique<PulseFormula>();
    }
    return *path_formula_;
}

void AbductiveDomain::setPathFormula(std::unique_ptr<PulseFormula> formula) {
    path_formula_ = std::move(formula);
}

void AbductiveDomain::addNonNull(AbstractValue addr) {
    getPathFormula().addNonNull(addr);
}

bool AbductiveDomain::isNonNull(AbstractValue addr) const {
    return getPathFormula().isNonNull(addr);
}

void AbductiveDomain::addEquality(AbstractValue v1, AbstractValue v2) {
    getPathFormula().addEquality(v1, v2);
}

AbstractValue AbductiveDomain::getCanonical(AbstractValue v) const {
    return getPathFormula().getCanonical(v);
}

void AbductiveDomain::abduceToPre(AbstractValue addr, const Access& access, Address target) {
    // Biabduction: if we read from memory not in pre, add it to pre
    if (!pre_heap_.findEdge(addr, access)) {
        pre_heap_.addEdge(addr, access, target);
    }
}

void AbductiveDomain::abduceAttrToPre(AbstractValue addr, Attribute attr) {
    // Abduce attribute to pre if not already there
    if (!pre_attrs_.has(addr, attr)) {
        pre_attrs_.add(addr, attr);
    }
}

void AbductiveDomain::setInvalidationInfo(AbstractValue addr,
                                          InvalidationKind kind,
                                          const llvm::Instruction* loc) {
    invalidation_info_[addr] = {kind, loc};
}

llvm::Optional<std::pair<InvalidationKind, const llvm::Instruction*>>
AbductiveDomain::getInvalidationInfo(AbstractValue addr) const {
    auto it = invalidation_info_.find(addr);
    if (it != invalidation_info_.end())
        return it->second;
    return llvm::None;
}

AbductiveDomain AbductiveDomain::clone() const {
    AbductiveDomain cloned;
    
    // Clone stacks (simple copy)
    cloned.post_stack_ = post_stack_;
    cloned.pre_stack_ = pre_stack_;
    
    // Clone heaps (simple copy)
    cloned.post_heap_ = post_heap_;
    cloned.pre_heap_ = pre_heap_;
    
    // Clone attributes (simple copy)
    cloned.post_attrs_ = post_attrs_;
    cloned.pre_attrs_ = pre_attrs_;
    
    // Clone path formula
    if (path_formula_) {
        cloned.path_formula_ = std::make_unique<PulseFormula>(path_formula_->clone());
    }

    cloned.invalidation_info_ = invalidation_info_;
    cloned.taint_domain_.join(taint_domain_);
    
    // Clone new fields
    cloned.skipped_calls_ = skipped_calls_;
    cloned.transitive_info_ = transitive_info_.clone();
    cloned.unknown_values_ = unknown_values_;
    cloned.need_dynamic_type_specialization_ = need_dynamic_type_specialization_;
    cloned.recursive_calls_ = recursive_calls_;
    cloned.loop_header_info_ = loop_header_info_;
    
    // Clone loop invariant under inference if present
    if (loop_invariant_under_inference_) {
        cloned.loop_invariant_under_inference_ = std::make_unique<LoopInvariantUnderInference>(
            loop_invariant_under_inference_->header,
            loop_invariant_under_inference_->entry_astate->clone());
    }

    return cloned;
}

llvm::Optional<AbductiveDomain> AbductiveDomain::merge(const AbductiveDomain& d1, 
                                                         const AbductiveDomain& d2) {
    // Merge path formulas first to check for contradictions
    PulseFormula merged_formula;
    if (d1.path_formula_ && d2.path_formula_) {
        merged_formula = PulseFormula::merge(*d1.path_formula_, *d2.path_formula_);
        if (!merged_formula.isConsistent()) {
            return llvm::None;  // Contradiction detected
        }
    } else if (d1.path_formula_) {
        merged_formula = d1.path_formula_->clone();
    } else if (d2.path_formula_) {
        merged_formula = d2.path_formula_->clone();
    }
    
    AbductiveDomain merged;
    merged.path_formula_ = std::make_unique<PulseFormula>(std::move(merged_formula));
    
    // Merge stacks: take union of variables
    // For variables in both, we'd need to merge their addresses (simplified: take first)
    for (const auto& kv : d1.post_stack_.getMap()) {
        merged.post_stack_.add(kv.first, kv.second);
    }
    for (const auto& kv : d2.post_stack_.getMap()) {
        if (!merged.post_stack_.find(kv.first)) {
            merged.post_stack_.add(kv.first, kv.second);
        }
    }
    
    // Merge heaps: take union of edges
    for (const auto& kv : d1.post_heap_.getEdges()) {
        for (const auto& edge_kv : kv.second) {
            merged.post_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
        }
    }
    for (const auto& kv : d2.post_heap_.getEdges()) {
        for (const auto& edge_kv : kv.second) {
            if (!merged.post_heap_.findEdge(kv.first, edge_kv.first)) {
                merged.post_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
            }
        }
    }
    
    // Merge attributes: take union
    for (const auto& kv : d1.post_attrs_.getAttrs()) {
        for (Attribute attr : kv.second) {
            merged.post_attrs_.add(kv.first, attr);
        }
    }
    for (const auto& kv : d2.post_attrs_.getAttrs()) {
        for (Attribute attr : kv.second) {
            merged.post_attrs_.add(kv.first, attr);
        }
    }
    
    // Merge pre-conditions similarly
    for (const auto& kv : d1.pre_stack_.getMap()) {
        merged.pre_stack_.add(kv.first, kv.second);
    }
    for (const auto& kv : d2.pre_stack_.getMap()) {
        if (!merged.pre_stack_.find(kv.first)) {
            merged.pre_stack_.add(kv.first, kv.second);
        }
    }
    
    for (const auto& kv : d1.pre_heap_.getEdges()) {
        for (const auto& edge_kv : kv.second) {
            merged.pre_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
        }
    }
    for (const auto& kv : d2.pre_heap_.getEdges()) {
        for (const auto& edge_kv : kv.second) {
            if (!merged.pre_heap_.findEdge(kv.first, edge_kv.first)) {
                merged.pre_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
            }
        }
    }
    
    for (const auto& kv : d1.pre_attrs_.getAttrs()) {
        for (Attribute attr : kv.second) {
            merged.pre_attrs_.add(kv.first, attr);
        }
    }
    for (const auto& kv : d2.pre_attrs_.getAttrs()) {
        for (Attribute attr : kv.second) {
            merged.pre_attrs_.add(kv.first, attr);
        }
    }

    for (const auto& kv : d1.invalidation_info_)
        merged.invalidation_info_.insert(kv);
    for (const auto& kv : d2.invalidation_info_) {
        if (merged.invalidation_info_.find(kv.first) == merged.invalidation_info_.end())
            merged.invalidation_info_.insert(kv);
    }
    
    // Merge skipped calls (union)
    merged.skipped_calls_ = d1.skipped_calls_;
    merged.skipped_calls_.insert(d2.skipped_calls_.begin(), d2.skipped_calls_.end());
    
    // Merge transitive info
    merged.transitive_info_ = TransitiveInfo::merge(d1.transitive_info_, d2.transitive_info_);
    
    // Merge unknown values flag (OR operation)
    merged.unknown_values_ = d1.unknown_values_ || d2.unknown_values_;
    
    // Merge need_dynamic_type_specialization (union)
    merged.need_dynamic_type_specialization_ = d1.need_dynamic_type_specialization_;
    merged.need_dynamic_type_specialization_.insert(
        d2.need_dynamic_type_specialization_.begin(),
        d2.need_dynamic_type_specialization_.end());
    
    // Merge recursive calls (union)
    merged.recursive_calls_ = d1.recursive_calls_;
    merged.recursive_calls_.insert(d2.recursive_calls_.begin(), d2.recursive_calls_.end());
    
    // Merge loop header info (preserve from lhs, as in Infer)
    merged.loop_header_info_ = d1.loop_header_info_;
    
    // Loop invariant under inference: preserve from lhs if present
    if (d1.loop_invariant_under_inference_) {
        merged.loop_invariant_under_inference_ = std::make_unique<LoopInvariantUnderInference>(
            d1.loop_invariant_under_inference_->header,
            d1.loop_invariant_under_inference_->entry_astate->clone());
    }

    return merged;
}

} // namespace pulse
