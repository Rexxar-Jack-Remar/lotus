//===----------------------------------------------------------------------===//
//
// Bug query representation.
// Defines the numerical query hierarchy and the out-of-line operations of the
// query handle that Core symbolic state stores by value.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Checks/Query.h"

#include "SymbolicExecution/Core/AnalysisState.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"

#include <memory>

using namespace SymbolicExecution;

NumericalQueryPtr::NumericalQueryPtr(
    const std::shared_ptr<DirectNumericalQuery> &V)
    : Data(V) {}

NumericalQueryPtr::NumericalQueryPtr(
    const std::shared_ptr<IndirectNumericalQuery> &V)
    : Data(V) {}

bool NumericalQueryPtr::operator==(const NumericalQueryPtr &R) const {
  if (Data == R.Data) {
    return true;
  }

  auto Op1 = get(), Op2 = R.get();
  if (Op1->getKind() != Op2->getKind() || Op1->getBugTy() != Op2->getBugTy()) {
    return false;
  }

  if (isa<DirectNumericalQuery>(Op1)) {
    return *cast<DirectNumericalQuery>(Op1) == *cast<DirectNumericalQuery>(Op2);
  } else {
    return *cast<IndirectNumericalQuery>(Op1) ==
           *cast<IndirectNumericalQuery>(Op2);
  }
}

size_t NumericalQueryPtr::hash() const { return Data->hash(); }

void NumericalQueryPtr::translate(PathCondSolver *Solver) const {
  if (Data) {
    Data->translate(Solver);
  }
}

NumericalQueryPtr NumericalQuery::clone() const {
  if (QK == QK_DIRECT) {
    return std::make_shared<DirectNumericalQuery>(
        *cast<DirectNumericalQuery>(this));
  } else {
    return std::make_shared<IndirectNumericalQuery>(
        *cast<IndirectNumericalQuery>(this));
  }
}

void NumericalQuery::dumpDeps() const {
  llvm::errs() << "Deps: ";
  for (const auto &P : Deps) {
    llvm::errs() << P.first.getID() << "\n";
  }
}

NumericalQueryPtr DirectNumericalQuery::getBofMustErrQuery() {
  // The symbolic expression is resolved to a must error
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_BOF));
}

NumericalQueryPtr
DirectNumericalQuery::getBofTaintOnlyQuery(const GuardedProgramValSet &Deps) {
  // The symbolic expression could not be built, resort to taint on Deps
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_BOF, Deps));
}

NumericalQueryPtr
DirectNumericalQuery::getBofSymbolicQuery(const PropertyValuePtr &Qr,
                                          const GuardedProgramValSet &Deps) {
  // The error condition is encoded in the symbolic expr Qr +
  // we have extra taint deps in Deps
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_BOF, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getDbzMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_DBZ));
}

NumericalQueryPtr
DirectNumericalQuery::getDbzSymbolicQuery(const PropertyValuePtr &Qr,
                                          const GuardedProgramValSet &Deps) {
  // Qr == 0
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_DBZ, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getIntOverflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_INT_OVERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getIntOverflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_INT_OVERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getIntUnderflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_INT_UNDERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getIntUnderflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_INT_UNDERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getNullDerefMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_NULL_DEREF));
}

NumericalQueryPtr DirectNumericalQuery::getNullDerefSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr == 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_NULL_DEREF, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntOverflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntOverflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntUnderflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntUnderflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getShiftOverflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_SHIFT_OVERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getShiftOverflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0 (for negative shift or shift >= bitwidth)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_SHIFT_OVERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getArrayIndexOOBMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_ARRAY_INDEX_OOB));
}

NumericalQueryPtr DirectNumericalQuery::getArrayIndexOOBSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0 (for negative index or index >= array_size)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_ARRAY_INDEX_OOB, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getUninitializedReadQuery() {
  // Uninitialized read is always a potential error
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_UNINIT_READ));
}

NumericalQueryPtr DirectNumericalQuery::getUafMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_UAF));
}

NumericalQueryPtr
DirectNumericalQuery::getUafSymbolicQuery(const PropertyValuePtr &Qr,
                                          const GuardedProgramValSet &Deps) {
  // Qr represents the condition that pointer points to freed memory
  // For UAF, we check if Qr is true (pointer is freed)
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_UAF, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getDoubleFreeMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_DOUBLE_FREE));
}

NumericalQueryPtr DirectNumericalQuery::getDoubleFreeSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr represents the condition that pointer was already freed
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_DOUBLE_FREE, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getNegativeArrayIndexMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX));
}

NumericalQueryPtr DirectNumericalQuery::getNegativeArrayIndexSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr < 0 (negative index)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getIntTruncationMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_INT_TRUNCATION));
}

NumericalQueryPtr DirectNumericalQuery::getIntTruncationSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0 (value exceeds destination type range)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_INT_TRUNCATION, Qr, false, Deps));
}

void DirectNumericalQuery::dump() const {
  llvm::errs() << "Direct Query:";
  if (mustSat()) {
    llvm::errs() << "must err!\n";
  } else if (!Q) {
    llvm::errs() << "null query expr!\n";
  } else {
    Q->dump();
  }
}

SMTExprVec DirectNumericalQuery::toSMT(PathCondSolver &Solver) const {
  SMTExprVec RetVec = Solver.createEmptySMTExprVec();
  if (!Q) {
    return RetVec;
  }

  SMTExpr ResExpr = Solver.buildExprForVal(Q.get());
  SMTExpr CstZero = Solver.buildBitVecVal(0, ResExpr.getBitVecSize());
  if (Eq) {
    RetVec.push_back(ResExpr == CstZero);
  } else {
    RetVec.push_back(ResExpr.basic_sgt(CstZero));
  }

  return RetVec;
}

NumericalQueryPtr IndirectNumericalQuery::getBofQuery(
    const ProgramValuePtr &BasePtr, const PropertyValuePtr &Offset,
    const PropertyValuePtr &AccSize, const GuardedProgramValSet &Deps) {
  return std::shared_ptr<IndirectNumericalQuery>(new IndirectNumericalQuery(
      AnalysisState::BUG_TY_BOF, BasePtr, Offset, AccSize, Deps));
}

void IndirectNumericalQuery::dump() const {
  llvm::errs() << "Indirect query:\n";
  llvm::errs() << "Base:" << BasePtr.getID() << ", Offset:";
  Offset->dump();
  llvm::errs() << "Acc Size:";
  AccSize->dump();
  dumpDeps();
}
