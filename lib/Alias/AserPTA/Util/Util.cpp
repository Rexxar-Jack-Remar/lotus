/**
 * @file Util.cpp
 * @brief Utility functions for AserPTA.
 *
 * Provides utility functions for pretty printing functions and checking
 * call compatibility for indirect calls.
 *
 * @author peiming
 */
#include "Alias/AserPTA/Util/Util.h"

#include "Alias/AserPTA/PointerAnalysis/Program/CallSite.h"

using namespace llvm;
using namespace aser;

/**
 * @brief Pretty print a function signature.
 *
 * Prints the function's return type, name, and parameter list in a readable
 * format, including varargs notation if applicable.
 *
 * @param func The function to print
 * @param os The output stream
 */
void aser::prettyFunctionPrinter(const Function *func, raw_ostream &os) {
  os << *func->getReturnType() << " @" << func->getName() << "(";
  auto *funcType = func->getFunctionType();
  for (unsigned I = 0, E = funcType->getNumParams(); I != E; ++I) {
    if (I)
      os << ", ";
    os << *funcType->getParamType(I);
  }
  if (funcType->isVarArg()) {
    if (funcType->getNumParams())
      os << ", ";
    os << "..."; // Output varargs portion of signature!
  }
  os << ")";
}

/**
 * @brief Check if an indirect call is compatible with a target function.
 *
 * Performs type compatibility checking between an indirect call site and a
 * potential target function. Handles varargs functions and ensures parameter
 * types match. Note: simple type checking fails for cases like
 * `call void (...) %ptr()`.
 *
 * @param indirectCall The indirect call instruction
 * @param target The potential target function
 * @return true if the call is compatible with the target, false otherwise
 */
bool aser::isCompatibleCall(const llvm::Instruction *indirectCall,
                            const llvm::Function *target) {
  aser::CallSite CS(indirectCall);
  assert(CS.isIndirectCall());

  // fast path, the same type
  if (CS.getCalledValue()->getType() == target->getType()) {
    return true;
  }

  if (CS.getType() != target->getReturnType()) {
    return false;
  }

  if (CS.getNumArgOperands() != target->arg_size() && !target->isVarArg()) {
    // two non-vararg function should at have same number of parameters
    return false;
  }

  if (target->isVarArg() && target->arg_size() > CS.getNumArgOperands()) {
    // calling a varargs function, the callsite should offer at least the
    // same number of parameters required by var-args
    return false;
  }

  // LLVM IR is strongly typed, so ensure every actually argument is of the
  // same type as the formal arguments.
  const auto *fit = CS.arg_begin();
  for (const Argument &arg : target->args()) {
    const Value *param = *fit;
    if (param->getType()->isPointerTy() != arg.getType()->isPointerTy()) {
      return false;
    }
    fit++;
  }

  return true;
}
