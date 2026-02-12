/*
 * Available Expressions Analysis (Backward)
 *
 * Demonstrates backward dataflow analysis in the Lotus framework
 *
 * Author: rainoftime
 */
#include "Dataflow/Mono/Analyses/Intra/AvailableExpressions.h"
#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

#include "llvm/IR/Instructions.h"

#include <unordered_set>

using namespace llvm;

namespace mono {

namespace {

/**
 * @brief Simple expression representation for dataflow analysis
 *
 * We represent expressions as "opcode + operands" pairs. This is a simplified
 * version suitable for demonstration. A production implementation would use
 * value numbering or hash consing.
 */
struct Expression {
  unsigned Opcode;
  SmallVector<Value *, 4> Operands;

  Expression(unsigned Opcode, ArrayRef<Value *> Ops)
      : Opcode(Opcode), Operands(Ops.begin(), Ops.end()) {}

  bool operator==(const Expression &Other) const {
    return Opcode == Other.Opcode && Operands == Other.Operands;
  }

  bool operator<(const Expression &Other) const {
    if (Opcode != Other.Opcode)
      return Opcode < Other.Opcode;
    return Operands < Other.Operands;
  }

  /// Returns true if this expression uses the given value
  bool usesValue(Value *V) const {
    return llvm::find(Operands, V) != Operands.end();
  }
};

/**
 * @brief Extract expressions computed by an instruction
 *
 * Currently handles binary operators, casts, and comparisons.
 */
static std::vector<Expression> getComputedExpressions(Instruction *Inst) {
  std::vector<Expression> Exprs;

  if (auto *BinOp = dyn_cast<BinaryOperator>(Inst)) {
    SmallVector<Value *, 2> Ops{BinOp->getOperand(0), BinOp->getOperand(1)};
    Exprs.emplace_back(BinOp->getOpcode(), Ops);
  } else if (auto *Cast = dyn_cast<CastInst>(Inst)) {
    SmallVector<Value *, 1> Ops{Cast->getOperand(0)};
    Exprs.emplace_back(Cast->getOpcode(), Ops);
  } else if (auto *Cmp = dyn_cast<CmpInst>(Inst)) {
    SmallVector<Value *, 2> Ops{Cmp->getOperand(0), Cmp->getOperand(1)};
    Exprs.emplace_back(Cmp->getOpcode(), Ops);
  }
  // Could extend to handle GEPs, calls to pure functions, etc.

  return Exprs;
}

/**
 * @brief Find expressions that are killed (invalidated) by an instruction
 *
 * An expression is killed if the instruction redefines one of its operands.
 * In SSA form, this is straightforward: we kill all expressions that use
 * the value being defined.
 */
static std::set<Expression>
getKilledExpressions(Instruction *Inst,
                     const std::set<Expression> &AllExprs) {
  std::set<Expression> Killed;

  // In SSA form, we kill expressions that use the value being redefined
  if (!Inst->getType()->isVoidTy()) {
    for (const auto &Expr : AllExprs) {
      if (Expr.usesValue(Inst)) {
        Killed.insert(Expr);
      }
    }
  }

  return Killed;
}

// ============================================================================
// Available Expressions Problem (Backward)
// ============================================================================

// Note: Expression is a custom type, so we keep std::set<Expression>
// For standard LLVM types (Value*, Instruction*), use LLVMMonoAnalysisDomain<ElementType>
struct AvailableExprDomain : LLVMMonoAnalysisDomain<std::set<Expression>> {};

class AvailableExprProblem : public IntraMonoProblem<AvailableExprDomain> {
public:
  explicit AvailableExprProblem(Function *F)
      : IntraMonoProblem<AvailableExprDomain>({F}) {
    // Collect all expressions in the function
    for (auto &BB : *F) {
      for (auto &Inst : BB) {
        auto Exprs = getComputedExpressions(&Inst);
        AllExpressions.insert(Exprs.begin(), Exprs.end());
      }
    }
  }

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Backward;
  }

  std::set<Expression> normalFlow(Instruction *Inst,
                                  const std::set<Expression> &In) override {
    std::set<Expression> Out = In;

    // KILL: Remove expressions that use values redefined by this instruction
    auto Killed = getKilledExpressions(Inst, Out);
    for (const auto &Expr : Killed) {
      Out.erase(Expr);
    }

    // GEN: Add expressions computed by this instruction
    auto Generated = getComputedExpressions(Inst);
    Out.insert(Generated.begin(), Generated.end());

    return Out;
  }

  std::set<Expression> merge(const std::set<Expression> &Lhs,
                             const std::set<Expression> &Rhs) override {
    // Intersection: an expression is available only if available on ALL paths
    std::set<Expression> Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  bool equal_to(const std::set<Expression> &Lhs,
                const std::set<Expression> &Rhs) override {
    return Lhs == Rhs;
  }

  std::set<Expression> allTop() override {
    // Top = universal set (all expressions available)
    // Used as initial value for backward analysis
    return AllExpressions;
  }

  std::unordered_map<Instruction *, std::set<Expression>>
  initialSeeds() override {
    std::unordered_map<Instruction *, std::set<Expression>> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr) {
      return Seeds;
    }

    // For backward analysis, seed all exit points (return instructions)
    for (auto &BB : *F) {
      if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
        // Exit point: no expressions are available (empty set)
        Seeds[Ret] = {};
      }
    }

    return Seeds;
  }

private:
  std::set<Expression> AllExpressions;
};

} // namespace

// ============================================================================
// Public API
// ============================================================================

std::unique_ptr<DataFlowResult>
runAvailableExpressionsAnalysis(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return nullptr;
  }

  AvailableExprProblem Problem(F);
  IntraMonoSolver<AvailableExprDomain> Solver(Problem);
  Solver.solve();

  // Note: For expression analysis, we'd normally return a specialized result
  // type. Here we use DataFlowResult with Value* for demonstration.
  // A production implementation would use a custom result container.

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      auto *I = &Inst;

      // For backward analysis, IN is what flows backward from successors
      // OUT is what flows backward to predecessors
      // Note: The solver's "In" is the analysis IN, "Out" is analysis OUT

      const auto &AnalysisIn = Solver.getInResultsAt(I);
      const auto &AnalysisOut = Solver.getOutResultsAt(I);

      // Store the count of available expressions (for demonstration)
      // A real implementation would store the actual expressions
      if (!AnalysisIn.empty()) {
        // Mark that expressions are available at this point
        // (Using instruction count as a proxy for expression count)
        for (size_t Idx = 0; Idx < AnalysisIn.size(); ++Idx) {
          Result->IN(I).insert(I); // Placeholder representation
        }
      }

      auto Generated = getComputedExpressions(I);
      for (const auto &Expr : Generated) {
        // Represent generated expressions (simplified)
        Result->GEN(I).insert(I);
      }
    }
  }

  return Result;
}

} // namespace mono
