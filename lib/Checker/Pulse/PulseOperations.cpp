#include "Checker/Pulse/PulseOperations.h"
#include "Checker/Pulse/PulseTaint.h"
#include "Checker/Pulse/PulseFormula.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <cassert>

namespace pulse {

//===----------------------------------------------------------------------===//
// AbstractValueFactory Implementation
//===----------------------------------------------------------------------===//

AbstractValue AbstractValueFactory::getOrCreate(const llvm::Value* v) {
    auto it = value_map_.find(v);
    if (it != value_map_.end()) {
        return it->second;
    }
    if (mustAliasFn_) {
        for (const auto& kv : value_map_) {
            if (mustAliasFn_(v, kv.first)) {
                AbstractValue av = kv.second;
                value_map_[v] = av;
                return av;
            }
        }
    }
    AbstractValue av(v, next_id_++);
    value_map_[v] = av;
    return av;
}

AbstractValue AbstractValueFactory::get(const llvm::Value* v) const {
    auto it = value_map_.find(v);
    assert(it != value_map_.end() && "Value not found in factory");
    return it->second;
}

bool AbstractValueFactory::has(const llvm::Value* v) const {
    return value_map_.find(v) != value_map_.end();
}

AbstractValue AbstractValueFactory::createFresh(const llvm::Value* hint) {
    // Create a fresh abstract value (for unknown/allocated memory)
    // In a full implementation, we'd track this properly
    AbstractValue av(hint, next_id_++);
    if (hint) {
        value_map_[hint] = av;
    }
    return av;
}

//===----------------------------------------------------------------------===//
// PulseOperations Implementation
//===----------------------------------------------------------------------===//

llvm::Optional<Address> PulseOperations::eval(AbductiveDomain& astate,
                                              const llvm::Value* exp,
                                              const llvm::Instruction* loc,
                                              const llvm::BasicBlock* pred) {
    if (auto* CI = llvm::dyn_cast<llvm::ConstantInt>(exp)) {
        if (CI->isZero()) {
            AbstractValue null_addr = factory_->createFresh();
            Address addr(null_addr);
            astate.getPostAttrs().add(null_addr, Attribute::Null);
            astate.getPathFormula().addNull(null_addr);
            return addr;
        }
    }

    if (llvm::isa<llvm::ConstantPointerNull>(exp)) {
        AbstractValue null_addr = factory_->createFresh();
        Address addr(null_addr);
        astate.getPostAttrs().add(null_addr, Attribute::Null);
        astate.getPathFormula().addNull(null_addr);
        return llvm::Optional<Address>(addr);
    }

    if (auto* addr = astate.getPostStack().find(exp)) {
        // Canonicalize the address
        AbstractValue canon_addr = astate.getCanonical(addr->addr);
        Address result(canon_addr);
        result.history = addr->history;
        return llvm::Optional<Address>(result);
    }

    if (auto* GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(exp)) {
        auto base_opt = eval(astate, GEP->getPointerOperand(), loc, pred);
        if (!base_opt)
            return llvm::None;
        Address base = *base_opt;
        AbstractValue cur = base.addr;
        for (unsigned i = 1, n = GEP->getNumOperands(); i < n; ++i) {
            llvm::Value* idx = GEP->getOperand(i);
            Access acc;
            if (auto* C = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
                acc = Access(static_cast<unsigned>(C->getZExtValue()));
            } else {
                AbstractValue idxAv = factory_->getOrCreate(idx);
                acc = Access::arrayIndex(idxAv);
            }
            if (auto* target = astate.getPostHeap().findEdge(cur, acc)) {
                cur = target->addr;
                continue;
            }
            AbstractValue fresh = factory_->createFresh(GEP);
            Address targetAddr(fresh);
            targetAddr.history = base.history;
            if (loc)
                targetAddr.history.addEvent(ValueHistory::EventKind::Unknown, loc, loc->getFunction());
            astate.getPostHeap().addEdge(cur, acc, targetAddr);
            astate.abduceToPre(cur, acc, targetAddr);
            astate.abduceAttrToPre(cur, Attribute::Allocated);
            cur = fresh;
        }
        AbstractValue gepAv = factory_->getOrCreate(GEP);
        Address gepAddr(gepAv);
        gepAddr.history = base.history;
        if (loc)
            gepAddr.history.addEvent(ValueHistory::EventKind::Unknown, loc, loc->getFunction());
        Access deref(AccessKind::Dereference);
        astate.getPostHeap().addEdge(gepAv, deref, Address(cur));
        astate.abduceToPre(gepAv, deref, Address(cur));
        astate.abduceAttrToPre(gepAv, Attribute::Allocated);
        return llvm::Optional<Address>(gepAddr);
    }

    if (auto* Phi = llvm::dyn_cast<llvm::PHINode>(exp)) {
        if (!pred)
            return llvm::None;
        // Check if pred is actually a predecessor of the PHI node
        bool is_predecessor = false;
        for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
            if (Phi->getIncomingBlock(i) == pred) {
                is_predecessor = true;
                break;
            }
        }
        if (!is_predecessor) {
            // pred is not a predecessor, try to find a valid predecessor
            if (Phi->getNumIncomingValues() > 0) {
                pred = Phi->getIncomingBlock(0);
            } else {
                return llvm::None;
            }
        }
        const llvm::Value* incoming = Phi->getIncomingValueForBlock(pred);
        return eval(astate, incoming, loc, nullptr);
    }

    if (llvm::isa<llvm::Instruction>(exp)) {
        AbstractValue av = factory_->getOrCreate(exp);
        return llvm::Optional<Address>(Address(av));
    }

    AbstractValue av = factory_->getOrCreate(exp);
    return llvm::Optional<Address>(Address(av));
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::evalDeref(AbductiveDomain& astate,
                          Address ptr,
                          const llvm::Instruction* loc) {
    // Canonicalize pointer
    AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
    
    // Check if pointer is null (using formula)
    if (astate.getPathFormula().isNull(canon_ptr) || 
        astate.getPostAttrs().has(canon_ptr, Attribute::Null)) {
        return {OperationResult::NullDereference, llvm::None};
    }
    
    // Check if pointer is invalid
    if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
        return {OperationResult::UseAfterFree, llvm::None};
    }
    
    // Try to find edge in heap (using canonical value)
    Access deref(AccessKind::Dereference);
    if (auto* target = astate.getPostHeap().findEdge(canon_ptr, deref)) {
        // Canonicalize target
        AbstractValue canon_target = astate.getCanonical(target->addr);
        Address result(canon_target);
        result.history = target->history;
        return {OperationResult::Success, result};
    }
    
    // Not in post heap: abduce to pre (biabduction)
    AbstractValue fresh = factory_->createFresh();
    Address target(fresh);
    target.history = ptr.history;
    target.history.addEvent(ValueHistory::EventKind::Allocation, loc, loc ? loc->getFunction() : nullptr);
    
    // Add to post heap (using canonical value)
    astate.getPostHeap().addEdge(canon_ptr, deref, target);
    
    // Abduce to pre
    astate.abduceToPre(canon_ptr, deref, target);
    astate.abduceAttrToPre(canon_ptr, Attribute::Allocated);
    
    return {OperationResult::Success, llvm::Optional<Address>(target)};
}

OperationResult PulseOperations::checkAddrAccess(AbductiveDomain& astate,
                                                  Address addr,
                                                  const llvm::Instruction* loc) {
    // Canonicalize address
    AbstractValue canon_addr = astate.getCanonical(addr.addr);
    
    // Check null (using formula)
    if (astate.getPathFormula().isNull(canon_addr) || 
        astate.getPostAttrs().has(canon_addr, Attribute::Null)) {
        return OperationResult::NullDereference;
    }
    
    // Check invalid
    if (astate.getPostAttrs().has(canon_addr, Attribute::Invalid)) {
        return OperationResult::UseAfterFree;
    }
    
    // Check uninitialized (for reads)
    if (astate.getPostAttrs().has(canon_addr, Attribute::Uninitialized)) {
        return OperationResult::UninitializedRead;
    }
    
    return OperationResult::Success;
}

void PulseOperations::allocate(AbductiveDomain& astate,
                               AbstractValue addr,
                               const llvm::Instruction* loc) {
    astate.getPostAttrs().add(addr, Attribute::Allocated);
    astate.getPostAttrs().remove(addr, Attribute::Invalid);
    astate.getPostAttrs().remove(addr, Attribute::Uninitialized);
}

void PulseOperations::invalidate(AbductiveDomain& astate,
                                 Address addr,
                                 const llvm::Instruction* loc,
                                 InvalidationKind kind) {
    AbstractValue canon = astate.getCanonical(addr.addr);
    astate.getPostAttrs().add(canon, Attribute::Invalid);
    astate.getPostAttrs().remove(canon, Attribute::Allocated);
    astate.setInvalidationInfo(canon, kind, loc);
    astate.getPostHeap().removeEdges(addr.addr);
}

OperationResult PulseOperations::writeDeref(AbductiveDomain& astate,
                                            Address ptr,
                                            Address value,
                                            const llvm::Instruction* loc) {
    // Check access
    auto result = checkAddrAccess(astate, ptr, loc);
    if (result != OperationResult::Success) {
        return result;
    }
    
    // Canonicalize values
    AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
    AbstractValue canon_value = astate.getCanonical(value.addr);
    Address canon_value_addr(canon_value);
    canon_value_addr.history = value.history;
    
    // Write to heap (using canonical values)
    Access deref(AccessKind::Dereference);
    astate.getPostHeap().addEdge(canon_ptr, deref, canon_value_addr);

    // Propagate taint: if value is tainted, propagate to memory location
    TaintOperations::propagateThroughStore(astate, canon_value, canon_ptr, loc);
    
    // Initialize if needed
    initialize(astate, canon_ptr);
    
    return OperationResult::Success;
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::readDeref(AbductiveDomain& astate,
                           Address ptr,
                           const llvm::Instruction* loc) {
    // Check access
    auto result = checkAddrAccess(astate, ptr, loc);
    if (result != OperationResult::Success) {
        return {result, llvm::None};
    }
    
    // Evaluate dereference
    auto deref_result = evalDeref(astate, ptr, loc);
    
    // Propagate taint: if memory location is tainted, propagate to loaded value
    if (deref_result.first == OperationResult::Success && deref_result.second) {
        AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
        AbstractValue canon_value = astate.getCanonical(deref_result.second->addr);
        TaintOperations::propagateThroughLoad(astate, canon_ptr, canon_value, loc);
    }
    
    return deref_result;
}

void PulseOperations::initialize(AbductiveDomain& astate, AbstractValue addr) {
    astate.getPostAttrs().remove(addr, Attribute::Uninitialized);
}

OperationResult PulseOperations::checkNull(AbductiveDomain& astate,
                                           Address addr,
                                           const llvm::Instruction* loc) {
    if (astate.getPostAttrs().has(addr.addr, Attribute::Null)) {
        return OperationResult::NullDereference;
    }
    return OperationResult::Success;
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::shallowCopy(AbductiveDomain& astate,
                            Address source,
                            const llvm::Instruction* loc) {
    // Check source is valid
    auto result = checkAddrAccess(astate, source, loc);
    if (result != OperationResult::Success) {
        return {result, llvm::None};
    }
    
    // Create fresh address for copy
    AbstractValue copy_addr = factory_->createFresh();
    Address copy(copy_addr);
    copy.history = source.history;
    if (loc) {
        copy.history.addEvent(ValueHistory::EventKind::Unknown, loc, loc->getFunction());
    }
    
    // Copy all edges from source to copy
    AbstractValue canon_source = astate.getCanonical(source.addr);
    const auto& edges = astate.getPostHeap().getEdges();
    auto it = edges.find(canon_source);
    if (it != edges.end()) {
        for (const auto& edge_kv : it->second) {
            astate.getPostHeap().addEdge(copy_addr, edge_kv.first, edge_kv.second);
        }
    }
    
    // Copy attributes (except allocation - copy is a new allocation)
    const auto& attrs = astate.getPostAttrs().get(canon_source);
    for (Attribute attr : attrs) {
        if (attr != Attribute::Allocated) {
            astate.getPostAttrs().add(copy_addr, attr);
        }
    }
    
    return {OperationResult::Success, llvm::Optional<Address>(copy)};
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::deepCopy(AbductiveDomain& astate,
                          Address source,
                          const llvm::Instruction* loc,
                          unsigned depth_max) {
    // Check source is valid
    auto result = checkAddrAccess(astate, source, loc);
    if (result != OperationResult::Success) {
        return {result, llvm::None};
    }
    
    // Create fresh address for copy
    AbstractValue copy_addr = factory_->createFresh();
    Address copy(copy_addr);
    copy.history = source.history;
    if (loc) {
        copy.history.addEvent(ValueHistory::EventKind::Unknown, loc, loc->getFunction());
    }
    
    // Recursively copy edges up to depth_max
    AbstractValue canon_source = astate.getCanonical(source.addr);
    deepCopyRecursive(astate, canon_source, copy_addr, depth_max, 0);
    
    return {OperationResult::Success, llvm::Optional<Address>(copy)};
}

void PulseOperations::deepCopyRecursive(AbductiveDomain& astate,
                                        AbstractValue source,
                                        AbstractValue target,
                                        unsigned depth_max,
                                        unsigned current_depth) {
    if (depth_max > 0 && current_depth >= depth_max) {
        // At max depth, do shallow copy
        const auto& edges = astate.getPostHeap().getEdges();
        auto it = edges.find(source);
        if (it != edges.end()) {
            for (const auto& edge_kv : it->second) {
                astate.getPostHeap().addEdge(target, edge_kv.first, edge_kv.second);
            }
        }
        return;
    }
    
    // Deep copy: recursively copy all edges
    const auto& edges = astate.getPostHeap().getEdges();
    auto it = edges.find(source);
    if (it != edges.end()) {
        for (const auto& edge_kv : it->second) {
            AbstractValue fresh_target = factory_->createFresh();
            Address target_addr(fresh_target);
            target_addr.history = edge_kv.second.history;
            astate.getPostHeap().addEdge(target, edge_kv.first, target_addr);
            
            // Recursively copy
            deepCopyRecursive(astate, edge_kv.second.addr, fresh_target, depth_max, current_depth + 1);
        }
    }
}

void PulseOperations::havoc(AbductiveDomain& astate,
                           Address addr,
                           const llvm::Instruction* loc) {
    // Remove all edges from the address (model unknown effects)
    AbstractValue canon_addr = astate.getCanonical(addr.addr);
    astate.getPostHeap().removeEdges(canon_addr);
    
    // Remove attributes (except allocation if it was allocated)
    bool was_allocated = astate.getPostAttrs().has(canon_addr, Attribute::Allocated);
    astate.getPostAttrs().clear(canon_addr);
    if (was_allocated) {
        astate.getPostAttrs().add(canon_addr, Attribute::Allocated);
    }
}

OperationResult PulseOperations::checkAddressEscape(AbductiveDomain& astate,
                                                  Address addr,
                                                  const llvm::Function* current_function,
                                                  const llvm::Instruction* loc) {
    // Check if address is from stack (local variable)
    // If it escapes (e.g., returned, stored to global, passed to function),
    // it should be heap-allocated
    
    AbstractValue canon_addr = astate.getCanonical(addr.addr);
    
    // Check if this is a stack address
    // In a full implementation, we'd track which addresses are stack vs heap
    // For now, we check if it's not marked as allocated
    if (!astate.getPostAttrs().has(canon_addr, Attribute::Allocated)) {
        // This might be a stack address that's escaping
        // Return error to indicate potential escape
        return OperationResult::InvalidAccess;
    }
    
    return OperationResult::Success;
}

} // namespace pulse
