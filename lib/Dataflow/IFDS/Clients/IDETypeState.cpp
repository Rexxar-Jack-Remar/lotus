/*
 *
 * Author: rainoftime
 */
#include "Dataflow/IFDS/Clients/IDETypeState.h"

#include <regex>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>

namespace ifds {

// ============================================================================
// TypeStateProperty Implementation
// ============================================================================

std::vector<TypeStateProperty::Transition>
TypeStateProperty::get_transitions(const llvm::Instruction *inst) const {
  std::vector<Transition> result;

  for (const auto &pair : m_operation_transitions) {
    if (matcher_matches(pair.first, inst)) {
      result.insert(result.end(), pair.second.begin(), pair.second.end());
    }
  }

  return result;
}

bool TypeStateProperty::matcher_matches(const OperationMatcher &matcher,
                                        const llvm::Instruction *inst) const {
  switch (matcher.type) {
  case OperationMatcher::FUNCTION_NAME: {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
      if (const llvm::Function *callee = call->getCalledFunction()) {
        return callee->getName() == matcher.pattern;
      }
    }
    return false;
  }

  case OperationMatcher::FUNCTION_REGEX: {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
      if (const llvm::Function *callee = call->getCalledFunction()) {
        try {
          std::regex re(matcher.pattern);
          return std::regex_match(callee->getName().str(), re);
        } catch (...) {
          return false;
        }
      }
    }
    return false;
  }

  case OperationMatcher::INSTRUCTION_OPCODE: {
    return std::string(inst->getOpcodeName()) == matcher.pattern;
  }

  case OperationMatcher::CUSTOM_PREDICATE: {
    return matcher.predicate && matcher.predicate(inst);
  }
  }

  return false;
}

// ============================================================================
// IDETypeState Implementation
// ============================================================================

IDETypeState::IDETypeState(std::shared_ptr<TypeStateProperty> property)
    : m_property(std::move(property)), m_track_globals(true),
      m_track_heap(true) {
  if (!m_property) {
    throw std::runtime_error("IDETypeState: property cannot be null");
  }
}

IDETypeState::IDETypeState(
    std::shared_ptr<TypeStateDescriptionBase> description)
    : m_description(std::move(description)), m_track_globals(true),
      m_track_heap(true) {
  if (!m_description) {
    throw std::runtime_error("IDETypeState: description cannot be null");
  }
  const std::string type_name = m_description->get_type_name_of_interest();
  if (!type_name.empty()) {
    m_tracked_types.insert(type_name);
  }
}

IDETypeState::FactSet IDETypeState::initial_facts(const llvm::Function *main) {
  FactSet seeds;

  // Track function arguments
  for (const llvm::Argument &arg : main->args()) {
    if (should_track(&arg)) {
      seeds.insert(&arg);
    }
  }

  // Track global variables if enabled
  if (m_track_globals) {
    const llvm::Module *module = main->getParent();
    for (const llvm::GlobalVariable &gv : module->globals()) {
      if (should_track(&gv)) {
        seeds.insert(&gv);
      }
    }
  }

  return seeds;
}

IDETypeState::Value IDETypeState::join(const Value &v1, const Value &v2) const {
  // Bottom is identity
  if (v1.is_bottom())
    return v2;
  if (v2.is_bottom())
    return v1;

  // Top absorbs everything
  if (v1.is_top() || v2.is_top())
    return Value(TypeStateValue::TOP);

  // Same state
  if (v1 == v2)
    return v1;

  // Conflicting states -> Top (unknown)
  return Value(TypeStateValue::TOP);
}

IDETypeState::FactSet IDETypeState::normal_flow(const llvm::Instruction *stmt,
                                                const llvm::Instruction *succ,
                                                const Fact &fact) {
  FactSet out;

  // Keep existing fact (unless killed by an aliasing store)
  bool fact_killed = false;

  // For stores: kill facts that may alias with the stored pointer
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const llvm::Value *stored_val = store->getValueOperand();
    const llvm::Value *ptr = store->getPointerOperand();

    // If the fact aliases with the pointer being stored to, it may be killed
    if (fact && may_alias_or_equal(fact, ptr)) {
      fact_killed = true;
    }

    // Propagate taint from stored value to pointer
    if (fact == stored_val && should_track(ptr)) {
      out.insert(ptr);
    }

    // Track the pointer itself
    if (should_track(ptr)) {
      out.insert(ptr);
    }
  }
  // For loads: propagate state from memory to register
  else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    const llvm::Value *ptr = load->getPointerOperand();

    // If fact aliases with loaded pointer, propagate to load result.
    // The old code had two separate blocks: one guarded by `fact &&
    // may_alias_or_equal(...)` and one unconditional `should_track(load)`
    // insert.  The unconditional insert caused the load result to be added
    // as a new fact even when no existing fact flowed into it, which is
    // unsound (it generates facts from thin air).  Only add the load result
    // when the fact actually flows through the pointer.
    if (fact && may_alias_or_equal(fact, ptr) && should_track(load)) {
      out.insert(load);
    }
  }
  // For PHI nodes: propagate from incoming values
  else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(stmt)) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      if (fact == phi->getIncomingValue(i)) {
        out.insert(phi);
      }
    }
  }
  // For casts: propagate aliases
  else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
    if (fact == cast->getOperand(0) && should_track(cast)) {
      out.insert(cast);
    }
  }
  // For GEP: track field accesses
  else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
    if (fact == gep->getPointerOperand() && should_track(gep)) {
      out.insert(gep);
    }
  }

  // Keep existing fact unless killed
  if (fact && should_track(fact) && !fact_killed) {
    out.insert(fact);
  }

  // Generate new fact for instruction result
  if (!stmt->getType()->isVoidTy() && should_track(stmt)) {
    out.insert(stmt);
  }

  return out;
}

IDETypeState::FactSet IDETypeState::call_flow(const llvm::CallBase *call,
                                              const llvm::Function *callee,
                                              const Fact &fact) {
  FactSet out;

  if (!callee || callee->isDeclaration()) {
    return out;
  }

  // Map actual arguments to formal parameters (with alias analysis)
  for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
    const llvm::Value *arg = call->getArgOperand(i);
    if (!arg)
      continue;

    const auto *param_it = callee->arg_begin();
    std::advance(param_it, i);
    if (param_it == callee->arg_end())
      break;

    const llvm::Argument *formal = &*param_it;

    // Direct match or may-alias
    if (fact == arg && should_track(formal)) {
      out.insert(formal);
    } else if (fact && arg->getType()->isPointerTy() &&
               fact->getType()->isPointerTy() &&
               may_alias_or_equal(arg, fact)) {
      // Fact aliases with argument, propagate to formal
      if (should_track(formal)) {
        out.insert(formal);
      }
    }
  }

  return out;
}

IDETypeState::FactSet IDETypeState::return_flow(const llvm::CallBase *call,
                                                const llvm::Instruction *exit_inst,
                                                const llvm::Instruction *return_site, const llvm::Function *callee,
                                                const Fact &exit_fact,
                                                const Fact &call_fact) {
  (void)callee;
  (void)exit_fact;

  FactSet out;

  // Pass through the call_fact
  if (call_fact && should_track(call_fact)) {
    out.insert(call_fact);
  }

  // Return value becomes a new fact
  if (!call->getType()->isVoidTy() && should_track(call)) {
    out.insert(call);
  }

  return out;
}

IDETypeState::FactSet
IDETypeState::call_to_return_flow(const llvm::CallBase *call,
                                  const llvm::Instruction *return_site,
                                  llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;

  bool preserve_fact = fact && should_track(fact);
  bool preserve_call_result = call && !call->getType()->isVoidTy() &&
                              should_track(call);

  if (m_description && call) {
    for (const llvm::Function *callee : callees) {
      if (!callee) {
        continue;
      }
      const llvm::StringRef callee_name = callee->getName();
      if (!m_description->is_api_function(callee_name)) {
        continue;
      }

      if (preserve_call_result &&
          m_description->is_factory_function(callee_name) &&
          fact == zero_fact()) {
        preserve_call_result = false;
      }

      if (preserve_fact) {
        for (int idx : m_description->get_consumer_param_indices(callee_name)) {
          if (idx < 0 || static_cast<unsigned>(idx) >= call->arg_size()) {
            continue;
          }
          const llvm::Value *actual =
              call->getArgOperand(static_cast<unsigned>(idx));
          if (!actual) {
            continue;
          }
          if (actual == fact ||
              (actual->getType()->isPointerTy() && fact->getType()->isPointerTy() &&
               may_alias_or_equal(actual, fact))) {
            preserve_fact = false;
            break;
          }
        }
      }
    }
  }

  // Keep the fact (it's not killed by the call)
  if (preserve_fact) {
    out.insert(fact);
  }

  // Call result is a new fact
  if (preserve_call_result) {
    out.insert(call);
  }

  return out;
}

IDETypeState::EdgeFunction
IDETypeState::normal_edge_function(const llvm::Instruction *stmt,
                                   const llvm::Instruction *succ,
                                   const Fact &src_fact, const Fact &tgt_fact) {
  (void)src_fact;
  (void)tgt_fact;

  if (!m_property) {
    return [](const Value &v) { return v; };
  }

  // Get transitions for this instruction
  auto transitions = m_property->get_transitions(stmt);

  if (transitions.empty()) {
    // Identity function
    return [](const Value &v) { return v; };
  }

  return make_transition_function(transitions);
}

IDETypeState::EdgeFunction
IDETypeState::call_edge_function(const llvm::CallBase *call,
                                 const llvm::Function *callee,
                                 const Fact &src_fact, const Fact &tgt_fact) {
  (void)callee;
  (void)src_fact;
  (void)tgt_fact;

  if (!m_property) {
    return [](const Value &v) { return v; };
  }

  // Get transitions for this call
  auto transitions = m_property->get_transitions(call);

  if (transitions.empty()) {
    // Identity function
    return [](const Value &v) { return v; };
  }

  return make_transition_function(transitions);
}

IDETypeState::EdgeFunction IDETypeState::return_edge_function(
    const llvm::CallBase *call, const llvm::Function *callee,
    const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const Fact &exit_fact,
    const Fact &ret_fact) {
  (void)call;
  (void)callee;
  (void)exit_inst;
  (void)return_site;
  (void)exit_fact;
  (void)ret_fact;

  // Identity function (transitions happen at call site)
  return [](const Value &v) { return v; };
}

IDETypeState::EdgeFunction IDETypeState::call_to_return_edge_function(
    const llvm::CallBase *call, const llvm::Instruction *return_site,
    llvm::ArrayRef<const llvm::Function *> callees, const Fact &src_fact,
    const Fact &tgt_fact) {
  (void)call;
  (void)return_site;
  (void)callees;
  (void)src_fact;
  (void)tgt_fact;

  // Identity function (local facts are preserved)
  return [](const Value &v) { return v; };
}

IDETypeState::FactSet IDETypeState::summary_flow(const llvm::CallBase *call,
                                                 const llvm::Function *callee,
                                                 const Fact &fact) {
  FactSet out;
  if (!m_description || !call || !callee) {
    return out;
  }

  const llvm::StringRef callee_name = callee->getName();

  if (fact == zero_fact() && m_description->is_factory_function(callee_name)) {
    if (!call->getType()->isVoidTy() && should_track(call)) {
      out.insert(call);
    }
    for (int idx : m_description->get_factory_param_indices(callee_name)) {
      if (idx < 0 || static_cast<unsigned>(idx) >= call->arg_size()) {
        continue;
      }
      const llvm::Value *actual = call->getArgOperand(static_cast<unsigned>(idx));
      if (should_track(actual)) {
        out.insert(actual);
      }
    }
  }

  if (fact && m_description->is_api_function(callee_name)) {
    for (int idx : m_description->get_consumer_param_indices(callee_name)) {
      if (idx < 0 || static_cast<unsigned>(idx) >= call->arg_size()) {
        continue;
      }
      const llvm::Value *actual = call->getArgOperand(static_cast<unsigned>(idx));
      if (!actual) {
        continue;
      }
      if (actual == fact) {
        out.insert(fact);
        break;
      }
      if (actual->getType()->isPointerTy() && fact->getType()->isPointerTy() &&
          may_alias_or_equal(actual, fact)) {
        out.insert(fact);
        break;
      }
    }
  }

  return out;
}

IDETypeState::EdgeFunction
IDETypeState::summary_edge_function(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const llvm::Instruction *return_site,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) {
  (void)return_site;
  if (!m_description || !call || !callee) {
    return [](const Value &v) { return v; };
  }

  const llvm::StringRef callee_name = callee->getName();
  const std::string token = callee_name.str();

  auto apply_api_transition = [description = m_description, token,
                               call](const Value &v) -> Value {
    if (v.is_top()) {
      return v;
    }
    const int current_state =
        v.is_bottom() ? description->uninitialized_state() : v.user_state();
    return Value(
        description->get_next_state(token, current_state, call));
  };

  if (src_fact == zero_fact() && m_description->is_factory_function(callee_name)) {
    bool applies = tgt_fact == call;
    if (!applies) {
      for (int idx : m_description->get_factory_param_indices(callee_name)) {
        if (idx < 0 || static_cast<unsigned>(idx) >= call->arg_size()) {
          continue;
        }
        applies |= call->getArgOperand(static_cast<unsigned>(idx)) == tgt_fact;
      }
    }
    if (applies) {
      return [description = m_description, token,
              call](const Value &) -> Value {
        return Value(description->get_next_state(
            token, description->uninitialized_state(), call));
      };
    }
  }

  if (src_fact == tgt_fact && tgt_fact &&
      m_description->is_api_function(callee_name)) {
    for (int idx : m_description->get_consumer_param_indices(callee_name)) {
      if (idx < 0 || static_cast<unsigned>(idx) >= call->arg_size()) {
        continue;
      }
      const llvm::Value *actual = call->getArgOperand(static_cast<unsigned>(idx));
      if (!actual) {
        continue;
      }
      if (actual == tgt_fact ||
          (actual->getType()->isPointerTy() && tgt_fact->getType()->isPointerTy() &&
           may_alias_or_equal(actual, tgt_fact))) {
        return apply_api_transition;
      }
    }
  }

  return [](const Value &v) { return v; };
}

bool IDETypeState::should_track(const llvm::Value *val) const {
  if (!val)
    return false;

  // Check if it's a global and we're tracking globals
  if (llvm::isa<llvm::GlobalVariable>(val)) {
    return m_track_globals;
  }

  // Check if it's heap-allocated (result of malloc, new, etc.) and we're
  // tracking heap
  if (m_track_heap) {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(val)) {
      if (const llvm::Function *callee = call->getCalledFunction()) {
        llvm::StringRef name = callee->getName();
        if (name == "malloc" || name == "calloc" || name == "realloc" ||
            name == "_Znwm" || name == "_Znam") { // new, new[]
          return true;
        }
      }
    }
  }

  // Check if the type matches tracked types.
  // NOTE: getPointerElementType() was deprecated in LLVM 14 and removed in
  // LLVM 17 (opaque-pointer migration).  We use
  // getNonOpaquePointerElementType() which is still available in LLVM 14.x (the
  // project's target version) but guarded so the code compiles cleanly when the
  // API is absent.
  std::unordered_set<std::string> tracked_types = m_tracked_types;
  if (tracked_types.empty() && m_description) {
    const std::string description_type =
        m_description->get_type_name_of_interest();
    if (!description_type.empty()) {
      tracked_types.insert(description_type);
    }
  }

  if (!tracked_types.empty() && val->getType()) {
    llvm::Type *ty = val->getType();
    if (auto *ptr_ty = llvm::dyn_cast<llvm::PointerType>(ty)) {
#if LLVM_VERSION_MAJOR < 17
      // LLVM 14–16: typed pointers still exist; use the non-opaque accessor.
      llvm::Type *elem_ty = ptr_ty->getNonOpaquePointerElementType();
      if (auto *struct_ty = llvm::dyn_cast<llvm::StructType>(elem_ty)) {
        if (struct_ty->hasName()) {
          std::string type_name = struct_ty->getName().str();
          if (tracked_types.count(type_name) > 0) {
            return true;
          }
        }
      }
#endif
      // LLVM 17+: all pointers are opaque; type-based filtering is not
      // possible without debug-info metadata.  Fall through to the
      // default pointer-type check below.
    }
  }

  // By default, track all pointer types
  return val->getType() && val->getType()->isPointerTy();
}

IDETypeState::EdgeFunction IDETypeState::make_transition_function(
    const std::vector<TypeStateProperty::Transition> &transitions) const {

  // Capture transitions by value
  return [transitions](const Value &v) -> Value {
    // If value is top or bottom, no transition
    if (v.is_special()) {
      return v;
    }

    // Apply first matching transition
    int current_state = v.user_state();
    for (const auto &trans : transitions) {
      if (trans.from_state == current_state) {
        return Value(trans.to_state);
      }
    }

    // No matching transition, return unchanged
    return v;
  };
}

// ============================================================================
// Predefined Typestate Properties
// ============================================================================

namespace predefined {

std::shared_ptr<TypeStateProperty> create_file_property() {
  auto prop = std::make_shared<TypeStateProperty>("File");

  // Define states
  int closed = prop->define_state("Closed");
  int opened = prop->define_state("Opened");
  int error = prop->define_state("Error", true);

  prop->set_initial_state(closed);

  // Transitions
  prop->add_transition_for_function("fopen", closed, opened);
  prop->add_transition_for_function("open", closed, opened);
  prop->add_transition_for_function("fclose", opened, closed);
  prop->add_transition_for_function("close", opened, closed);

  // Error transitions (operating on closed file)
  prop->add_transition_for_function("fread", closed, error);
  prop->add_transition_for_function("fwrite", closed, error);
  prop->add_transition_for_function("read", closed, error);
  prop->add_transition_for_function("write", closed, error);

  // Double close
  prop->add_transition_for_function("fclose", closed, error);
  prop->add_transition_for_function("close", closed, error);

  return prop;
}

std::shared_ptr<TypeStateProperty> create_lock_property() {
  auto prop = std::make_shared<TypeStateProperty>("Lock");

  // Define states
  int unlocked = prop->define_state("Unlocked");
  int locked = prop->define_state("Locked");
  int error = prop->define_state("Error", true);

  prop->set_initial_state(unlocked);

  // Transitions
  prop->add_transition_for_function("pthread_mutex_lock", unlocked, locked);
  prop->add_transition_for_function("pthread_mutex_unlock", locked, unlocked);

  // Error transitions
  prop->add_transition_for_function("pthread_mutex_lock", locked,
                                    error); // double lock
  prop->add_transition_for_function("pthread_mutex_unlock", unlocked,
                                    error); // unlock unlocked

  return prop;
}

std::shared_ptr<TypeStateProperty> create_memory_property() {
  auto prop = std::make_shared<TypeStateProperty>("Memory");

  // Define states
  int unallocated = prop->define_state("Unallocated");
  int allocated = prop->define_state("Allocated");
  int freed = prop->define_state("Freed");
  int error = prop->define_state("Error", true);

  prop->set_initial_state(unallocated);

  // Allocation
  prop->add_transition_for_function("malloc", unallocated, allocated);
  prop->add_transition_for_function("calloc", unallocated, allocated);
  prop->add_transition_for_function("realloc", unallocated, allocated);

  // Free
  prop->add_transition_for_function("free", allocated, freed);

  // Error transitions
  prop->add_transition_for_function("free", freed, error); // double free
  prop->add_transition_for_function("free", unallocated,
                                    error); // free unallocated

  return prop;
}

std::shared_ptr<TypeStateProperty> create_socket_property() {
  auto prop = std::make_shared<TypeStateProperty>("Socket");

  // Define states
  int uninitialized = prop->define_state("Uninitialized");
  int created = prop->define_state("Created");
  int bound = prop->define_state("Bound");
  int listening = prop->define_state("Listening");
  int connected = prop->define_state("Connected");
  int closed = prop->define_state("Closed");
  int error = prop->define_state("Error", true);

  prop->set_initial_state(uninitialized);

  // Normal flow
  prop->add_transition_for_function("socket", uninitialized, created);
  prop->add_transition_for_function("bind", created, bound);
  prop->add_transition_for_function("listen", bound, listening);
  prop->add_transition_for_function("accept", listening, connected);
  prop->add_transition_for_function("connect", created, connected);
  prop->add_transition_for_function("close", connected, closed);
  prop->add_transition_for_function("close", listening, closed);
  prop->add_transition_for_function("close", bound, closed);

  // Error transitions (operations in wrong state)
  prop->add_transition_for_function("bind", uninitialized, error);
  prop->add_transition_for_function("listen", created, error);
  prop->add_transition_for_function("accept", created, error);
  prop->add_transition_for_function("send", created, error);
  prop->add_transition_for_function("recv", created, error);

  return prop;
}

} // namespace predefined

} // namespace ifds
