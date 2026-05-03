/// @file ConditionRef.h
/// @brief Lightweight metadata handle carrying provenance information for
///        path-condition guards.
///
/// ConditionRef bridges two canonical guard representations used throughout
/// the guarded value-flow graph:
///   - **StructuralGuard** — records the LLVM terminator instruction
///     (branch / switch) and the specific successor edge that materialises
///     the guard.  Used during structural graph construction to attach
///     source-level control-flow information to region nodes.
///   - **SemanticPathCond** — wraps a `path_cond_t` pointer from the
///     constraint system, which may carry imported callee conditions from
///     inter-procedural analysis.
///
/// Downstream passes can inspect `getKind()` to decide whether to use a
/// structural encoding (backed by LLVM IR) or a solver-backed encoding
/// (backed by path_cond_t).

#pragma once

#include "Alias/InclusionBased/LotusAA/MemoryModel/Types.h"
#include "IR/GSA/GSA.h"

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>

namespace lotus {
namespace gvfg {

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::path_cond_t;
using llvm::Value;

/// Immutable reference to a guard condition carried on a GVFG edge or region.
///
/// Two variants:
///   - StructuralGuard  — (branch/switch, successor, condition value)
///   - SemanticPathCond — opaque reference into the constraint system
class ConditionRef {
public:
  enum class Kind {
    None,              ///< No condition attached
    StructuralGuard,   ///< Guard from an LLVM terminator edge
    SemanticPathCond,  ///< Guard from the path_cond_t constraint system
  };

  ConditionRef() = default;

  /// Returns a null ConditionRef (no condition).
  static ConditionRef none() { return ConditionRef(); }

  /// Build from a branch or switch guard edge.
  /// @param kind          one of BranchTrue, BranchFalse, SwitchCase, SwitchDefault
  /// @param control_block the block containing the terminator
  /// @param successor     the specific successor edge this guard controls
  /// @param condition     the branch condition value or switch condition value
  /// @param case_value    for SwitchCase, the ConstantInt case value; else nullptr
  static ConditionRef fromGuard(gsa::GuardKind kind, BasicBlock *control_block,
                                BasicBlock *successor, Value *condition,
                                ConstantInt *case_value = nullptr) {
    ConditionRef ref;
    ref.kind_ = Kind::StructuralGuard;
    ref.guard_kind_ = kind;
    ref.control_block_ = control_block;
    ref.successor_ = successor;
    ref.condition_ = condition;
    ref.case_value_ = case_value;
    return ref;
  }

  /// Build from an existing path_cond_t in the constraint system.
  static ConditionRef fromPathCond(path_cond_t path_cond) {
    ConditionRef ref;
    ref.kind_ = Kind::SemanticPathCond;
    ref.path_cond_ = path_cond;
    return ref;
  }

  Kind getKind() const { return kind_; }
  bool isValid() const { return kind_ != Kind::None; }

  gsa::GuardKind getGuardKind() const { return guard_kind_; }
  BasicBlock *getControlBlock() const { return control_block_; }
  BasicBlock *getSuccessor() const { return successor_; }
  Value *getCondition() const { return condition_; }
  ConstantInt *getCaseValue() const { return case_value_; }
  path_cond_t getPathCond() const { return path_cond_; }

  /// Human-readable string for debugging.
  std::string render() const;

  bool operator==(const ConditionRef &other) const {
    return kind_ == other.kind_ && guard_kind_ == other.guard_kind_ &&
           control_block_ == other.control_block_ &&
           successor_ == other.successor_ && condition_ == other.condition_ &&
           case_value_ == other.case_value_ && path_cond_ == other.path_cond_;
  }

  bool operator!=(const ConditionRef &other) const { return !(*this == other); }

private:
  Kind kind_{Kind::None};
  gsa::GuardKind guard_kind_{gsa::GuardKind::Opaque};
  BasicBlock *control_block_{nullptr};
  BasicBlock *successor_{nullptr};
  Value *condition_{nullptr};
  ConstantInt *case_value_{nullptr};
  path_cond_t path_cond_{nullptr};
};

} // namespace gvfg
} // namespace lotus
