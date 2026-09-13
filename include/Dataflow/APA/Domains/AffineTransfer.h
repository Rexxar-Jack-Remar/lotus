#pragma once

#include "Dataflow/APA/Domains/AffineRelationDomain.h"

namespace llvm {
class Instruction;
class CallBase;
class Function;
} // namespace llvm

namespace elimination {

struct AffineEdgeTransfer {
  llvm::Instruction *inst = nullptr;
  llvm::Instruction *succ = nullptr;
  bool operator==(const AffineEdgeTransfer &other) const {
    return inst == other.inst && succ == other.succ;
  }
};

/// LLVM transfer construction for the affine relation domain.
class AffineTransferBuilder {
public:
  using Relation = AffineRelationDomain::value_type;
  Relation instructionTransfer(llvm::Instruction &inst) const;
  Relation edgeTransferRelation(const llvm::Instruction &term,
                                const llvm::Instruction &succ) const;
  Relation phiTransferForEdge(const llvm::Instruction &term,
                              const llvm::Instruction &succ) const;
  Relation callEntryTransfer(const llvm::CallBase &call,
                             const llvm::Function &callee) const;
  Relation callReturnTransfer(const llvm::CallBase &call,
                              const llvm::Function &callee,
                              llvm::Instruction *exit) const;
  static bool instructionHasEffect(const llvm::Instruction &inst);
  static bool edgeHasCondition(const llvm::Instruction &inst);
  static bool edgeEntersPhi(const llvm::Instruction &pred,
                            const llvm::Instruction &succ);
  static bool isTrackedScalar(const llvm::Value *value);
  static bool isAssumeLikeCall(const llvm::CallBase &call);
};

} // namespace elimination
