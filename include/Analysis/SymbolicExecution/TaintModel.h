/** @file TaintModel.h @brief Taint propagation model for symbolic-execution-based taint analysis. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_TAINTMODEL_H
#define ANALYSIS_SYMBOLICEXECUTION_TAINTMODEL_H

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

/// Declarative taint specification consulted by the symbolic execution engine.
///
/// Taint tracking is one input to bug finding in this subsystem, especially for
/// bug classes that care about untrusted data reaching sensitive operations.
/// `TaintModel` records which library functions introduce taint, how taint
/// flows between arguments and results, and which APIs should be treated as
/// sinks by higher-level checkers.
class TaintModel {
public:
  TaintModel();

  /// Populates `DstVect` with values that should inherit taint from `Arg` at
  /// the given call site according to the library transfer rules.
  void getTransferDstVect(const CallBase *CS, Value *Arg,
                          std::vector<Value *> &DstVect) const;

  /// Returns true when the function's return value should be treated as a taint
  /// source, such as an environment read or input-producing library call.
  bool isFunctionRetAsSource(const Function *func) const;

  /// Returns true when one or more arguments of the function should be treated
  /// as direct taint sources.
  bool isFunctionArgAsSource(const Function *func) const;

  /// Returns the source-argument specification for `func`, if one exists.
  /// Each integer identifies a formal parameter index recognized as a source.
  const std::vector<int> *getTaintSourceArguments(Function *func) const;

private:
  // Function names mapped to source-argument positions.
  std::unordered_map<std::string, std::vector<int>> ArgAsSourceFunctions;

  // Functions whose return values should seed taint.
  std::set<std::string> RetAsSourceFunctions;

  // Function names mapped to transfer patterns from source argument to other
  // arguments or results. A multimap is used because one API can have multiple
  // transfer schemas.
  std::multimap<std::string, std::vector<int>> DataTransferFunctions;

  // Sensitive functions tracked by name so bug checkers can recognize sinks.
  std::unordered_map<std::string, std::vector<int>> SinkFunctions;
};

#endif
