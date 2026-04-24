/// @file MemObject.cpp
/// @brief Memory object abstraction - the core memory model for LotusAA
///
/// This file implements `MemObject`, the fundamental abstraction representing
/// memory locations in pointer analysis. Each memory object represents a set
/// of concrete memory locations that are abstracted together.
///
/// **Object Types:**
///
/// 1. **Concrete Objects** (`MemObject::CONCRETE`):
///    - Allocation-site sensitive: one object per allocation site
///    - Examples: alloca instructions, malloc calls, escaped objects
///    - Known allocation point in analyzed code
///
/// 2. **Symbolic Objects** (`SymbolicMemObject`, kind=SYMBOLIC):
///    - Represent memory from outside function scope
///    - Examples: function arguments, global variables
///    - May create **pseudo-arguments** for field accesses (e.g., **p)
///    - Used for inter-procedural summary generation
///
/// 3. **Special Objects**:
///    - `NullObj`: Singleton representing null pointer
///    - `UnknownObj`: Top element (may point anywhere - conservative)
///
/// **Field-Sensitivity:**
/// Each MemObject contains a map of `ObjectLocator`s, one per field offset:
/// ```
/// MemObject [base]
///   ├── Locator @offset 0 → stores values at base
///   ├── Locator @offset 8 → stores values at field +8
///   └── Locator @offset 16 → stores values at field +16
/// ```
///
/// **Pseudo-Arguments (Symbolic Objects Only):**
/// When a symbolic object's field is accessed but not defined locally,
/// a pseudo-argument is created to represent the external value:
/// ```c
/// void f(struct S *s) {
///   return s->field;  // Creates pseudo-arg for s->field
/// }
/// // Summary: Input = {s, s->field (pseudo-arg)}
/// ```
///
/// **Object Identity:**
/// - Each object has unique `obj_index` within its function
/// - Allocation site (`alloc_site`) provides source-level identity
/// - Used for points-to set comparison and aliasing queries
///
/// **Memory Management:**
/// - Objects owned by PTGraph, deleted with it
/// - Locators owned by MemObject, deleted in destructor
/// - Special singletons (NullObj, UnknownObj) deleted by LotusAA pass
///
/// @see ObjectLocator for field-level value tracking
/// @see SymbolicMemObject for symbolic object specialization
/// @see PTGraph for object ownership and creation

#include "Alias/InclusionBased/LotusAA/MemoryModel/MemObject.h"

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/MemoryModel/PointsToGraph.h"
#include "Alias/InclusionBased/LotusAA/Support/Config.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

//===----------------------------------------------------------------------===//
// Static Member Initialization
//===----------------------------------------------------------------------===//

MemObject *MemObject::NullObj = nullptr;
MemObject *MemObject::UnknownObj = nullptr;

LLVMValueIndex *LLVMValueIndex::Instance = nullptr;

//===----------------------------------------------------------------------===//
// MemObject Implementation
//===----------------------------------------------------------------------===//

MemObject::MemObject(Value *alloc_site, PTGraph *pt_graph, ObjKind obj_kind)
    : alloc_site(alloc_site), pt_graph(pt_graph), obj_kind(obj_kind),
      pt_index(pt_graph ? pt_graph->pt_index : -1),
      obj_index(pt_graph ? pt_graph->obj_index++ : -1), loc_index(0) {}

MemObject::~MemObject() {
  for (auto &it : locators) {
    delete it.second;
  }
}

void MemObject::dump() {
  outs() << "Object: " << getName() << "\n";
  if (alloc_site) {
    outs() << "  Alloc: ";
    alloc_site->print(outs());
    outs() << "\n";
  }
  for (auto &loc_pair : locators) {
    outs() << "  Offset " << loc_pair.first << ":\n";
    loc_pair.second->dump();
  }
}

void MemObject::clear() {
  for (auto &it : locators) {
    delete it.second;
  }
  lotus_clear_hash(&locators);
  lotus_clear_hash(&updated_offset);
  lotus_clear_hash(&pointer_offset);
}

std::string MemObject::getName() {
  std::string name;
  raw_string_ostream ss(name);

  if (alloc_site) {
    if (alloc_site->hasName())
      ss << alloc_site->getName();
    else
      ss << "obj_" << (void *)alloc_site;

    if (isa<AllocaInst>(alloc_site))
      ss << "(alloca)";
    else if (isa<CallBase>(alloc_site))
      ss << "(malloc)";
    else if (isa<GlobalVariable>(alloc_site))
      ss << "(global)";
  } else {
    ss << (isNull() ? "NullObj" : "UnknownObj");
  }

  ss.flush();
  return name;
}

Type *MemObject::guessType() {
  return alloc_site ? alloc_site->getType() : nullptr;
}

ObjectLocator *MemObject::findLocator(int64_t offset, bool is_create) {
  auto it = locators.find(offset);
  if (it != locators.end())
    return it->second;

  if (is_create) {
    ObjectLocator *loc = new ObjectLocator(this, offset);
    locators[offset] = loc;
    return loc;
  }

  return nullptr;
}

bool MemObject::isReallyAllocated() {
  if (!alloc_site)
    return false;

  // Stack allocation
  if (isa<AllocaInst>(alloc_site))
    return true;

  // Heap allocation (check via spec manager if available)
  if (CallBase *call = dyn_cast<CallBase>(alloc_site)) {
    if (Function *F = call->getCalledFunction()) {
      // Get LotusAA instance through PTGraph to access spec manager
      if (pt_graph && pt_graph->lotus_aa) {
        return pt_graph->lotus_aa->getSpecManager().isAllocator(F);
      }

      // Fallback: simple name-based heuristic (if no PTGraph/LotusAA available)
      StringRef name = F->getName();
      if (name.contains("malloc") || name.contains("alloc") || name == "new" ||
          name == "calloc" || name == "realloc")
        return true;
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// SymbolicMemObject Implementation
//===----------------------------------------------------------------------===//
SymbolicMemObject::~SymbolicMemObject() {
  for (auto &it : pseudo_args) {
    delete it.second;
  }
}

std::string SymbolicMemObject::getName() {
  std::string name;
  raw_string_ostream ss(name);

  int id = getPTG()->getObjectID(this);
  ss << "Sym_" << id;

  if (alloc_site && !isa<Argument>(alloc_site)) {
    ss << ": " << MemObject::getName();
  }

  ss.flush();
  return name;
}

Argument *SymbolicMemObject::findCreatePseudoArg(ObjectLocator *locator,
                                                 Type *arg_type) {
  auto it = pseudo_args.find(locator);
  if (it != pseudo_args.end())
    return it->second;

  if (arg_type) {
    // LLVM Arguments must have first-class types or be void
    // If we have a non-first-class type (struct, array), use a pointer to it
    Type *actual_type = arg_type;
    if (!arg_type->isFirstClassType() && !arg_type->isVoidTy()) {
      actual_type = arg_type->getPointerTo();
    }

    // LLVM doesn't allow naming void-typed values, so use empty name for void
    // types
    std::string name;
    if (!actual_type->isVoidTy()) {
      raw_string_ostream ss(name);
      ss << *locator;
      ss.flush();
    }

    Argument *arg = new Argument(actual_type, name);
    pseudo_args[locator] = arg;
    return arg;
  }

  return nullptr;
}
